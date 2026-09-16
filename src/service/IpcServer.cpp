// coding: utf-8
// =============================================================================
// IpcServer.cpp
//
// 命名管道服务端实现。
//
// 关键 Windows API：
//   - CreateNamedPipeW      创建一个命名管道实例
//   - ConnectNamedPipe      等待客户端连接（OVERLAPPED 模式下非阻塞）
//   - ReadFile / WriteFile  读写管道（OVERLAPPED 模式下可超时等待）
//   - CancelIoEx            取消指定 handle 上所有 pending I/O（跨线程）
//   - WaitForMultipleObjects 同时等待 I/O 事件、停止事件与超时
//
// RAII：使用 unique_ptr + 自定义 deleter 管理 HANDLE 与 OVERLAPPED 事件。
// =============================================================================
#include "IpcServer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/Error.h"
#include "core/Result.h"
#include "core/Types.h"
#include "protocol/Messages.h"
#include "protocol/Serializer.h"

namespace yuli::vault::service {

namespace {

// ============================================================================
// RAII 句柄包装
// ============================================================================

struct HandleDeleter {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using HandlePtr = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

/// 将 std::string（UTF-8）转为 std::wstring，供 CreateNamedPipeW 使用。
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

constexpr DWORD kPipeBufferSize = 64 * 1024;  // 64 KB
// 写超时：若客户端不读取响应，30 秒后中止写入，避免工作线程被卡死。
// 读取使用 INFINITE——命名管道在客户端进程退出时立即产生 ERROR_BROKEN_PIPE，
// 无需空闲超时来检测死连接；空闲超时反而会断开正常的空闲长连接。
constexpr DWORD kWriteTimeoutMs = 30'000;

/// 构造一个 manual-reset 事件。
HandlePtr create_manual_event() {
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return HandlePtr(h);
}

/// 等待单个 overlapped I/O 完成，同时响应停止事件与超时。
/// \param io_event I/O 完成事件
/// \param stop_event 停止事件
/// \param handle 管道 handle，超时/停止时调用 CancelIo(handle) 取消
/// \param timeout_ms 超时毫秒
/// \return >0 表示 I/O 正常完成（调用方应 GetOverlappedResult）；
///         0 表示需要中止（超时或停止）
int wait_io_or_stop(HANDLE io_event, HANDLE stop_event, HANDLE handle,
                    DWORD timeout_ms) {
    HANDLE waits[2] = {io_event, stop_event};
    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    if (wait == WAIT_OBJECT_0) {
        return 1;  // I/O 完成
    }
    if (wait == WAIT_OBJECT_0 + 1) {
        // 停止信号：取消 pending I/O 并等待其完成
        CancelIo(handle);
        WaitForSingleObject(io_event, INFINITE);
        return 0;
    }
    if (wait == WAIT_TIMEOUT) {
        // 超时：取消 pending I/O 并等待其完成
        CancelIo(handle);
        WaitForSingleObject(io_event, INFINITE);
        return 0;
    }
    // 其他错误
    CancelIo(handle);
    WaitForSingleObject(io_event, INFINITE);
    return 0;
}

/// 同步读取指定字节数（使用 OVERLAPPED 以支持超时与取消）。
/// \return true 读取成功且字节数匹配；false 表示失败/超时/停止
bool read_exact(HANDLE handle, void* buffer, size_t total,
                HANDLE io_event, HANDLE stop_event, DWORD timeout_ms) {
    auto* dst = static_cast<std::byte*>(buffer);
    size_t received = 0;
    while (received < total) {
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        ResetEvent(io_event);

        DWORD to_read = static_cast<DWORD>(total - received);
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(handle, dst + received, to_read, &bytes_read, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;  // 客户端断开或其他错误
            }
            // pending：等待完成、停止或超时
            if (wait_io_or_stop(io_event, stop_event, handle, timeout_ms) == 0) {
                return false;
            }
            if (!GetOverlappedResult(handle, &ov, &bytes_read, FALSE)) {
                return false;
            }
        }
        if (bytes_read == 0) {
            return false;  // EOF
        }
        received += bytes_read;
    }
    return true;
}

/// 同步写入指定字节数（使用 OVERLAPPED 以支持超时与取消）。
bool write_exact(HANDLE handle, const void* buffer, size_t total,
                 HANDLE io_event, HANDLE stop_event, DWORD timeout_ms) {
    auto* src = static_cast<const std::byte*>(buffer);
    size_t sent = 0;
    while (sent < total) {
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        ResetEvent(io_event);

        DWORD to_write = static_cast<DWORD>(total - sent);
        DWORD bytes_written = 0;
        BOOL ok = WriteFile(handle, src + sent, to_write, &bytes_written, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return false;
            }
            if (wait_io_or_stop(io_event, stop_event, handle, timeout_ms) == 0) {
                return false;
            }
            if (!GetOverlappedResult(handle, &ov, &bytes_written, FALSE)) {
                return false;
            }
        }
        if (bytes_written == 0) {
            return false;
        }
        sent += bytes_written;
    }
    return true;
}

}  // namespace

// ============================================================================
// 构造与析构
// ============================================================================

IpcServer::IpcServer(std::string_view pipe_name, Handler handler)
    : pipe_name_(pipe_name), handler_(std::move(handler)) {}

IpcServer::~IpcServer() {
    stop();
}

// ============================================================================
// 活跃客户端管理
// ============================================================================

void IpcServer::register_client(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        active_client_handles_.push_back(h);
    }
    active_clients_.fetch_add(1);
    touch_activity();
}

void IpcServer::unregister_client(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        active_client_handles_.erase(
            std::remove(active_client_handles_.begin(),
                        active_client_handles_.end(), h),
            active_client_handles_.end());
    }
    active_clients_.fetch_sub(1);
    touch_activity();
}

void IpcServer::touch_activity() {
    std::lock_guard<std::mutex> lock(activity_mutex_);
    last_activity_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point IpcServer::last_activity() const {
    std::lock_guard<std::mutex> lock(activity_mutex_);
    return last_activity_;
}

// ============================================================================
// start / stop
// ============================================================================

void IpcServer::start() {
    if (running_.exchange(true)) {
        return;  // 已在运行
    }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        running_ = false;
        return;
    }
    listener_thread_ = std::thread([this] { listener_loop(); });
}

void IpcServer::stop() {
    if (!running_.exchange(false)) {
        return;  // 未运行
    }

    // 1. 唤醒监听线程
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
    if (listen_pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(listen_pipe_, nullptr);
    }
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
    if (listen_pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(listen_pipe_);
        listen_pipe_ = INVALID_HANDLE_VALUE;
    }

    // 2. 取消所有活跃客户端的 pending I/O（让工作线程退出 ReadFile/WriteFile）
    {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        for (HANDLE h : active_client_handles_) {
            if (h != nullptr && h != INVALID_HANDLE_VALUE) {
                CancelIoEx(h, nullptr);
            }
        }
    }

    // 3. join 所有工作线程
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    // 4. 关闭停止事件
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

// ============================================================================
// 监听线程
// ============================================================================

void IpcServer::listener_loop() {
    const std::wstring wide_name = utf8_to_wide(pipe_name_);

    while (running_.load()) {
        // 创建一个新的管道实例（OVERLAPPED 模式）
        HANDLE raw = CreateNamedPipeW(
            wide_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kPipeBufferSize,
            kPipeBufferSize,
            0,
            nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            // 创建失败：稍等后重试，避免忙循环
            if (!running_.load()) break;
            WaitForSingleObject(stop_event_, 1000);
            continue;
        }

        // OVERLAPPED 等待连接
        OVERLAPPED connect_ov{};
        HandlePtr connect_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        connect_ov.hEvent = connect_event.get();

        BOOL connected = ConnectNamedPipe(raw, &connect_ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // 等待连接或停止
                HANDLE waits[2] = {connect_event.get(), stop_event_};
                DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 + 1) {
                    // 停止：取消 pending connect 并关闭管道实例
                    CancelIo(raw);
                    WaitForSingleObject(connect_event.get(), INFINITE);
                    CloseHandle(raw);
                    break;
                }
                if (wait != WAIT_OBJECT_0) {
                    // 异常：关闭管道实例重试
                    CloseHandle(raw);
                    continue;
                }
                // 连接成功
            } else if (err == ERROR_PIPE_CONNECTED) {
                // 客户端在 ConnectNamedPipe 前已连接（罕见但合法）
            } else {
                // 其他错误
                CloseHandle(raw);
                continue;
            }
        }

        if (!running_.load()) {
            CloseHandle(raw);
            break;
        }

        // 启动工作线程处理该客户端
        register_client(raw);
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.emplace_back([this, raw] { client_loop(raw); });
        }
    }
}

// ============================================================================
// 客户端工作线程
// ============================================================================

void IpcServer::client_loop(HANDLE client_handle) {
    // RAII：保证退出时关闭 handle 与注销
    HandlePtr handle_guard(client_handle);
    HandlePtr io_event(create_manual_event());

    auto cleanup = [this, client_handle] {
        unregister_client(client_handle);
        // handle 由 handle_guard 关闭
    };

    while (running_.load()) {
        // 1. 读 16 字节 MessageHeader
        //    读取使用 INFINITE 超时：UI 空闲时不发请求，服务端应耐心等待，
        //    而非 30 秒后断开连接。客户端进程退出时管道立即断裂（ERROR_BROKEN_PIPE），
        //    read_exact 会返回 false 并退出循环。
        protocol::MessageHeader header{};
        if (!read_exact(client_handle, &header, sizeof(header),
                        io_event.get(), stop_event_, INFINITE)) {
            break;
        }

        // 2. 解析 header
        auto header_result = protocol::parse_header(
            core::ByteSpan(reinterpret_cast<const std::byte*>(&header),
                           sizeof(header)));
        if (!header_result) {
            // header 非法：断开客户端
            break;
        }
        const protocol::MessageHeader& parsed = header_result->first;

        // 3. 读 payload
        core::ByteVec payload(parsed.payload_size);
        if (parsed.payload_size > 0) {
            if (!read_exact(client_handle, payload.data(), payload.size(),
                            io_event.get(), stop_event_, INFINITE)) {
                break;
            }
        }

        // 4. 调用 handler
        core::ByteVec response_payload;
        if (handler_) {
            response_payload = handler_(payload, parsed);
        }

        // 5. 构造响应 header（echo command 与 request_id）
        protocol::MessageHeader resp_header{};
        resp_header.magic = protocol::kMagic;
        resp_header.version = protocol::kProtocolVersion;
        resp_header.command = parsed.command;
        resp_header.request_id = parsed.request_id;
        resp_header.payload_size = static_cast<uint32_t>(response_payload.size());

        // 6. 写响应 header + payload（写超时 30 秒，防止客户端不读取响应卡死线程）
        if (!write_exact(client_handle, &resp_header, sizeof(resp_header),
                         io_event.get(), stop_event_, kWriteTimeoutMs)) {
            break;
        }
        if (!response_payload.empty()) {
            if (!write_exact(client_handle, response_payload.data(),
                             response_payload.size(),
                             io_event.get(), stop_event_, kWriteTimeoutMs)) {
                break;
            }
        }
    }

    cleanup();
    DisconnectNamedPipe(client_handle);
    // handle_guard 析构时 CloseHandle
}

}  // namespace yuli::vault::service
