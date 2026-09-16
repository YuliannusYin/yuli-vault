// coding: utf-8
// =============================================================================
// CryptoEngine.h
//
// PwdVault 加密引擎实现（AES-256-GCM + Argon2id）。
//
// 设计要点：
//   - 加密密钥（32 字节）在构造时拷贝入对象，析构时用 sodium_memzero 清零。
//   - OpenSSL 资源用 unique_ptr + 自定义 deleter 进行 RAII 管理。
//   - 不可拷贝、不可移动（避免加密密钥在堆中被多次复制散落）。
//
// 依赖：
//   - OpenSSL 1.1+ 的 EVP 接口（AES-256-GCM）
//   - libsodium 1.0.13+ 的 crypto_pwhash_argon2id / randombytes_buf / sodium_memzero
// =============================================================================
#pragma once

#include <string>
#include <utility>

#include "ICryptoEngine.h"
#include "Types.h"

namespace yuli::vault::crypto {

/// 加密引擎实现。基于 AES-256-GCM（OpenSSL EVP）与 Argon2id（libsodium）。
class CryptoEngine : public core::ICryptoEngine {
public:
    /// 构造加密引擎。
    /// \param encryption_key 加密密钥（必须为 32 字节，用于 AES-256）；非 32 字节时
    ///                       对象仍可构造，但 encrypt/decrypt 会返回 CryptoError
    explicit CryptoEngine(core::ByteSpan encryption_key);

    CryptoEngine(const CryptoEngine&) = delete;
    CryptoEngine& operator=(const CryptoEngine&) = delete;
    CryptoEngine(CryptoEngine&&) = delete;
    CryptoEngine& operator=(CryptoEngine&&) = delete;

    ~CryptoEngine() override;

    /// 加密明文，返回 [IV(12) || ciphertext || tag(16)]。
    core::Result<core::ByteVec> encrypt(core::ByteSpan plaintext,
                                        core::ByteSpan associated_data = {}) override;

    /// 解密 [IV || ciphertext || tag]，返回明文字符串；tag 校验失败返回 CryptoError。
    core::Result<std::string> decrypt(core::ByteSpan ciphertext,
                                      core::ByteSpan associated_data = {}) override;

    /// 由程序密码派生 32 字节密钥（Argon2id，INTERACTIVE 参数）。
    /// salt 长度不足 16 字节时返回错误。
    core::Result<core::ByteVec> derive_key(const std::string& password,
                                           core::ByteSpan salt) override;

    /// 随机生成加密密钥（32 字节）与 IV（12 字节）。
    core::Result<std::pair<core::ByteVec, core::ByteVec>> generate_key_and_iv() override;

    /// 校验程序密码：用相同 Argon2id 参数派生 hash，再用 sodium_memcmp 常量时间比较。
    /// 任何参数非法（salt 过短、expected_hash 长度不是 32 字节、派生失败）均返回 false。
    bool verify_password(const std::string& password,
                         core::ByteSpan salt,
                         core::ByteSpan expected_hash) override;

private:
    core::ByteVec encryption_key_;
};

}  // namespace yuli::vault::crypto
