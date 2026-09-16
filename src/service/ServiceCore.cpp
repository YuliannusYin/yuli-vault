// coding: utf-8
// =============================================================================
// ServiceCore.cpp
//
// 命令分发与业务逻辑实现。
//
// 加密约定：
//   - entry.password 在内存中为明文 std::string
//   - 程序密码已启用时：持久化前用 entry_crypto_->encrypt(password) 加密，
//     得到 [IV || ciphertext || tag]，拆分为 iv(12) / password(密文) / tag(16) 三段
//   - 程序密码未启用时（明文模式）：password 直接以明文 BLOB 存入 storage，
//     iv 与 tag 为空 BLOB
//   - 读取时：若 iv/tag 非空则拼回 [IV || ciphertext || tag] 解密；否则直接返回明文
//
// 解锁失败计数：
//   - 连续 5 次失败后锁定 5 分钟，期间 Unlock 直接返回错误
//   - 成功解锁后计数清零
// =============================================================================
#include "ServiceCore.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CryptoEngine.h"
#include "ICryptoEngine.h"
#include "IPasswordGenerator.h"
#include "IStorageEngine.h"
#include "ProgramPasswordStore.h"
#include "Types.h"

#include "Commands.h"
#include "Messages.h"
#include "Serializer.h"
#include "VaultPayload.h"

#include <sodium.h>

namespace yuli::vault::service {

namespace {

constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;
constexpr int kMaxLoginAttempts = 5;
constexpr auto kLockoutDuration = std::chrono::minutes(5);

/// 安全清零 ByteVec。
void secure_zero(core::ByteVec& v) {
    if (!v.empty()) {
        sodium_memzero(v.data(), v.size());
    }
}

/// 安全清零 std::string。
void secure_zero_string(std::string& s) {
    if (!s.empty()) {
        sodium_memzero(s.data(), s.size());
    }
}

/// 当前 Unix 时间戳（秒）。
uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

// ============================================================================
// 构造与析构
// ============================================================================

ServiceCore::ServiceCore(std::unique_ptr<core::ICryptoEngine> crypto,
                         std::unique_ptr<core::IStorageEngine> storage,
                         std::unique_ptr<core::IPasswordGenerator> generator,
                         std::filesystem::path meta_path)
    : crypto_(std::move(crypto)),
      storage_(std::move(storage)),
      generator_(std::move(generator)),
      password_store_(std::make_unique<ProgramPasswordStore>(std::move(meta_path))) {
    // 启动时检测程序密码是否已启用
    password_enabled_ = password_store_->exists();
    if (!password_enabled_) {
        // 明文模式：自动解锁，并将复制过来的 v2 库升级为 v3。
        unlocked_ = true;
        (void)migrate_legacy_schema_unlocked();
    }
    // 加密模式下 unlocked_ 保持 false，需用户 Unlock 后才解锁
}

ServiceCore::~ServiceCore() {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_encryption_key();
}

// ============================================================================
// 内部辅助
// ============================================================================

void ServiceCore::set_encryption_key(core::ByteVec key) {
    // 调用方持锁
    encryption_key_ = std::move(key);
    entry_crypto_ = std::make_unique<crypto::CryptoEngine>(encryption_key_);
}

void ServiceCore::clear_encryption_key() {
    // 调用方持锁
    entry_crypto_.reset();
    secure_zero(encryption_key_);
    encryption_key_.clear();
}

bool ServiceCore::is_in_cooldown() const {
    return lock_until_ > std::chrono::steady_clock::now();
}

core::ByteVec ServiceCore::make_error(core::ErrorCode code, std::string message) const {
    protocol::ErrorResponse resp;
    resp.code = code;
    resp.message = std::move(message);
    return protocol::serialize(resp);
}

core::Result<core::PasswordEntry> ServiceCore::encrypt_entry(
    core::PasswordEntry entry) const {
    core::ByteVec plain = core::encode_item_payload(entry);
    if (!password_enabled_) {
        entry.payload = std::move(plain);
        entry.iv.clear();
        entry.tag.clear();
        return core::Result<core::PasswordEntry>::Ok(std::move(entry));
    }

    if (!entry_crypto_) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::Unauthorized, "vault is locked"));
    }

    core::ByteSpan plaintext(plain.data(), plain.size());
    auto enc_result = entry_crypto_->encrypt(plaintext);
    if (!enc_result) {
        return core::Result<core::PasswordEntry>::Err(enc_result.error());
    }

    const auto& blob = *enc_result;
    if (blob.size() < kIvLen + kTagLen) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::CryptoError, "ciphertext blob too short"));
    }

    entry.iv.assign(blob.begin(), blob.begin() + kIvLen);
    entry.tag.assign(blob.end() - kTagLen, blob.end());
    entry.payload.assign(blob.begin() + kIvLen, blob.end() - kTagLen);
    return core::Result<core::PasswordEntry>::Ok(std::move(entry));
}

core::Result<core::PasswordEntry> ServiceCore::decrypt_entry(
    core::PasswordEntry entry) const {
    if (!password_enabled_) {
        if (!entry.payload.empty()) {
            (void)core::try_decode_item_payload(entry, entry.payload);
        }
        return core::Result<core::PasswordEntry>::Ok(std::move(entry));
    }

    if (!entry_crypto_) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::Unauthorized, "vault is locked"));
    }

    // Legacy v2 rows store password ciphertext in `password` + iv/tag.
    // v3 rows store payload ciphertext in `payload` + iv/tag.
    core::ByteVec blob;
    if (!entry.payload.empty() && !entry.iv.empty()) {
        blob.reserve(entry.iv.size() + entry.payload.size() + entry.tag.size());
        blob.insert(blob.end(), entry.iv.begin(), entry.iv.end());
        blob.insert(blob.end(), entry.payload.begin(), entry.payload.end());
        blob.insert(blob.end(), entry.tag.begin(), entry.tag.end());
    } else {
        blob.reserve(entry.iv.size() + entry.password.size() + entry.tag.size());
        blob.insert(blob.end(), entry.iv.begin(), entry.iv.end());
        blob.insert(blob.end(),
                    reinterpret_cast<const std::byte*>(entry.password.data()),
                    reinterpret_cast<const std::byte*>(entry.password.data()) +
                        entry.password.size());
        blob.insert(blob.end(), entry.tag.begin(), entry.tag.end());
    }

    auto dec_result = entry_crypto_->decrypt(blob);
    if (!dec_result) {
        return core::Result<core::PasswordEntry>::Err(dec_result.error());
    }

    const std::string& plain = *dec_result;
    core::ByteSpan plain_span(
        reinterpret_cast<const std::byte*>(plain.data()), plain.size());
    if (!core::try_decode_item_payload(entry, plain_span)) {
        // v2: decrypt produced the password string only.
        entry.password = plain;
        entry.type = core::VaultItemType::Login;
    }
    entry.iv.clear();
    entry.tag.clear();
    entry.payload.clear();
    return core::Result<core::PasswordEntry>::Ok(std::move(entry));
}

core::Error ServiceCore::migrate_legacy_schema_unlocked() {
    if (storage_->schema_version() >= 3) {
        return core::Error{};
    }
    return storage_->migrate_v2_to_v3([this](core::VaultItem row) {
        // v2: password column may be ciphertext; decrypt_entry understands that.
        auto dec = decrypt_entry(std::move(row));
        if (!dec) {
            return core::Result<core::VaultItem>::Err(dec.error());
        }
        return encrypt_entry(std::move(*dec));
    });
}

core::Result<core::GeneratedPasswordRecord> ServiceCore::encrypt_generated_record(
    core::GeneratedPasswordRecord record) const {
    // 明文模式：不做加密
    if (!password_enabled_) {
        record.iv.clear();
        record.tag.clear();
        return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(record));
    }
    if (!entry_crypto_) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::Unauthorized, "vault is locked"));
    }
    core::ByteSpan plaintext(
        reinterpret_cast<const std::byte*>(record.password.data()),
        record.password.size());
    auto enc_result = entry_crypto_->encrypt(plaintext);
    if (!enc_result) {
        return core::Result<core::GeneratedPasswordRecord>::Err(enc_result.error());
    }
    const auto& blob = *enc_result;
    if (blob.size() < kIvLen + kTagLen) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::CryptoError, "ciphertext blob too short"));
    }
    record.iv.assign(blob.begin(), blob.begin() + kIvLen);
    record.tag.assign(blob.end() - kTagLen, blob.end());
    record.password.assign(
        reinterpret_cast<const char*>(blob.data() + kIvLen),
        blob.size() - kIvLen - kTagLen);
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(record));
}

core::Result<core::GeneratedPasswordRecord> ServiceCore::decrypt_generated_record(
    core::GeneratedPasswordRecord record) const {
    // 明文模式：不做解密
    if (!password_enabled_) {
        return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(record));
    }
    if (!entry_crypto_) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::Unauthorized, "vault is locked"));
    }
    core::ByteVec blob;
    blob.reserve(record.iv.size() + record.password.size() + record.tag.size());
    blob.insert(blob.end(), record.iv.begin(), record.iv.end());
    blob.insert(blob.end(),
                reinterpret_cast<const std::byte*>(record.password.data()),
                reinterpret_cast<const std::byte*>(record.password.data()) +
                    record.password.size());
    blob.insert(blob.end(), record.tag.begin(), record.tag.end());
    auto dec_result = entry_crypto_->decrypt(blob);
    if (!dec_result) {
        return core::Result<core::GeneratedPasswordRecord>::Err(dec_result.error());
    }
    record.password = std::move(*dec_result);
    record.iv.clear();
    record.tag.clear();
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(record));
}

int32_t ServiceCore::current_generator_limit() {
    auto r = storage_->get_setting("generator.history_limit");
    if (!r) return core::kGeneratorLimitUnlimited;
    if (r->empty()) return core::kGeneratorLimitUnlimited;
    try {
        return static_cast<int32_t>(std::stoi(*r));
    } catch (...) {
        return core::kGeneratorLimitUnlimited;
    }
}

core::Error ServiceCore::enforce_generator_limit(int32_t limit) {
    if (limit <= 0) return core::Error{};  // 0 = 无限制
    auto list_result = storage_->list_generated_records();
    if (!list_result) {
        return list_result.error();
    }
    // list_generated_records 返回顺序：created_at DESC, id DESC
    // 即最新在前；保留前 limit 条，删除其余
    auto& records = *list_result;
    if (static_cast<int32_t>(records.size()) <= limit) return core::Error{};
    for (size_t i = static_cast<size_t>(limit); i < records.size(); ++i) {
        auto rm_err = storage_->remove_generated_record(records[i].id);
        if (!rm_err.ok()) return rm_err;
    }
    return core::Error{};
}

// ============================================================================
// 请求分发
// ============================================================================

core::ByteVec ServiceCore::handle_request(core::ByteSpan payload,
                                          const protocol::MessageHeader& req_header) {
    using protocol::CommandId;
    switch (req_header.command) {
        case CommandId::Ping:                    return handle_ping();
        case CommandId::Unlock:                  return handle_unlock(payload);
        case CommandId::Lock:                    return handle_lock();
        case CommandId::EnableProgramPassword:   return handle_enable_program_password(payload);
        case CommandId::DisableProgramPassword:  return handle_disable_program_password(payload);
        case CommandId::ChangeProgramPassword:   return handle_change_program_password(payload);
        case CommandId::GetVaultStatus:          return handle_get_vault_status();
        case CommandId::AddEntry:                return handle_add_entry(payload);
        case CommandId::UpdateEntry:             return handle_update_entry(payload);
        case CommandId::RemoveEntry:             return handle_remove_entry(payload);
        case CommandId::GetEntry:                return handle_get_entry(payload);
        case CommandId::SearchEntries:           return handle_search_entries(payload);
        case CommandId::ListEntries:             return handle_list_entries();
        case CommandId::GeneratePassword:        return handle_generate_password(payload);
        case CommandId::EstimateStrength:        return handle_estimate_strength(payload);
        case CommandId::ListGeneratedRecords:    return handle_list_generated_records();
        case CommandId::RemoveGeneratedRecord:   return handle_remove_generated_record(payload);
        case CommandId::ClearGeneratedRecords:   return handle_clear_generated_records();
        case CommandId::GetGeneratorSettings:    return handle_get_generator_settings();
        case CommandId::SetGeneratorLimit:        return handle_set_generator_limit(payload);
        case CommandId::AddTag:                   return handle_add_tag(payload);
        case CommandId::UpdateTag:               return handle_update_tag(payload);
        case CommandId::RemoveTag:               return handle_remove_tag(payload);
        case CommandId::ListTags:                return handle_list_tags();
        case CommandId::GetTag:                  return handle_get_tag(payload);
        case CommandId::FindTagByName:           return handle_find_tag_by_name(payload);
        case CommandId::GetEntryTags:            return handle_get_entry_tags(payload);
        case CommandId::SetEntryTags:            return handle_set_entry_tags(payload);
        case CommandId::Shutdown:
            // Shutdown 由 main.cpp 的 handler lambda 拦截处理，不应到达此处
            return protocol::serialize(protocol::ShutdownResponse{});
    }
    return make_error(core::ErrorCode::InvalidArgument, "unknown command");
}

// ============================================================================
// 各 handler
// ============================================================================

core::ByteVec ServiceCore::handle_ping() {
    protocol::PingResponse resp;
    resp.server_timestamp = now_unix_seconds();
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_unlock(core::ByteSpan payload) {
    auto req_result = protocol::deserialize<protocol::UnlockRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed UnlockRequest: ") +
                              req_result.error().what());
    }
    const auto& req = *req_result;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!password_enabled_) {
        // 明文模式无需解锁
        protocol::UnlockResponse resp;
        resp.success = true;
        return protocol::serialize(resp);
    }

    if (is_in_cooldown()) {
        // 计算冷却剩余秒数，写入 error_message 供 UI 解析倒计时
        auto remaining_sec = std::chrono::duration_cast<std::chrono::seconds>(
            lock_until_ - std::chrono::steady_clock::now()).count();
        if (remaining_sec < 0) remaining_sec = 0;
        protocol::UnlockResponse resp;
        resp.success = false;
        resp.error_message = "Locked, retry in " + std::to_string(remaining_sec) +
                             " seconds";
        return protocol::serialize(resp);
    }

    auto unlock_result = password_store_->unlock(req.password, *crypto_);
    if (!unlock_result) {
        ++login_attempts_;
        if (login_attempts_ >= kMaxLoginAttempts) {
            // 刚触发锁定：返回完整锁定提示（含锁定总时长），UI 进入倒计时
            lock_until_ = std::chrono::steady_clock::now() + kLockoutDuration;
            auto lockout_sec = std::chrono::duration_cast<std::chrono::seconds>(
                kLockoutDuration).count();
            protocol::UnlockResponse resp;
            resp.success = false;
            resp.error_message = "Too many incorrect passwords, locked. Retry in " +
                                 std::to_string(lockout_sec) + " seconds";
            return protocol::serialize(resp);
        }
        // 普通失败：返回剩余尝试次数，UI 同步显示
        const int remaining = kMaxLoginAttempts - login_attempts_;
        protocol::UnlockResponse resp;
        resp.success = false;
        resp.error_message = "Incorrect password, " + std::to_string(remaining) +
                             " attempts remaining";
        return protocol::serialize(resp);
    }

    set_encryption_key(std::move(*unlock_result));
    unlocked_ = true;
    login_attempts_ = 0;

    auto mig = migrate_legacy_schema_unlocked();
    if (!mig.ok()) {
        clear_encryption_key();
        unlocked_ = false;
        protocol::UnlockResponse resp;
        resp.success = false;
        resp.error_message = std::string("failed to migrate vault schema: ") + mig.what();
        return protocol::serialize(resp);
    }

    protocol::UnlockResponse resp;
    resp.success = true;
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_lock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!password_enabled_) {
        // 明文模式不支持锁定
        return make_error(core::ErrorCode::InvalidArgument,
                          "lock is not available when program password is disabled");
    }
    clear_encryption_key();
    unlocked_ = false;
    return protocol::serialize(protocol::LockResponse{});
}

core::ByteVec ServiceCore::handle_enable_program_password(core::ByteSpan payload) {
    auto req_result = protocol::deserialize<protocol::EnableProgramPasswordRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed EnableProgramPasswordRequest: ") +
                              req_result.error().what());
    }
    const auto& req = *req_result;

    std::lock_guard<std::mutex> lock(mutex_);

    if (password_enabled_) {
        protocol::EnableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = "program password is already enabled";
        return protocol::serialize(resp);
    }

    // 1. 初始化 vault.meta，获取新的 encryption_key
    auto init_result = password_store_->initialize(req.password, *crypto_);
    if (!init_result) {
        protocol::EnableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = init_result.error().what();
        return protocol::serialize(resp);
    }

    // 2. 设置 encryption_key 并切换到加密模式
    set_encryption_key(std::move(*init_result));
    password_enabled_ = true;
    unlocked_ = true;
    login_attempts_ = 0;

    // 3. 重新加密所有现有明文条目
    auto list_result = storage_->list_entries();
    if (!list_result) {
        // 列表失败，回滚：清除密钥但保留 vault.meta
        clear_encryption_key();
        password_enabled_ = false;
        unlocked_ = true;
        password_store_->destroy();
        protocol::EnableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = std::string("failed to list entries for re-encryption: ") +
                             list_result.error().what();
        return protocol::serialize(resp);
    }

    // 逐条加密并更新
    for (auto& entry : *list_result) {
        // entry 来自明文存储，password 为明文，iv/tag 为空
        auto enc_result = encrypt_entry(std::move(entry));
        if (!enc_result) {
            // 加密失败：回滚（尽力而为）
            clear_encryption_key();
            password_enabled_ = false;
            unlocked_ = true;
            password_store_->destroy();
            protocol::EnableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = std::string("failed to encrypt entry: ") +
                                 enc_result.error().what();
            return protocol::serialize(resp);
        }
        auto upd_result = storage_->update_entry(*enc_result);
        if (!upd_result) {
            clear_encryption_key();
            password_enabled_ = false;
            unlocked_ = true;
            password_store_->destroy();
            protocol::EnableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = std::string("failed to update encrypted entry: ") +
                                 upd_result.error().what();
            return protocol::serialize(resp);
        }
    }

    // 3b. 同步重新加密所有生成记录（明文→密文）
    //     失败视为非致命：记录仍可由用户清空，不影响主流程的成功
    //     使用 update_generated_record 保留原始 created_at 时间戳
    {
        auto gen_list = storage_->list_generated_records();
        if (gen_list) {
            for (auto& rec : *gen_list) {
                auto enc = encrypt_generated_record(std::move(rec));
                if (!enc) break;
                (void)storage_->update_generated_record(*enc);
            }
        }
    }

    protocol::EnableProgramPasswordResponse resp;
    resp.success = true;
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_disable_program_password(core::ByteSpan payload) {
    auto req_result = protocol::deserialize<protocol::DisableProgramPasswordRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed DisableProgramPasswordRequest: ") +
                              req_result.error().what());
    }
    const auto& req = *req_result;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!password_enabled_) {
        protocol::DisableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = "program password is not enabled";
        return protocol::serialize(resp);
    }

    // 1. 验证程序密码并获取 encryption_key
    if (!unlocked_ || !entry_crypto_) {
        // 需要先解锁
        if (is_in_cooldown()) {
            protocol::DisableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = "too many failed attempts, please wait";
            return protocol::serialize(resp);
        }
        auto unlock_result = password_store_->unlock(req.password, *crypto_);
        if (!unlock_result) {
            ++login_attempts_;
            if (login_attempts_ >= kMaxLoginAttempts) {
                lock_until_ = std::chrono::steady_clock::now() + kLockoutDuration;
            }
            protocol::DisableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = unlock_result.error().what();
            return protocol::serialize(resp);
        }
        set_encryption_key(std::move(*unlock_result));
        unlocked_ = true;
        login_attempts_ = 0;
    }

    // 2. 解密所有条目转回明文
    auto list_result = storage_->list_entries();
    if (!list_result) {
        protocol::DisableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = std::string("failed to list entries: ") +
                             list_result.error().what();
        return protocol::serialize(resp);
    }

    for (auto& entry : *list_result) {
        auto dec_result = decrypt_entry(std::move(entry));
        if (!dec_result) {
            protocol::DisableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = std::string("failed to decrypt entry: ") +
                                 dec_result.error().what();
            return protocol::serialize(resp);
        }
        auto upd_result = storage_->update_entry(*dec_result);
        if (!upd_result) {
            protocol::DisableProgramPasswordResponse resp;
            resp.success = false;
            resp.error_message = std::string("failed to update plaintext entry: ") +
                                 upd_result.error().what();
            return protocol::serialize(resp);
        }
    }

    // 2b. 同步重新解密所有生成记录（密文→明文）
    //     失败视为非致命：记录仍可由用户清空，不影响主流程的成功
    //     使用 update_generated_record 保留原始 created_at 时间戳
    {
        auto gen_list = storage_->list_generated_records();
        if (gen_list) {
            for (auto& rec : *gen_list) {
                auto dec = decrypt_generated_record(std::move(rec));
                if (!dec) break;
                (void)storage_->update_generated_record(*dec);
            }
        }
    }

    // 3. 删除 vault.meta
    auto destroy_err = password_store_->destroy();
    if (!destroy_err.ok()) {
        protocol::DisableProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = std::string("failed to delete meta: ") +
                             destroy_err.what();
        return protocol::serialize(resp);
    }

    // 4. 清除内存中的加密密钥，切换到明文模式
    clear_encryption_key();
    password_enabled_ = false;
    unlocked_ = true;
    login_attempts_ = 0;

    protocol::DisableProgramPasswordResponse resp;
    resp.success = true;
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_change_program_password(core::ByteSpan payload) {
    auto req_result = protocol::deserialize<protocol::ChangeProgramPasswordRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed ChangeProgramPasswordRequest: ") +
                              req_result.error().what());
    }
    const auto& req = *req_result;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!password_enabled_) {
        protocol::ChangeProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = "program password is not enabled";
        return protocol::serialize(resp);
    }

    // 直接调用 ProgramPasswordStore::change_password（它内部验证旧密码）
    auto err = password_store_->change_password(req.old_password, req.new_password, *crypto_);
    if (!err.ok()) {
        protocol::ChangeProgramPasswordResponse resp;
        resp.success = false;
        resp.error_message = err.what();
        return protocol::serialize(resp);
    }

    protocol::ChangeProgramPasswordResponse resp;
    resp.success = true;
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_get_vault_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    protocol::GetVaultStatusResponse resp;
    resp.password_enabled = password_enabled_;
    resp.is_locked = password_enabled_ && !unlocked_;
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_add_entry(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::AddEntryRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed AddEntryRequest: ") +
                              req_result.error().what());
    }

    // 必填字段校验：entry_name / account / password
    if (req_result->entry.title.empty() ||
        req_result->entry.account.empty() ||
        req_result->entry.password.empty()) {
        return make_error(core::ErrorCode::InvalidArgument,
                          "title / account / password must not be empty");
    }

    // 保存明文副本用于响应
    core::PasswordEntry plain_entry = req_result->entry;

    // 解析 entry.tags：将 id==0 的新标签按 name 解析为有效 id
    auto resolved_tags = resolve_entry_tags(req_result->entry.tags);
    if (!resolved_tags) {
        return make_error(resolved_tags.error().code, resolved_tags.error().what());
    }
    plain_entry.tags = *resolved_tags;

    // 加密 password 字段（明文模式下为 no-op）；同步标签到存储
    core::PasswordEntry enc_entry = plain_entry;
    auto enc_result = encrypt_entry(std::move(enc_entry));
    if (!enc_result) {
        return make_error(enc_result.error().code, enc_result.error().what());
    }

    auto add_result = storage_->add_entry(*enc_result);
    if (!add_result) {
        return make_error(add_result.error().code, add_result.error().what());
    }

    // 响应中返回明文条目（带分配的 id 与时间戳；tags 来自存储回填）
    plain_entry.id = add_result->id;
    plain_entry.created_at = add_result->created_at;
    plain_entry.updated_at = add_result->updated_at;
    plain_entry.tags = add_result->tags;

    protocol::AddEntryResponse resp;
    resp.entry = std::move(plain_entry);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_update_entry(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::UpdateEntryRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed UpdateEntryRequest: ") +
                              req_result.error().what());
    }

    // 必填字段校验：entry_name / account / password
    if (req_result->entry.title.empty() ||
        req_result->entry.account.empty() ||
        req_result->entry.password.empty()) {
        return make_error(core::ErrorCode::InvalidArgument,
                          "title / account / password must not be empty");
    }

    core::PasswordEntry plain_entry = req_result->entry;

    // 解析新标签（id==0）为有效 id
    auto resolved_tags = resolve_entry_tags(req_result->entry.tags);
    if (!resolved_tags) {
        return make_error(resolved_tags.error().code, resolved_tags.error().what());
    }
    plain_entry.tags = *resolved_tags;

    core::PasswordEntry enc_entry = plain_entry;
    auto enc_result = encrypt_entry(std::move(enc_entry));
    if (!enc_result) {
        return make_error(enc_result.error().code, enc_result.error().what());
    }

    auto upd_result = storage_->update_entry(*enc_result);
    if (!upd_result) {
        return make_error(upd_result.error().code, upd_result.error().what());
    }

    plain_entry.updated_at = upd_result->updated_at;
    plain_entry.tags = upd_result->tags;

    protocol::UpdateEntryResponse resp;
    resp.entry = std::move(plain_entry);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_remove_entry(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::RemoveEntryRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed RemoveEntryRequest: ") +
                              req_result.error().what());
    }
    core::Error err = storage_->remove_entry(req_result->id);
    if (!err.ok()) {
        return make_error(err.code, err.what());
    }
    return protocol::serialize(protocol::RemoveEntryResponse{});
}

core::ByteVec ServiceCore::handle_get_entry(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::GetEntryRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed GetEntryRequest: ") +
                              req_result.error().what());
    }
    auto get_result = storage_->get_entry(req_result->id);
    if (!get_result) {
        return make_error(get_result.error().code, get_result.error().what());
    }
    auto dec_result = decrypt_entry(std::move(*get_result));
    if (!dec_result) {
        return make_error(dec_result.error().code, dec_result.error().what());
    }
    protocol::GetEntryResponse resp;
    resp.entry = std::move(*dec_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_search_entries(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::SearchEntriesRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed SearchEntriesRequest: ") +
                              req_result.error().what());
    }
    core::SearchQuery tag_filter;
    tag_filter.tag_ids = req_result->query.tag_ids;
    auto search_result = storage_->search_entries(tag_filter);
    if (!search_result) {
        return make_error(search_result.error().code, search_result.error().what());
    }
    protocol::SearchEntriesResponse resp;
    resp.entries.reserve(search_result->size());
    for (auto& e : *search_result) {
        auto dec_result = decrypt_entry(std::move(e));
        if (!dec_result) {
            return make_error(dec_result.error().code, dec_result.error().what());
        }
        if (core::item_matches_text_query(*dec_result, req_result->query)) {
            resp.entries.push_back(std::move(*dec_result));
        }
    }
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_list_entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto list_result = storage_->list_entries();
    if (!list_result) {
        return make_error(list_result.error().code, list_result.error().what());
    }
    protocol::ListEntriesResponse resp;
    resp.entries.reserve(list_result->size());
    for (auto& e : *list_result) {
        auto dec_result = decrypt_entry(std::move(e));
        if (!dec_result) {
            return make_error(dec_result.error().code, dec_result.error().what());
        }
        resp.entries.push_back(std::move(*dec_result));
    }
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_generate_password(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::GeneratePasswordRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed GeneratePasswordRequest: ") +
                              req_result.error().what());
    }
    auto gen_result = generator_->generate(req_result->options);
    if (!gen_result) {
        return make_error(gen_result.error().code, gen_result.error().what());
    }

    // 自动追加一条生成记录（失败不影响生成响应）
    core::GeneratedPasswordRecord record;
    record.password = *gen_result;
    record.length = static_cast<int32_t>((*gen_result).size());
    auto enc_rec = encrypt_generated_record(std::move(record));
    if (enc_rec) {
        auto add_err = storage_->add_generated_record(*enc_rec);
        if (add_err.ok()) {
            // 追加成功后执行上限清理（best-effort，失败忽略）
            (void)enforce_generator_limit(current_generator_limit());
        }
    }

    protocol::GeneratePasswordResponse resp;
    resp.password = std::move(*gen_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_estimate_strength(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::EstimateStrengthRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed EstimateStrengthRequest: ") +
                              req_result.error().what());
    }
    protocol::EstimateStrengthResponse resp;
    resp.estimate = generator_->estimate_strength(req_result->password);
    // 请求中的密码不再需要，清零（反序列化的 string 在 req_result 析构时释放，但不清零）
    secure_zero_string(req_result->password);
    return protocol::serialize(resp);
}

// ============================================================================
// 生成器历史记录
// ============================================================================

core::ByteVec ServiceCore::handle_list_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto list_result = storage_->list_generated_records();
    if (!list_result) {
        return make_error(list_result.error().code,
                          std::string("list_generated_records: ") +
                              list_result.error().what());
    }
    protocol::ListGeneratedRecordsResponse resp;
    resp.records.reserve((*list_result).size());
    for (auto& rec : *list_result) {
        auto dec = decrypt_generated_record(std::move(rec));
        if (!dec) {
            // 解密失败的记录跳过，不阻塞整个列表
            continue;
        }
        resp.records.push_back(*dec);
    }
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_remove_generated_record(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::RemoveGeneratedRecordRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed RemoveGeneratedRecordRequest: ") +
                              req_result.error().what());
    }
    auto rm_err = storage_->remove_generated_record(req_result->id);
    if (!rm_err.ok()) {
        return make_error(rm_err.code,
                          std::string("remove_generated_record: ") + rm_err.what());
    }
    return protocol::serialize(protocol::RemoveGeneratedRecordResponse{});
}

core::ByteVec ServiceCore::handle_clear_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto clr_err = storage_->clear_generated_records();
    if (!clr_err.ok()) {
        return make_error(clr_err.code,
                          std::string("clear_generated_records: ") + clr_err.what());
    }
    return protocol::serialize(protocol::ClearGeneratedRecordsResponse{});
}

core::ByteVec ServiceCore::handle_get_generator_settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    protocol::GetGeneratorSettingsResponse resp;
    resp.history_limit = current_generator_limit();
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_set_generator_limit(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::SetGeneratorLimitRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed SetGeneratorLimitRequest: ") +
                              req_result.error().what());
    }
    // 负数视为非法
    if (req_result->limit < 0) {
        return make_error(core::ErrorCode::InvalidArgument,
                          "limit must be >= 0 (0 = unlimited)");
    }
    // 持久化（0 在 set_setting 中被解释为"删除"，此处显式存字符串 "0"）
    auto set_err = storage_->set_setting(
        "generator.history_limit", std::to_string(req_result->limit));
    if (!set_err.ok()) {
        return make_error(set_err.code,
                          std::string("set_setting: ") + set_err.what());
    }
    // 立即执行一次上限清理
    auto enf_err = enforce_generator_limit(req_result->limit);
    if (!enf_err.ok()) {
        return make_error(enf_err.code,
                          std::string("enforce_generator_limit: ") + enf_err.what());
    }
    protocol::SetGeneratorLimitResponse resp;
    resp.success = true;
    return protocol::serialize(resp);
}

// ============================================================================
// 标签（Tag）与 entry-tag 关联
// ============================================================================

core::Result<std::vector<core::Tag>> ServiceCore::resolve_entry_tags(
    const std::vector<core::Tag>& tags) {
    // 调用方持锁
    std::vector<core::Tag> resolved;
    resolved.reserve(tags.size());
    for (const auto& t : tags) {
        if (t.id != 0) {
            resolved.push_back(t);
            continue;
        }
        if (t.name.empty()) {
            // 空名标签静默跳过
            continue;
        }
        // 先查 by name
        auto found = storage_->find_tag_by_name(t.name);
        if (found) {
            resolved.push_back(*found);
            continue;
        }
        if (found.error().code != core::ErrorCode::NotFound) {
            return core::Result<std::vector<core::Tag>>::Err(found.error());
        }
        // 不存在则新建（保留调用方传入的 color）
        auto added = storage_->add_tag(t);
        if (!added) {
            return core::Result<std::vector<core::Tag>>::Err(added.error());
        }
        resolved.push_back(*added);
    }
    return core::Result<std::vector<core::Tag>>::Ok(std::move(resolved));
}

core::ByteVec ServiceCore::handle_add_tag(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::AddTagRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed AddTagRequest: ") +
                              req_result.error().what());
    }
    auto add_result = storage_->add_tag(req_result->tag);
    if (!add_result) {
        return make_error(add_result.error().code, add_result.error().what());
    }
    protocol::AddTagResponse resp;
    resp.tag = std::move(*add_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_update_tag(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::UpdateTagRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed UpdateTagRequest: ") +
                              req_result.error().what());
    }
    auto upd_result = storage_->update_tag(req_result->tag);
    if (!upd_result) {
        return make_error(upd_result.error().code, upd_result.error().what());
    }
    protocol::UpdateTagResponse resp;
    resp.tag = std::move(*upd_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_remove_tag(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::RemoveTagRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed RemoveTagRequest: ") +
                              req_result.error().what());
    }
    auto rm_err = storage_->remove_tag(req_result->id);
    if (!rm_err.ok()) {
        return make_error(rm_err.code, rm_err.what());
    }
    return protocol::serialize(protocol::RemoveTagResponse{});
}

core::ByteVec ServiceCore::handle_list_tags() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto list_result = storage_->list_tags();
    if (!list_result) {
        return make_error(list_result.error().code, list_result.error().what());
    }
    protocol::ListTagsResponse resp;
    resp.tags = std::move(*list_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_get_tag(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::GetTagRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed GetTagRequest: ") +
                              req_result.error().what());
    }
    auto get_result = storage_->get_tag(req_result->id);
    if (!get_result) {
        return make_error(get_result.error().code, get_result.error().what());
    }
    protocol::GetTagResponse resp;
    resp.tag = std::move(*get_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_find_tag_by_name(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::FindTagByNameRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed FindTagByNameRequest: ") +
                              req_result.error().what());
    }
    auto find_result = storage_->find_tag_by_name(req_result->name);
    if (!find_result) {
        return make_error(find_result.error().code, find_result.error().what());
    }
    protocol::FindTagByNameResponse resp;
    resp.tag = std::move(*find_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_get_entry_tags(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::GetEntryTagsRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed GetEntryTagsRequest: ") +
                              req_result.error().what());
    }
    auto get_result = storage_->get_entry_tags(req_result->entry_id);
    if (!get_result) {
        return make_error(get_result.error().code, get_result.error().what());
    }
    protocol::GetEntryTagsResponse resp;
    resp.tags = std::move(*get_result);
    return protocol::serialize(resp);
}

core::ByteVec ServiceCore::handle_set_entry_tags(core::ByteSpan payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unlocked_) {
        return make_error(core::ErrorCode::Unauthorized, "vault is locked");
    }
    auto req_result = protocol::deserialize<protocol::SetEntryTagsRequest>(payload);
    if (!req_result) {
        return make_error(core::ErrorCode::InvalidArgument,
                          std::string("malformed SetEntryTagsRequest: ") +
                              req_result.error().what());
    }
    auto set_err = storage_->set_entry_tags(req_result->entry_id, req_result->tag_ids);
    if (!set_err.ok()) {
        return make_error(set_err.code, set_err.what());
    }
    return protocol::serialize(protocol::SetEntryTagsResponse{});
}

}  // namespace yuli::vault::service
