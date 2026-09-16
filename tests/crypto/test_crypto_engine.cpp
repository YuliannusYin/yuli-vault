// coding: utf-8
// =============================================================================
// test_crypto_engine.cpp
//
// PwdVault 加密引擎 GoogleTest 单元测试。覆盖：
//   - 加密/解密往返（带与不带 AAD、空明文）
//   - 篡改密文/Tag 后解密失败（GCM 认证）
//   - 不同加密密钥解密失败
//   - 错误长度的 salt 派生密钥失败
//   - 相同输入派生相同密钥；不同密码派生不同密钥
//   - generate_key_and_iv 返回正确长度且随机
//   - verify_password 正确/错误密码、错误 salt 长度
// =============================================================================
#include "CryptoEngine.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

constexpr size_t kKeyLen = 32;
constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kSaltLen = crypto_pwhash_argon2id_SALTBYTES;  // 16

yuli::vault::core::ByteVec bytes_from_string(const std::string& s) {
    return yuli::vault::core::ByteVec(
        reinterpret_cast<const std::byte*>(s.data()),
        reinterpret_cast<const std::byte*>(s.data()) + s.size());
}

yuli::vault::core::ByteVec random_bytes(size_t n) {
    yuli::vault::core::ByteVec v(n);
    randombytes_buf(v.data(), n);
    return v;
}

}  // namespace

class CryptoEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        encryption_key_ = random_bytes(kKeyLen);
    }
    yuli::vault::core::ByteVec encryption_key_;
};

// ---------------------------------------------------------------------------
// 加密/解密往返
// ---------------------------------------------------------------------------

TEST_F(CryptoEngineTest, EncryptDecryptRoundtripNoAAD) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Hello, PwdVault!";

    auto enc = engine.encrypt(bytes_from_string(plaintext));
    ASSERT_TRUE(enc.ok()) << enc.error().what();
    // 输出长度 = IV(12) + plaintext + tag(16)
    EXPECT_EQ(enc.value().size(), kIvLen + plaintext.size() + kTagLen);

    auto dec = engine.decrypt(enc.value());
    ASSERT_TRUE(dec.ok()) << dec.error().what();
    EXPECT_EQ(dec.value(), plaintext);
}

TEST_F(CryptoEngineTest, EncryptDecryptRoundtripWithAAD) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Secret message with AAD";
    auto aad = bytes_from_string("associated-data-12345");

    auto enc = engine.encrypt(bytes_from_string(plaintext), aad);
    ASSERT_TRUE(enc.ok()) << enc.error().what();

    auto dec = engine.decrypt(enc.value(), aad);
    ASSERT_TRUE(dec.ok()) << dec.error().what();
    EXPECT_EQ(dec.value(), plaintext);
}

TEST_F(CryptoEngineTest, DecryptWithMismatchedAADFails) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Secret message";
    auto aad = bytes_from_string("correct-aad");
    auto wrong_aad = bytes_from_string("wrong-aad");

    auto enc = engine.encrypt(bytes_from_string(plaintext), aad);
    ASSERT_TRUE(enc.ok());

    auto dec = engine.decrypt(enc.value(), wrong_aad);
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

TEST_F(CryptoEngineTest, EmptyPlaintextRoundtrip) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    yuli::vault::core::ByteVec empty;

    auto enc = engine.encrypt(empty);
    ASSERT_TRUE(enc.ok()) << enc.error().what();
    // IV(12) + tag(16) = 28，无密文体
    EXPECT_EQ(enc.value().size(), kIvLen + kTagLen);

    auto dec = engine.decrypt(enc.value());
    ASSERT_TRUE(dec.ok()) << dec.error().what();
    EXPECT_TRUE(dec.value().empty());
}

TEST_F(CryptoEngineTest, EncryptProducesDifferentCiphertextsForSamePlaintext) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Same plaintext, different IVs";

    auto enc1 = engine.encrypt(bytes_from_string(plaintext));
    auto enc2 = engine.encrypt(bytes_from_string(plaintext));
    ASSERT_TRUE(enc1.ok());
    ASSERT_TRUE(enc2.ok());
    // 因 IV 随机，两次加密结果应不同
    EXPECT_NE(enc1.value(), enc2.value());
}

// ---------------------------------------------------------------------------
// 篡改与认证失败
// ---------------------------------------------------------------------------

TEST_F(CryptoEngineTest, TamperedCiphertextFailsDecryption) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Tamper test payload";

    auto enc = engine.encrypt(bytes_from_string(plaintext));
    ASSERT_TRUE(enc.ok());

    yuli::vault::core::ByteVec tampered = enc.value();
    // 翻转密文中段的一个字节（位于 IV 之后、tag 之前）
    ASSERT_GT(tampered.size(), 20u);
    const size_t idx = 15;
    tampered[idx] = static_cast<std::byte>(
        static_cast<unsigned char>(tampered[idx]) ^ 0xFF);

    auto dec = engine.decrypt(tampered);
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

TEST_F(CryptoEngineTest, TamperedTagFailsDecryption) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    const std::string plaintext = "Tag tamper test";

    auto enc = engine.encrypt(bytes_from_string(plaintext));
    ASSERT_TRUE(enc.ok());

    yuli::vault::core::ByteVec tampered = enc.value();
    // 翻转最后一个字节（属于 tag）
    const size_t idx = tampered.size() - 1;
    tampered[idx] = static_cast<std::byte>(
        static_cast<unsigned char>(tampered[idx]) ^ 0xFF);

    auto dec = engine.decrypt(tampered);
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

TEST_F(CryptoEngineTest, ShortCiphertextFailsDecryption) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    yuli::vault::core::ByteVec short_ct(10);  // < IV(12) + tag(16)
    auto dec = engine.decrypt(short_ct);
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

TEST_F(CryptoEngineTest, DecryptWithDifferentEncryptionKeyFails) {
    yuli::vault::crypto::CryptoEngine engine1(encryption_key_);
    auto other_key = random_bytes(kKeyLen);
    yuli::vault::crypto::CryptoEngine engine2(other_key);

    const std::string plaintext = "Cross-key test";
    auto enc = engine1.encrypt(bytes_from_string(plaintext));
    ASSERT_TRUE(enc.ok());

    auto dec = engine2.decrypt(enc.value());
    ASSERT_FALSE(dec.ok());
    EXPECT_EQ(dec.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

// ---------------------------------------------------------------------------
// 密钥派生
// ---------------------------------------------------------------------------

TEST_F(CryptoEngineTest, DeriveKeyFailsWithShortSalt) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    yuli::vault::core::ByteVec short_salt(8);  // < 16
    auto result = engine.derive_key("password123", short_salt);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, yuli::vault::core::ErrorCode::CryptoError);
}

TEST_F(CryptoEngineTest, DeriveKeyProduces32Bytes) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    auto result = engine.derive_key("mypassword", salt);
    ASSERT_TRUE(result.ok()) << result.error().what();
    EXPECT_EQ(result.value().size(), kKeyLen);
}

TEST_F(CryptoEngineTest, DeriveKeyIsDeterministic) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    auto r1 = engine.derive_key("mypassword", salt);
    auto r2 = engine.derive_key("mypassword", salt);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r1.value(), r2.value());
}

TEST_F(CryptoEngineTest, DeriveKeyDiffersForDifferentPasswords) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    auto r1 = engine.derive_key("password-A", salt);
    auto r2 = engine.derive_key("password-B", salt);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    EXPECT_NE(r1.value(), r2.value());
}

// ---------------------------------------------------------------------------
// generate_key_and_iv
// ---------------------------------------------------------------------------

TEST_F(CryptoEngineTest, GenerateKeyAndIvReturnsCorrectLengths) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto result = engine.generate_key_and_iv();
    ASSERT_TRUE(result.ok()) << result.error().what();
    EXPECT_EQ(result.value().first.size(), kKeyLen);
    EXPECT_EQ(result.value().second.size(), kIvLen);
}

TEST_F(CryptoEngineTest, GenerateKeyAndIvProducesRandomOutput) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto r1 = engine.generate_key_and_iv();
    auto r2 = engine.generate_key_and_iv();
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    // 两次生成应不同（极小概率相同）
    EXPECT_NE(r1.value().first, r2.value().first);
    EXPECT_NE(r1.value().second, r2.value().second);
}

// ---------------------------------------------------------------------------
// verify_password
// ---------------------------------------------------------------------------

TEST_F(CryptoEngineTest, VerifyPasswordCorrectReturnsTrue) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    auto hash = engine.derive_key("correct-password", salt);
    ASSERT_TRUE(hash.ok());

    EXPECT_TRUE(engine.verify_password("correct-password", salt, hash.value()));
}

TEST_F(CryptoEngineTest, VerifyPasswordWrongReturnsFalse) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    auto hash = engine.derive_key("correct-password", salt);
    ASSERT_TRUE(hash.ok());

    EXPECT_FALSE(engine.verify_password("wrong-password", salt, hash.value()));
}

TEST_F(CryptoEngineTest, VerifyPasswordWithShortSaltReturnsFalse) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    yuli::vault::core::ByteVec short_salt(8);
    yuli::vault::core::ByteVec dummy_hash(kKeyLen);
    EXPECT_FALSE(engine.verify_password("any-password", short_salt, dummy_hash));
}

TEST_F(CryptoEngineTest, VerifyPasswordWithWrongHashLengthReturnsFalse) {
    yuli::vault::crypto::CryptoEngine engine(encryption_key_);
    auto salt = random_bytes(kSaltLen);
    yuli::vault::core::ByteVec bad_hash(16);  // 期望 32 字节
    EXPECT_FALSE(engine.verify_password("any-password", salt, bad_hash));
}
