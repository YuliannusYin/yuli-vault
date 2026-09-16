// coding: utf-8
// =============================================================================
// ProgramPasswordStore.cpp
//
// meta 文件二进制格式：
//   offset  size      field
//   0       4         magic       = 0x4D4B5650 ('P','V','K','M' little-endian)
//   4       2         version     = 1
//   6       4         salt_len    = 16
//   10      16        salt
//   26      4         blob_len    = 60 (12 IV + 32 ciphertext + 16 tag)
//   30      blob_len  encrypted_encryption_key_blob  = [IV || ciphertext || tag]
// =============================================================================
#include "ProgramPasswordStore.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "CryptoEngine.h"
#include "ICryptoEngine.h"
#include "Types.h"

#include <sodium.h>

namespace yuli::vault::service {

namespace {

constexpr uint32_t kMetaMagic = 0x4D4B5650u;  // 'P','V','K','M' little-endian
constexpr uint16_t kMetaVersion = 1;
constexpr size_t kSaltLen = 16;
constexpr size_t kEncryptionKeyLen = 32;

/// meta 文件在内存中的解析结果。
struct MetaRecord {
    core::ByteVec salt;
    core::ByteVec encrypted_blob;  // [IV || ciphertext || tag]
};

/// 小端写入 POD 值到字节向量。
template <typename T>
void write_pod(std::ofstream& out, T value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

/// 从输入流读取 POD 值。
template <typename T>
bool read_pod(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

/// 安全清零 ByteVec。
void secure_zero(core::ByteVec& v) {
    if (!v.empty()) {
        sodium_memzero(v.data(), v.size());
    }
}

core::Result<MetaRecord> read_meta(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError,
                        "meta file open failed: " + path.string()));
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint32_t salt_len = 0;
    if (!read_pod(f, magic) || !read_pod(f, version) || !read_pod(f, salt_len)) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file truncated (header)"));
    }
    if (magic != kMetaMagic) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file magic mismatch"));
    }
    if (version != kMetaVersion) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError,
                        "meta file version unsupported: " + std::to_string(version)));
    }
    if (salt_len != kSaltLen) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError,
                        "meta file salt length unexpected: " + std::to_string(salt_len)));
    }

    MetaRecord rec;
    rec.salt.resize(kSaltLen);
    if (!f.read(reinterpret_cast<char*>(rec.salt.data()), kSaltLen)) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file truncated (salt)"));
    }

    uint32_t blob_len = 0;
    if (!read_pod(f, blob_len)) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file truncated (blob_len)"));
    }
    // 粗略上界保护：encryption_key 加密后最长 12 + N + 16，N 不会超过 64KB
    if (blob_len > 65536) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file blob too large"));
    }
    rec.encrypted_blob.resize(blob_len);
    if (blob_len > 0 && !f.read(reinterpret_cast<char*>(rec.encrypted_blob.data()),
                                static_cast<std::streamsize>(blob_len))) {
        return core::Result<MetaRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "meta file truncated (blob)"));
    }

    return core::Result<MetaRecord>::Ok(std::move(rec));
}

core::Error write_meta(const std::filesystem::path& path, const MetaRecord& rec) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return core::Error(core::ErrorCode::StorageError,
                           "meta file create failed: " + path.string());
    }
    write_pod(f, kMetaMagic);
    write_pod(f, kMetaVersion);
    write_pod(f, static_cast<uint32_t>(rec.salt.size()));
    f.write(reinterpret_cast<const char*>(rec.salt.data()),
            static_cast<std::streamsize>(rec.salt.size()));
    write_pod(f, static_cast<uint32_t>(rec.encrypted_blob.size()));
    f.write(reinterpret_cast<const char*>(rec.encrypted_blob.data()),
            static_cast<std::streamsize>(rec.encrypted_blob.size()));
    if (!f) {
        return core::Error(core::ErrorCode::StorageError, "meta file write failed");
    }
    return core::Error{};
}

}  // namespace

ProgramPasswordStore::ProgramPasswordStore(std::filesystem::path meta_path)
    : meta_path_(std::move(meta_path)) {}

ProgramPasswordStore::~ProgramPasswordStore() = default;

bool ProgramPasswordStore::exists() const {
    std::error_code ec;
    return std::filesystem::exists(meta_path_, ec);
}

core::Result<core::ByteVec> ProgramPasswordStore::initialize(const std::string& program_password,
                                                              core::ICryptoEngine& crypto) {
    if (exists()) {
        return core::Result<core::ByteVec>::Err(
            core::Error(core::ErrorCode::AlreadyExists,
                        "program password already enabled"));
    }

    // 1. 生成 16 字节 salt
    core::ByteVec salt(kSaltLen);
    randombytes_buf(salt.data(), kSaltLen);

    // 2. 用 password + salt 派生 KEK（32 字节）
    auto kek_result = crypto.derive_key(program_password, salt);
    if (!kek_result) {
        return core::Result<core::ByteVec>::Err(kek_result.error());
    }
    core::ByteVec kek = std::move(kek_result).value();

    // RAII：确保 KEK 在所有退出路径上清零
    struct KekZeroer {
        core::ByteVec& v;
        ~KekZeroer() { secure_zero(v); }
    } kek_zeroer{kek};

    // 3. 生成 32 字节 encryption_key
    auto gen_result = crypto.generate_key_and_iv();
    if (!gen_result) {
        return core::Result<core::ByteVec>::Err(gen_result.error());
    }
    core::ByteVec encryption_key = std::move(gen_result->first);

    // 4. 用 KEK 构造临时 CryptoEngine，加密 encryption_key
    crypto::CryptoEngine kek_engine(kek);
    auto enc_result = kek_engine.encrypt(encryption_key);
    if (!enc_result) {
        secure_zero(encryption_key);
        return core::Result<core::ByteVec>::Err(enc_result.error());
    }

    // 5. 写入 meta 文件
    MetaRecord rec;
    rec.salt = salt;
    rec.encrypted_blob = std::move(*enc_result);
    core::Error err = write_meta(meta_path_, rec);
    if (!err.ok()) {
        secure_zero(encryption_key);
        return core::Result<core::ByteVec>::Err(err);
    }

    return core::Result<core::ByteVec>::Ok(std::move(encryption_key));
}

core::Result<core::ByteVec> ProgramPasswordStore::unlock(const std::string& program_password,
                                                          core::ICryptoEngine& crypto) {
    auto meta_result = read_meta(meta_path_);
    if (!meta_result) {
        return core::Result<core::ByteVec>::Err(meta_result.error());
    }
    const MetaRecord& rec = *meta_result;

    // 1. 用 password + salt 派生 KEK
    auto kek_result = crypto.derive_key(program_password, rec.salt);
    if (!kek_result) {
        return core::Result<core::ByteVec>::Err(kek_result.error());
    }
    core::ByteVec kek = std::move(kek_result).value();

    struct KekZeroer {
        core::ByteVec& v;
        ~KekZeroer() { secure_zero(v); }
    } kek_zeroer{kek};

    // 2. 用 KEK 构造临时 CryptoEngine，解密 encryption_key
    crypto::CryptoEngine kek_engine(kek);
    auto dec_result = kek_engine.decrypt(rec.encrypted_blob);
    if (!dec_result) {
        // GCM tag 校验失败 → 程序密码错误
        return core::Result<core::ByteVec>::Err(
            core::Error(core::ErrorCode::Unauthorized,
                        "program password incorrect"));
    }

    // 3. 校验长度并转回 ByteVec
    const auto* mk_ptr = reinterpret_cast<const std::byte*>(dec_result->data());
    core::ByteVec encryption_key(mk_ptr, mk_ptr + dec_result->size());
    if (encryption_key.size() != kEncryptionKeyLen) {
        secure_zero(encryption_key);
        return core::Result<core::ByteVec>::Err(
            core::Error(core::ErrorCode::CryptoError,
                        "decrypted encryption key length mismatch"));
    }

    return core::Result<core::ByteVec>::Ok(std::move(encryption_key));
}

core::Error ProgramPasswordStore::change_password(const std::string& old_password,
                                                   const std::string& new_password,
                                                   core::ICryptoEngine& crypto) {
    // 1. 读取现有 meta 并用旧密码解密 encryption_key
    auto meta_result = read_meta(meta_path_);
    if (!meta_result) {
        return meta_result.error();
    }
    const MetaRecord& old_rec = *meta_result;

    auto old_kek_result = crypto.derive_key(old_password, old_rec.salt);
    if (!old_kek_result) {
        return old_kek_result.error();
    }
    core::ByteVec old_kek = std::move(old_kek_result).value();

    struct KekZeroer {
        core::ByteVec& v;
        ~KekZeroer() { secure_zero(v); }
    } old_kek_zeroer{old_kek};

    crypto::CryptoEngine old_kek_engine(old_kek);
    auto dec_result = old_kek_engine.decrypt(old_rec.encrypted_blob);
    if (!dec_result) {
        return core::Error(core::ErrorCode::Unauthorized,
                           "old program password incorrect");
    }

    const auto* ek_ptr = reinterpret_cast<const std::byte*>(dec_result->data());
    core::ByteVec encryption_key(ek_ptr, ek_ptr + dec_result->size());
    if (encryption_key.size() != kEncryptionKeyLen) {
        secure_zero(encryption_key);
        return core::Error(core::ErrorCode::CryptoError,
                           "decrypted encryption key length mismatch");
    }

    struct EkZeroer {
        core::ByteVec& v;
        ~EkZeroer() { secure_zero(v); }
    } ek_zeroer{encryption_key};

    // 2. 生成新 salt 并用新密码派生新 KEK
    core::ByteVec new_salt(kSaltLen);
    randombytes_buf(new_salt.data(), kSaltLen);

    auto new_kek_result = crypto.derive_key(new_password, new_salt);
    if (!new_kek_result) {
        return new_kek_result.error();
    }
    core::ByteVec new_kek = std::move(new_kek_result).value();

    struct NewKekZeroer {
        core::ByteVec& v;
        ~NewKekZeroer() { secure_zero(v); }
    } new_kek_zeroer{new_kek};

    // 3. 用新 KEK 重新加密 encryption_key
    crypto::CryptoEngine new_kek_engine(new_kek);
    auto enc_result = new_kek_engine.encrypt(encryption_key);
    if (!enc_result) {
        return enc_result.error();
    }

    // 4. 覆写 meta 文件
    MetaRecord new_rec;
    new_rec.salt = new_salt;
    new_rec.encrypted_blob = std::move(*enc_result);
    return write_meta(meta_path_, new_rec);
}

core::Error ProgramPasswordStore::destroy() {
    std::error_code ec;
    if (!std::filesystem::exists(meta_path_, ec)) {
        return core::Error{};  // 已不存在，视为成功
    }
    std::filesystem::remove(meta_path_, ec);
    if (ec) {
        return core::Error(core::ErrorCode::StorageError,
                           "failed to delete meta file: " + ec.message());
    }
    return core::Error{};
}

}  // namespace yuli::vault::service
