// coding: utf-8
// =============================================================================
// ICryptoEngine.h
//
// 加密引擎抽象接口。服务进程通过此接口完成对称加密、密钥派生与口令校验。
//
// 约定：
//   - 对称算法：AES-256-GCM
//   - KDF     ：Argon2id
//   - IV      ：12 字节随机数，每次加密重新生成
//   - Tag     ：16 字节 GCM 认证 tag
//
// encrypt() 返回的 ByteVec 约定为 [IV(12) || ciphertext || tag(16)] 三段拼接，
// 长度 = 12 + plaintext.size() + 16；decrypt() 的入参 \p ciphertext 也按此格式解析。
// 采用拼接形式而非结构体，便于序列化进 SQLite BLOB 列与跨进程传输。
// =============================================================================
#pragma once

#include <string>
#include <utility>

#include "Error.h"
#include "Result.h"
#include "Types.h"

namespace yuli::vault::core {

/// 加密引擎抽象接口。
class ICryptoEngine {
public:
    virtual ~ICryptoEngine() = default;

    /// 加密明文。
    /// \param plaintext 待加密的明文字节
    /// \param associated_data 关联数据（AAD，不加密但参与认证；可为空）
    /// \return 成功时返回 [IV || ciphertext || tag] 拼接的字节序列
    virtual Result<ByteVec> encrypt(ByteSpan plaintext,
                                    ByteSpan associated_data = {}) = 0;

    /// 解密密文。
    /// \param ciphertext [IV || ciphertext || tag] 拼接的字节序列
    /// \param associated_data 必须与加密时使用的 AAD 一致（可为空）
    /// \return 成功时返回明文字符串
    virtual Result<std::string> decrypt(ByteSpan ciphertext,
                                        ByteSpan associated_data = {}) = 0;

    /// 由主密码派生密钥（Argon2id）。
    /// \param password 主密码
    /// \param salt 盐值（建议 16 字节，需持久化）
    /// \return 成功时返回派生密钥（32 字节，用于 AES-256）
    virtual Result<ByteVec> derive_key(const std::string& password,
                                       ByteSpan salt) = 0;

    /// 随机生成主密钥与初始 IV（首次初始化新库时使用）。
    /// \return 成功时返回 (key, iv) 对
    virtual Result<std::pair<ByteVec, ByteVec>> generate_key_and_iv() = 0;

    /// 校验主密码。
    /// \param password 待校验的主密码
    /// \param salt 与派生时一致的盐值
    /// \param expected_hash 持久化的期望哈希（派生密钥或其变种）
    /// \return true 表示匹配，false 表示不匹配（不返回 Error，避免与正常失败混淆）
    virtual bool verify_password(const std::string& password,
                                 ByteSpan salt,
                                 ByteSpan expected_hash) = 0;
};

}  // namespace yuli::vault::core
