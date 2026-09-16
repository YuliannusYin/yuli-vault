// coding: utf-8
// =============================================================================
// CryptoEngine.cpp
//
// PwdVault 加密引擎实现：
//   - AES-256-GCM 加解密：使用 OpenSSL EVP 接口
//   - Argon2id 密钥派生：使用 libsodium crypto_pwhash_argon2id
//   - 随机数：使用 libsodium randombytes_buf
//   - 敏感内存清零：使用 libsodium sodium_memzero
//
// 输出格式约定：
//   - encrypt() 返回 [IV(12) || ciphertext || tag(16)]，长度 = 12 + N + 16
//   - decrypt() 入参为同样格式
// =============================================================================
#include "CryptoEngine.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <sodium.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace yuli::vault::crypto {

namespace {

// ============================================================================
// OpenSSL EVP_CIPHER_CTX 的 RAII 包装
// ============================================================================
struct EvpCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
using EvpCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCtxDeleter>;

// 将只读 ByteSpan 的指针转换为 OpenSSL 期望的 const unsigned char*
const unsigned char* to_uchar_ptr(core::ByteSpan span) {
    return reinterpret_cast<const unsigned char*>(span.data());
}

// 将可写 ByteVec 的指针转换为 OpenSSL 期望的 unsigned char*
unsigned char* to_uchar_ptr(core::ByteVec& vec) {
    return reinterpret_cast<unsigned char*>(vec.data());
}

// 构造 CryptoError（无 OpenSSL 上下文）
core::Error make_crypto_error(std::string msg) {
    return core::Error{core::ErrorCode::CryptoError, std::move(msg)};
}

// 构造 CryptoError，附加 OpenSSL 错误队列中的最新错误信息
core::Error make_crypto_error_ossl(std::string_view prefix) {
    unsigned long err = ERR_get_error();
    char buf[256] = {0};
    ERR_error_string_n(err, buf, sizeof(buf));
    std::string msg{prefix};
    if (buf[0] != '\0') {
        msg += ": ";
        msg += buf;
    }
    return core::Error{core::ErrorCode::CryptoError, std::move(msg)};
}

// 算法常量
constexpr int kIvLen = 12;       // GCM 推荐 IV 长度
constexpr int kTagLen = 16;      // GCM 认证 tag 长度
constexpr size_t kKeyLen = 32;   // AES-256 加密密钥长度

}  // namespace

// ============================================================================
// 构造与析构
// ============================================================================

CryptoEngine::CryptoEngine(core::ByteSpan encryption_key)
    : encryption_key_(encryption_key.begin(), encryption_key.end()) {
    // libsodium 在首次使用前必须初始化；多次调用幂等且线程安全
    sodium_init();
}

CryptoEngine::~CryptoEngine() {
    if (!encryption_key_.empty()) {
        sodium_memzero(encryption_key_.data(), encryption_key_.size());
    }
}

// ============================================================================
// encrypt
// ============================================================================

core::Result<core::ByteVec> CryptoEngine::encrypt(core::ByteSpan plaintext,
                                                   core::ByteSpan associated_data) {
    if (encryption_key_.size() != kKeyLen) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error("encryption key length must be 32 bytes"));
    }

    // 1. 生成随机 IV
    core::ByteVec iv(kIvLen);
    randombytes_buf(iv.data(), kIvLen);

    // 2. 创建并初始化 EVP 上下文
    EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_new failed"));
    }

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_EncryptInit_ex (cipher) failed"));
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) != 1) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_ctrl (set IV len) failed"));
    }

    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                          reinterpret_cast<const unsigned char*>(encryption_key_.data()),
                          reinterpret_cast<const unsigned char*>(iv.data())) != 1) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_EncryptInit_ex (key/iv) failed"));
    }

    // 3. 处理 AAD（如果非空）。AAD 不产生密文输出，out 参数传 nullptr
    int out_len = 0;
    if (!associated_data.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), nullptr, &out_len,
                              to_uchar_ptr(associated_data),
                              static_cast<int>(associated_data.size())) != 1) {
            return core::Result<core::ByteVec>::Err(
                make_crypto_error_ossl("EVP_EncryptUpdate (AAD) failed"));
        }
    }

    // 4. 加密明文。GCM 是流式加密，输出长度等于输入长度
    core::ByteVec ciphertext(plaintext.size());
    int cipher_len = 0;
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(ctx.get(), to_uchar_ptr(ciphertext), &cipher_len,
                              to_uchar_ptr(plaintext),
                              static_cast<int>(plaintext.size())) != 1) {
            return core::Result<core::ByteVec>::Err(
                make_crypto_error_ossl("EVP_EncryptUpdate (plaintext) failed"));
        }
    }

    // 5. Finalize。GCM 无填充，final_len 应为 0
    //    即便 plaintext 为空，也要传一个有效指针避免 nullptr 解引用风险
    unsigned char dummy = 0;
    unsigned char* final_out = &dummy;
    if (!ciphertext.empty()) {
        final_out = to_uchar_ptr(ciphertext) + cipher_len;
    }
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), final_out, &final_len) != 1) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_EncryptFinal_ex failed"));
    }

    // 6. 获取 GCM tag
    std::array<unsigned char, kTagLen> tag{};
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, kTagLen, tag.data()) != 1) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_ctrl (get tag) failed"));
    }

    // 7. 拼接输出：[IV || ciphertext || tag]
    const size_t total_cipher_len =
        static_cast<size_t>(cipher_len) + static_cast<size_t>(final_len);
    core::ByteVec output;
    output.reserve(kIvLen + total_cipher_len + kTagLen);
    output.insert(output.end(), iv.begin(), iv.end());
    output.insert(output.end(), ciphertext.begin(),
                  ciphertext.begin() + static_cast<std::ptrdiff_t>(total_cipher_len));
    output.insert(output.end(),
                  reinterpret_cast<std::byte*>(tag.data()),
                  reinterpret_cast<std::byte*>(tag.data() + kTagLen));

    return core::Result<core::ByteVec>::Ok(std::move(output));
}

// ============================================================================
// decrypt
// ============================================================================

core::Result<std::string> CryptoEngine::decrypt(core::ByteSpan ciphertext,
                                                 core::ByteSpan associated_data) {
    if (encryption_key_.size() != kKeyLen) {
        return core::Result<std::string>::Err(
            make_crypto_error("encryption key length must be 32 bytes"));
    }

    if (ciphertext.size() < static_cast<size_t>(kIvLen) + static_cast<size_t>(kTagLen)) {
        return core::Result<std::string>::Err(
            make_crypto_error("ciphertext too short (need at least 28 bytes)"));
    }

    // 1. 解析输入：[IV(12) || ciphertext || tag(16)]
    const std::byte* iv_ptr = ciphertext.data();
    const size_t ct_len =
        ciphertext.size() - static_cast<size_t>(kIvLen) - static_cast<size_t>(kTagLen);
    const std::byte* ct_ptr = ciphertext.data() + kIvLen;
    const std::byte* tag_ptr = ct_ptr + ct_len;

    // 2. 创建并初始化 EVP 上下文
    EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return core::Result<std::string>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_new failed"));
    }

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return core::Result<std::string>::Err(
            make_crypto_error_ossl("EVP_DecryptInit_ex (cipher) failed"));
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) != 1) {
        return core::Result<std::string>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_ctrl (set IV len) failed"));
    }

    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                          reinterpret_cast<const unsigned char*>(encryption_key_.data()),
                          reinterpret_cast<const unsigned char*>(iv_ptr)) != 1) {
        return core::Result<std::string>::Err(
            make_crypto_error_ossl("EVP_DecryptInit_ex (key/iv) failed"));
    }

    // 3. 处理 AAD（必须与加密时一致）
    int out_len = 0;
    if (!associated_data.empty()) {
        if (EVP_DecryptUpdate(ctx.get(), nullptr, &out_len,
                              to_uchar_ptr(associated_data),
                              static_cast<int>(associated_data.size())) != 1) {
            return core::Result<std::string>::Err(
                make_crypto_error_ossl("EVP_DecryptUpdate (AAD) failed"));
        }
    }

    // 4. 解密密文（输出长度等于 ct_len）
    std::string plaintext;
    plaintext.resize(ct_len);
    int plain_len = 0;
    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx.get(),
                              reinterpret_cast<unsigned char*>(plaintext.data()),
                              &plain_len,
                              reinterpret_cast<const unsigned char*>(ct_ptr),
                              static_cast<int>(ct_len)) != 1) {
            sodium_memzero(plaintext.data(), plaintext.size());
            return core::Result<std::string>::Err(
                make_crypto_error_ossl("EVP_DecryptUpdate (ciphertext) failed"));
        }
    }

    // 5. 设置期望的 tag（必须拷贝到独立缓冲区，避免别名问题）
    std::array<unsigned char, kTagLen> tag_buf{};
    std::memcpy(tag_buf.data(), reinterpret_cast<const unsigned char*>(tag_ptr), kTagLen);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, kTagLen, tag_buf.data()) != 1) {
        sodium_memzero(plaintext.data(), plaintext.size());
        return core::Result<std::string>::Err(
            make_crypto_error_ossl("EVP_CIPHER_CTX_ctrl (set tag) failed"));
    }

    // 6. Finalize：验证 tag。失败时返回 0，表示认证失败
    unsigned char dummy = 0;
    unsigned char* final_out = &dummy;
    if (!plaintext.empty()) {
        final_out = reinterpret_cast<unsigned char*>(plaintext.data()) + plain_len;
    }
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), final_out, &final_len) != 1) {
        // tag 验证失败：清零已部分解密的内容，避免泄露
        sodium_memzero(plaintext.data(), plaintext.size());
        return core::Result<std::string>::Err(
            make_crypto_error("authentication failed"));
    }

    plaintext.resize(static_cast<size_t>(plain_len) + static_cast<size_t>(final_len));
    return core::Result<std::string>::Ok(std::move(plaintext));
}

// ============================================================================
// derive_key
// ============================================================================

core::Result<core::ByteVec> CryptoEngine::derive_key(const std::string& password,
                                                      core::ByteSpan salt) {
    if (salt.size() < crypto_pwhash_argon2id_SALTBYTES) {
        return core::Result<core::ByteVec>::Err(
            make_crypto_error("salt too short (need at least " +
                              std::to_string(crypto_pwhash_argon2id_SALTBYTES) +
                              " bytes)"));
    }

    core::ByteVec key(kKeyLen);
    if (crypto_pwhash_argon2id(
            reinterpret_cast<unsigned char*>(key.data()),
            static_cast<unsigned long long>(key.size()),
            password.data(),
            static_cast<unsigned long long>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_argon2id_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_argon2id_ALG_ARGON2ID13) != 0) {
        sodium_memzero(key.data(), key.size());
        return core::Result<core::ByteVec>::Err(
            make_crypto_error("Argon2id key derivation failed (out of memory?)"));
    }

    return core::Result<core::ByteVec>::Ok(std::move(key));
}

// ============================================================================
// generate_key_and_iv
// ============================================================================

core::Result<std::pair<core::ByteVec, core::ByteVec>> CryptoEngine::generate_key_and_iv() {
    core::ByteVec key(kKeyLen);
    core::ByteVec iv(static_cast<size_t>(kIvLen));

    randombytes_buf(key.data(), kKeyLen);
    randombytes_buf(iv.data(), static_cast<size_t>(kIvLen));

    return core::Result<std::pair<core::ByteVec, core::ByteVec>>::Ok(
        std::make_pair(std::move(key), std::move(iv)));
}

// ============================================================================
// verify_password
// ============================================================================

bool CryptoEngine::verify_password(const std::string& password,
                                    core::ByteSpan salt,
                                    core::ByteSpan expected_hash) {
    if (salt.size() < crypto_pwhash_argon2id_SALTBYTES) {
        return false;
    }
    if (expected_hash.size() != kKeyLen) {
        return false;
    }

    core::ByteVec derived(kKeyLen);
    if (crypto_pwhash_argon2id(
            reinterpret_cast<unsigned char*>(derived.data()),
            static_cast<unsigned long long>(derived.size()),
            password.data(),
            static_cast<unsigned long long>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_argon2id_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_argon2id_ALG_ARGON2ID13) != 0) {
        sodium_memzero(derived.data(), derived.size());
        return false;
    }

    // 常量时间比较，避免侧信道泄露
    int match = sodium_memcmp(derived.data(),
                             reinterpret_cast<const unsigned char*>(expected_hash.data()),
                             derived.size());
    sodium_memzero(derived.data(), derived.size());
    return match == 0;
}

}  // namespace yuli::vault::crypto
