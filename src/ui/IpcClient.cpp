// coding: utf-8
// =============================================================================
// IpcClient.cpp
//
// Yuli Vault UI 进程 IPC 客户端实现。基于 Windows 命名管道：
//   - connect_to_service: 用 CreateFileW Open \\.\pipe\YuliVaultService
//   - read_all / write_all: 用 overlapped I/O + WaitForSingleObject 实现 10 秒超时
//   - 重试: connect 失败时重试 3 次，间隔 500ms（QThread::msleep）
//   - RAII: ScopedHandle 包装事件句柄；管道句柄在析构/disconnect Medium释放
//   - 异步: send_request_async 模板用 QtConcurrent::run 在线程池上跑同步
//     send_request；pipe_mutex_ 串行化所有请求避免管道帧错乱
// =============================================================================
#include "IpcClient.h"

#include <QThread>
#include <QString>

// Windows 头：放在 Qt 之后以避免 min/max 宏污染 Qt 模板
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace yuli::vault::ui {

namespace {

/// 单次请求超时（毫秒）。超时返回 IpcError。
constexpr DWORD kRequestTimeoutMs = 10000;

/// connect 重试次数（不含首次尝试）。
constexpr int kConnectRetryCount = 3;

/// 每次 connect 重试前等待的毫秒数。
constexpr int kConnectRetryIntervalMs = 500;

/// RAII 包装 Windows HANDLE。释放时调用 CloseHandle。
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle_(h) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool valid() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE tmp = handle_;
        handle_ = nullptr;
        return tmp;
    }
    void reset(HANDLE h = nullptr) {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = h;
    }

private:
    HANDLE handle_ = nullptr;
};

/// UTF-8 字符串 → UTF-16 wstring，用于 Win32 API 调用。
std::wstring utf8_to_wide(std::string_view sv) {
    if (sv.empty()) return std::wstring();
    const int required = ::MultiByteToWideChar(
        CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (required <= 0) return std::wstring();
    std::wstring ws(static_cast<size_t>(required), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), ws.data(), required);
    return ws;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与析构
// ---------------------------------------------------------------------------

IpcClient::IpcClient(QObject* parent) : QObject(parent) {}

IpcClient::~IpcClient() {
    disconnect();
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

bool IpcClient::connect_to_service(std::string_view pipe_name) {
    if (connected_) {
        return true;
    }

    const std::wstring wname = utf8_to_wide(pipe_name);

    // 总尝试次数 = 1 次初始 + kConnectRetryCount 次重试
    for (int attempt = 0; attempt <= kConnectRetryCount; ++attempt) {
        if (attempt > 0) {
            QThread::msleep(static_cast<unsigned long>(kConnectRetryIntervalMs));
        }

        HANDLE raw = ::CreateFileW(
            wname.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,                          // 独占访问
            nullptr,                    // 默认Security属性
            OPEN_EXISTING,               // 管道必须已由 service 创建
            FILE_FLAG_OVERLAPPED,        // Enable overlapped 以支持超时
            nullptr);

        if (raw == INVALID_HANDLE_VALUE) {
            // 此轮失败，下一轮重试
            continue;
        }

        pipe_handle_ = raw;
        connected_ = true;
        request_id_ = 0;
        return true;
    }
    return false;
}

void IpcClient::disconnect() {
    if (pipe_handle_ != nullptr && pipe_handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(static_cast<HANDLE>(pipe_handle_));
        pipe_handle_ = nullptr;
    }
    if (connected_) {
        connected_ = false;
        emit disconnected();
    }
}

bool IpcClient::is_connected() const {
    return connected_;
}

// ---------------------------------------------------------------------------
// 内部 I/O 辅助
// ---------------------------------------------------------------------------

void IpcClient::handle_disconnect(const std::string& reason) {
    if (!connected_) {
        return;  // 已断开，避免重复触发信号
    }

    if (pipe_handle_ != nullptr && pipe_handle_ != INVALID_HANDLE_VALUE) {
        ::CancelIo(static_cast<HANDLE>(pipe_handle_));
        ::CloseHandle(static_cast<HANDLE>(pipe_handle_));
        pipe_handle_ = nullptr;
    }
    connected_ = false;

    emit error_occurred(QString::fromStdString(reason));
    emit disconnected();
}

bool IpcClient::write_all(const void* data, size_t size) {
    if (!connected_ || pipe_handle_ == nullptr) return false;
    HANDLE h = static_cast<HANDLE>(pipe_handle_);

    const auto* p = static_cast<const std::byte*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return false;

        OVERLAPPED ov{};
        ov.hEvent = event.get();

        const DWORD to_write = static_cast<DWORD>(
            std::min(remaining, static_cast<size_t>(0x7FFFFFFFu)));
        DWORD written = 0;
        BOOL ok = ::WriteFile(h, p, to_write, &written, &ov);

        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;  // 管道已断或参数错误
            }
            // 等待 I/O 完成（带超时）
            const DWORD wait_result =
                ::WaitForSingleObject(event.get(), kRequestTimeoutMs);
            if (wait_result != WAIT_OBJECT_0) {
                // 超时或失败：Cancel未决 I/O，等待Cancel完成
                ::CancelIo(h);
                DWORD dummy = 0;
                ::GetOverlappedResult(h, &ov, &dummy, TRUE);
                return false;
            }
            if (!::GetOverlappedResult(h, &ov, &written, FALSE)) {
                return false;
            }
        }

        if (written == 0) return false;
        p += written;
        remaining -= written;
    }
    return true;
}

bool IpcClient::read_all(void* out, size_t size) {
    if (!connected_ || pipe_handle_ == nullptr) return false;
    HANDLE h = static_cast<HANDLE>(pipe_handle_);

    auto* p = static_cast<std::byte*>(out);
    size_t remaining = size;

    while (remaining > 0) {
        ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return false;

        OVERLAPPED ov{};
        ov.hEvent = event.get();

        const DWORD to_read = static_cast<DWORD>(
            std::min(remaining, static_cast<size_t>(0x7FFFFFFFu)));
        DWORD read = 0;
        BOOL ok = ::ReadFile(h, p, to_read, &read, &ov);

        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;  // 管道已断（ERROR_BROKEN_PIPE 等）
            }
            const DWORD wait_result =
                ::WaitForSingleObject(event.get(), kRequestTimeoutMs);
            if (wait_result != WAIT_OBJECT_0) {
                ::CancelIo(h);
                DWORD dummy = 0;
                ::GetOverlappedResult(h, &ov, &dummy, TRUE);
                return false;
            }
            if (!::GetOverlappedResult(h, &ov, &read, FALSE)) {
                return false;
            }
        }

        if (read == 0) return false;
        p += read;
        remaining -= read;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IPC 命令实现（实例化 send_request 模板）
// ---------------------------------------------------------------------------

core::Result<protocol::PingResponse> IpcClient::ping() {
    return send_request<protocol::PingRequest, protocol::PingResponse>(
        protocol::CommandId::Ping, protocol::PingRequest{});
}

core::Result<protocol::UnlockResponse> IpcClient::unlock(const std::string& password) {
    protocol::UnlockRequest req;
    req.password = password;
    return send_request<protocol::UnlockRequest, protocol::UnlockResponse>(
        protocol::CommandId::Unlock, req);
}

core::Result<protocol::LockResponse> IpcClient::lock() {
    return send_request<protocol::LockRequest, protocol::LockResponse>(
        protocol::CommandId::Lock, protocol::LockRequest{});
}

core::Result<protocol::EnableProgramPasswordResponse> IpcClient::enable_program_password(const std::string& password) {
    protocol::EnableProgramPasswordRequest req;
    req.password = password;
    return send_request<protocol::EnableProgramPasswordRequest, protocol::EnableProgramPasswordResponse>(
        protocol::CommandId::EnableProgramPassword, req);
}

core::Result<protocol::DisableProgramPasswordResponse> IpcClient::disable_program_password(const std::string& password) {
    protocol::DisableProgramPasswordRequest req;
    req.password = password;
    return send_request<protocol::DisableProgramPasswordRequest, protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, req);
}

core::Result<protocol::ChangeProgramPasswordResponse> IpcClient::change_program_password(const std::string& old_password, const std::string& new_password) {
    protocol::ChangeProgramPasswordRequest req;
    req.old_password = old_password;
    req.new_password = new_password;
    return send_request<protocol::ChangeProgramPasswordRequest, protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, req);
}

core::Result<protocol::GetVaultStatusResponse> IpcClient::get_vault_status() {
    return send_request<protocol::GetVaultStatusRequest, protocol::GetVaultStatusResponse>(
        protocol::CommandId::GetVaultStatus, protocol::GetVaultStatusRequest{});
}

core::Result<protocol::AddEntryResponse> IpcClient::add_entry(const core::PasswordEntry& entry) {
    protocol::AddEntryRequest req;
    req.entry = entry;
    return send_request<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, req);
}

core::Result<protocol::UpdateEntryResponse> IpcClient::update_entry(const core::PasswordEntry& entry) {
    protocol::UpdateEntryRequest req;
    req.entry = entry;
    return send_request<protocol::UpdateEntryRequest, protocol::UpdateEntryResponse>(
        protocol::CommandId::UpdateEntry, req);
}

core::Result<protocol::RemoveEntryResponse> IpcClient::remove_entry(int64_t id) {
    protocol::RemoveEntryRequest req;
    req.id = id;
    return send_request<protocol::RemoveEntryRequest, protocol::RemoveEntryResponse>(
        protocol::CommandId::RemoveEntry, req);
}

core::Result<protocol::GetEntryResponse> IpcClient::get_entry(int64_t id) {
    protocol::GetEntryRequest req;
    req.id = id;
    return send_request<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, req);
}

core::Result<protocol::SearchEntriesResponse> IpcClient::search_entries(const core::SearchQuery& query) {
    protocol::SearchEntriesRequest req;
    req.query = query;
    return send_request<protocol::SearchEntriesRequest, protocol::SearchEntriesResponse>(
        protocol::CommandId::SearchEntries, req);
}

core::Result<protocol::ListEntriesResponse> IpcClient::list_entries() {
    return send_request<protocol::ListEntriesRequest, protocol::ListEntriesResponse>(
        protocol::CommandId::ListEntries, protocol::ListEntriesRequest{});
}

core::Result<protocol::GeneratePasswordResponse> IpcClient::generate_password(const core::PasswordGeneratorOptions& options) {
    protocol::GeneratePasswordRequest req;
    req.options = options;
    return send_request<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
        protocol::CommandId::GeneratePassword, req);
}

core::Result<protocol::EstimateStrengthResponse> IpcClient::estimate_strength(const std::string& password) {
    protocol::EstimateStrengthRequest req;
    req.password = password;
    return send_request<protocol::EstimateStrengthRequest, protocol::EstimateStrengthResponse>(
        protocol::CommandId::EstimateStrength, req);
}

// ---------------------------------------------------------------------------
// Generator history
// ---------------------------------------------------------------------------

core::Result<protocol::ListGeneratedRecordsResponse> IpcClient::list_generated_records() {
    return send_request<protocol::ListGeneratedRecordsRequest, protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords, protocol::ListGeneratedRecordsRequest{});
}

core::Result<protocol::RemoveGeneratedRecordResponse> IpcClient::remove_generated_record(int64_t id) {
    protocol::RemoveGeneratedRecordRequest req;
    req.id = id;
    return send_request<protocol::RemoveGeneratedRecordRequest, protocol::RemoveGeneratedRecordResponse>(
        protocol::CommandId::RemoveGeneratedRecord, req);
}

core::Result<protocol::ClearGeneratedRecordsResponse> IpcClient::clear_generated_records() {
    return send_request<protocol::ClearGeneratedRecordsRequest, protocol::ClearGeneratedRecordsResponse>(
        protocol::CommandId::ClearGeneratedRecords, protocol::ClearGeneratedRecordsRequest{});
}

core::Result<protocol::GetGeneratorSettingsResponse> IpcClient::get_generator_settings() {
    return send_request<protocol::GetGeneratorSettingsRequest, protocol::GetGeneratorSettingsResponse>(
        protocol::CommandId::GetGeneratorSettings, protocol::GetGeneratorSettingsRequest{});
}

core::Result<protocol::SetGeneratorLimitResponse> IpcClient::set_generator_limit(int32_t limit) {
    protocol::SetGeneratorLimitRequest req;
    req.limit = limit;
    return send_request<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, req);
}

// ---------------------------------------------------------------------------
// Tags管理
// ---------------------------------------------------------------------------

core::Result<protocol::AddTagResponse> IpcClient::add_tag(const core::Tag& tag) {
    protocol::AddTagRequest req;
    req.tag = tag;
    return send_request<protocol::AddTagRequest, protocol::AddTagResponse>(
        protocol::CommandId::AddTag, req);
}

core::Result<protocol::UpdateTagResponse> IpcClient::update_tag(const core::Tag& tag) {
    protocol::UpdateTagRequest req;
    req.tag = tag;
    return send_request<protocol::UpdateTagRequest, protocol::UpdateTagResponse>(
        protocol::CommandId::UpdateTag, req);
}

core::Result<protocol::RemoveTagResponse> IpcClient::remove_tag(int64_t id) {
    protocol::RemoveTagRequest req;
    req.id = id;
    return send_request<protocol::RemoveTagRequest, protocol::RemoveTagResponse>(
        protocol::CommandId::RemoveTag, req);
}

core::Result<protocol::ListTagsResponse> IpcClient::list_tags() {
    return send_request<protocol::ListTagsRequest, protocol::ListTagsResponse>(
        protocol::CommandId::ListTags, protocol::ListTagsRequest{});
}

core::Result<protocol::GetTagResponse> IpcClient::get_tag(int64_t id) {
    protocol::GetTagRequest req;
    req.id = id;
    return send_request<protocol::GetTagRequest, protocol::GetTagResponse>(
        protocol::CommandId::GetTag, req);
}

core::Result<protocol::FindTagByNameResponse> IpcClient::find_tag_by_name(const std::string& name) {
    protocol::FindTagByNameRequest req;
    req.name = name;
    return send_request<protocol::FindTagByNameRequest, protocol::FindTagByNameResponse>(
        protocol::CommandId::FindTagByName, req);
}

core::Result<protocol::GetEntryTagsResponse> IpcClient::get_entry_tags(int64_t entry_id) {
    protocol::GetEntryTagsRequest req;
    req.entry_id = entry_id;
    return send_request<protocol::GetEntryTagsRequest, protocol::GetEntryTagsResponse>(
        protocol::CommandId::GetEntryTags, req);
}

core::Result<protocol::SetEntryTagsResponse> IpcClient::set_entry_tags(int64_t entry_id, const std::vector<int64_t>& tag_ids) {
    protocol::SetEntryTagsRequest req;
    req.entry_id = entry_id;
    req.tag_ids = tag_ids;
    return send_request<protocol::SetEntryTagsRequest, protocol::SetEntryTagsResponse>(
        protocol::CommandId::SetEntryTags, req);
}

// ---------------------------------------------------------------------------
// 异步 IPC 命令实现（每个返回 QFuture，调用方用 QFutureWatcher 接收结果）
// ---------------------------------------------------------------------------

QFuture<core::Result<protocol::PingResponse>> IpcClient::ping_async() {
    return send_request_async<protocol::PingRequest, protocol::PingResponse>(
        protocol::CommandId::Ping, protocol::PingRequest{});
}

QFuture<core::Result<protocol::UnlockResponse>> IpcClient::unlock_async(const std::string& password) {
    protocol::UnlockRequest req;
    req.password = password;
    return send_request_async<protocol::UnlockRequest, protocol::UnlockResponse>(
        protocol::CommandId::Unlock, req);
}

QFuture<core::Result<protocol::LockResponse>> IpcClient::lock_async() {
    return send_request_async<protocol::LockRequest, protocol::LockResponse>(
        protocol::CommandId::Lock, protocol::LockRequest{});
}

QFuture<core::Result<protocol::EnableProgramPasswordResponse>> IpcClient::enable_program_password_async(const std::string& password) {
    protocol::EnableProgramPasswordRequest req;
    req.password = password;
    return send_request_async<protocol::EnableProgramPasswordRequest, protocol::EnableProgramPasswordResponse>(
        protocol::CommandId::EnableProgramPassword, req);
}

QFuture<core::Result<protocol::DisableProgramPasswordResponse>> IpcClient::disable_program_password_async(const std::string& password) {
    protocol::DisableProgramPasswordRequest req;
    req.password = password;
    return send_request_async<protocol::DisableProgramPasswordRequest, protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, req);
}

QFuture<core::Result<protocol::ChangeProgramPasswordResponse>> IpcClient::change_program_password_async(const std::string& old_password, const std::string& new_password) {
    protocol::ChangeProgramPasswordRequest req;
    req.old_password = old_password;
    req.new_password = new_password;
    return send_request_async<protocol::ChangeProgramPasswordRequest, protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, req);
}

QFuture<core::Result<protocol::GetVaultStatusResponse>> IpcClient::get_vault_status_async() {
    return send_request_async<protocol::GetVaultStatusRequest, protocol::GetVaultStatusResponse>(
        protocol::CommandId::GetVaultStatus, protocol::GetVaultStatusRequest{});
}

QFuture<core::Result<protocol::AddEntryResponse>> IpcClient::add_entry_async(const core::PasswordEntry& entry) {
    protocol::AddEntryRequest req;
    req.entry = entry;
    return send_request_async<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, req);
}

QFuture<core::Result<protocol::UpdateEntryResponse>> IpcClient::update_entry_async(const core::PasswordEntry& entry) {
    protocol::UpdateEntryRequest req;
    req.entry = entry;
    return send_request_async<protocol::UpdateEntryRequest, protocol::UpdateEntryResponse>(
        protocol::CommandId::UpdateEntry, req);
}

QFuture<core::Result<protocol::RemoveEntryResponse>> IpcClient::remove_entry_async(int64_t id) {
    protocol::RemoveEntryRequest req;
    req.id = id;
    return send_request_async<protocol::RemoveEntryRequest, protocol::RemoveEntryResponse>(
        protocol::CommandId::RemoveEntry, req);
}

QFuture<core::Result<protocol::GetEntryResponse>> IpcClient::get_entry_async(int64_t id) {
    protocol::GetEntryRequest req;
    req.id = id;
    return send_request_async<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, req);
}

QFuture<core::Result<protocol::SearchEntriesResponse>> IpcClient::search_entries_async(const core::SearchQuery& query) {
    protocol::SearchEntriesRequest req;
    req.query = query;
    return send_request_async<protocol::SearchEntriesRequest, protocol::SearchEntriesResponse>(
        protocol::CommandId::SearchEntries, req);
}

QFuture<core::Result<protocol::ListEntriesResponse>> IpcClient::list_entries_async() {
    return send_request_async<protocol::ListEntriesRequest, protocol::ListEntriesResponse>(
        protocol::CommandId::ListEntries, protocol::ListEntriesRequest{});
}

QFuture<core::Result<protocol::GeneratePasswordResponse>> IpcClient::generate_password_async(const core::PasswordGeneratorOptions& options) {
    protocol::GeneratePasswordRequest req;
    req.options = options;
    return send_request_async<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
        protocol::CommandId::GeneratePassword, req);
}

QFuture<core::Result<protocol::EstimateStrengthResponse>> IpcClient::estimate_strength_async(const std::string& password) {
    protocol::EstimateStrengthRequest req;
    req.password = password;
    return send_request_async<protocol::EstimateStrengthRequest, protocol::EstimateStrengthResponse>(
        protocol::CommandId::EstimateStrength, req);
}

// ---------------------------------------------------------------------------
// 异步：Generator history
// ---------------------------------------------------------------------------

QFuture<core::Result<protocol::ListGeneratedRecordsResponse>> IpcClient::list_generated_records_async() {
    return send_request_async<protocol::ListGeneratedRecordsRequest, protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords, protocol::ListGeneratedRecordsRequest{});
}

QFuture<core::Result<protocol::RemoveGeneratedRecordResponse>> IpcClient::remove_generated_record_async(int64_t id) {
    protocol::RemoveGeneratedRecordRequest req;
    req.id = id;
    return send_request_async<protocol::RemoveGeneratedRecordRequest, protocol::RemoveGeneratedRecordResponse>(
        protocol::CommandId::RemoveGeneratedRecord, req);
}

QFuture<core::Result<protocol::ClearGeneratedRecordsResponse>> IpcClient::clear_generated_records_async() {
    return send_request_async<protocol::ClearGeneratedRecordsRequest, protocol::ClearGeneratedRecordsResponse>(
        protocol::CommandId::ClearGeneratedRecords, protocol::ClearGeneratedRecordsRequest{});
}

QFuture<core::Result<protocol::GetGeneratorSettingsResponse>> IpcClient::get_generator_settings_async() {
    return send_request_async<protocol::GetGeneratorSettingsRequest, protocol::GetGeneratorSettingsResponse>(
        protocol::CommandId::GetGeneratorSettings, protocol::GetGeneratorSettingsRequest{});
}

QFuture<core::Result<protocol::SetGeneratorLimitResponse>> IpcClient::set_generator_limit_async(int32_t limit) {
    protocol::SetGeneratorLimitRequest req;
    req.limit = limit;
    return send_request_async<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, req);
}

// ---------------------------------------------------------------------------
// 异步：Tags管理
// ---------------------------------------------------------------------------

QFuture<core::Result<protocol::AddTagResponse>> IpcClient::add_tag_async(const core::Tag& tag) {
    protocol::AddTagRequest req;
    req.tag = tag;
    return send_request_async<protocol::AddTagRequest, protocol::AddTagResponse>(
        protocol::CommandId::AddTag, req);
}

QFuture<core::Result<protocol::UpdateTagResponse>> IpcClient::update_tag_async(const core::Tag& tag) {
    protocol::UpdateTagRequest req;
    req.tag = tag;
    return send_request_async<protocol::UpdateTagRequest, protocol::UpdateTagResponse>(
        protocol::CommandId::UpdateTag, req);
}

QFuture<core::Result<protocol::RemoveTagResponse>> IpcClient::remove_tag_async(int64_t id) {
    protocol::RemoveTagRequest req;
    req.id = id;
    return send_request_async<protocol::RemoveTagRequest, protocol::RemoveTagResponse>(
        protocol::CommandId::RemoveTag, req);
}

QFuture<core::Result<protocol::ListTagsResponse>> IpcClient::list_tags_async() {
    return send_request_async<protocol::ListTagsRequest, protocol::ListTagsResponse>(
        protocol::CommandId::ListTags, protocol::ListTagsRequest{});
}

QFuture<core::Result<protocol::GetTagResponse>> IpcClient::get_tag_async(int64_t id) {
    protocol::GetTagRequest req;
    req.id = id;
    return send_request_async<protocol::GetTagRequest, protocol::GetTagResponse>(
        protocol::CommandId::GetTag, req);
}

QFuture<core::Result<protocol::FindTagByNameResponse>> IpcClient::find_tag_by_name_async(const std::string& name) {
    protocol::FindTagByNameRequest req;
    req.name = name;
    return send_request_async<protocol::FindTagByNameRequest, protocol::FindTagByNameResponse>(
        protocol::CommandId::FindTagByName, req);
}

QFuture<core::Result<protocol::GetEntryTagsResponse>> IpcClient::get_entry_tags_async(int64_t entry_id) {
    protocol::GetEntryTagsRequest req;
    req.entry_id = entry_id;
    return send_request_async<protocol::GetEntryTagsRequest, protocol::GetEntryTagsResponse>(
        protocol::CommandId::GetEntryTags, req);
}

QFuture<core::Result<protocol::SetEntryTagsResponse>> IpcClient::set_entry_tags_async(int64_t entry_id, const std::vector<int64_t>& tag_ids) {
    protocol::SetEntryTagsRequest req;
    req.entry_id = entry_id;
    req.tag_ids = tag_ids;
    return send_request_async<protocol::SetEntryTagsRequest, protocol::SetEntryTagsResponse>(
        protocol::CommandId::SetEntryTags, req);
}

}  // namespace yuli::vault::ui
