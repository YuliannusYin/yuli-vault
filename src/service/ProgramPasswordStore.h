// coding: utf-8
// =============================================================================
// ProgramPasswordStore.h
//
// PwdVault 程序密码与加密密钥的持久化管理。
//
// 设计要点：
//   - meta 文件（vault.meta）保存 salt 与被 KEK 加密的 encryption_key。
//   - KEK 由程序密码经 Argon2id 派生，仅存在于内存中，函数返回前清零。
//   - encryption_key 用于加解密 PasswordEntry 的 password 字段，仅在 unlock 成功后
//     存活于 ServiceCore 内存中，lock 或进程退出时清零。
//   - meta 文件存在 <=> 程序密码已启用；不存在 <=> 明文模式（无程序密码）。
//   - meta 文件格式为自定义二进制（magic + version + salt + encrypted_blob），
//     便于跨平台与版本演进。
//
// 依赖：
//   - core::ICryptoEngine：用于 derive_key / generate_key_and_iv
//   - crypto::CryptoEngine：内部构造临时实例以 KEK 加解密 encryption_key
// =============================================================================
#pragma once

#include <filesystem>

#include "core/Error.h"
#include "core/Result.h"
#include "core/Types.h"

namespace yuli::vault::core {
class ICryptoEngine;
}

namespace yuli::vault::service {

/// 程序密码与 encryption_key 持久化存储。
class ProgramPasswordStore {
public:
    /// 构造。
    /// \param meta_path meta 文件路径（通常为 %APPDATA%/PwdVault/vault.meta）
    explicit ProgramPasswordStore(std::filesystem::path meta_path);

    /// 析构。本类不持有敏感数据成员，无需特殊清零。
    ~ProgramPasswordStore();

    ProgramPasswordStore(const ProgramPasswordStore&) = delete;
    ProgramPasswordStore& operator=(const ProgramPasswordStore&) = delete;

    /// 检查 meta 文件是否已存在（即程序密码是否已启用）。
    bool exists() const;

    /// 首次初始化：生成 salt、KEK、encryption_key，将加密后的 encryption_key 写入 meta。
    /// \param program_password 用户程序密码
    /// \param crypto 加密引擎（仅使用 derive_key 与 generate_key_and_iv）
    /// \return 成功时返回明文 encryption_key（32 字节），仅在内存
    core::Result<core::ByteVec> initialize(const std::string& program_password,
                                           core::ICryptoEngine& crypto);

    /// 解锁：读 meta，用 password + salt 派生 KEK，解密 encryption_key。
    /// \param program_password 用户程序密码
    /// \param crypto 加密引擎（仅使用 derive_key）
    /// \return 成功时返回明文 encryption_key；密码错误或 GCM tag 校验失败返回 Unauthorized
    core::Result<core::ByteVec> unlock(const std::string& program_password,
                                       core::ICryptoEngine& crypto);

    /// 修改程序密码：验证旧密码后，用新密码重新包装 encryption_key。
    /// encryption_key 本身不变，条目无需重新加密。
    /// \param old_password 旧程序密码
    /// \param new_password 新程序密码
    /// \param crypto 加密引擎（仅使用 derive_key）
    /// \return 成功时返回 Ok；旧密码错误返回 Unauthorized
    core::Error change_password(const std::string& old_password,
                                const std::string& new_password,
                                core::ICryptoEngine& crypto);

    /// 删除 meta 文件（禁用程序密码时调用）。
    /// 调用前应已验证程序密码并解密所有条目为明文。
    core::Error destroy();

private:
    std::filesystem::path meta_path_;
};

}  // namespace yuli::vault::service
