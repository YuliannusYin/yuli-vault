// coding: utf-8
// =============================================================================
// test_migration.cpp
//
// Directory copy (PwdVault -> YuliVault), schema v2 -> v3, and encrypted
// v2 rewrap after unlock.
// =============================================================================
#include "AppDataDir.h"
#include "CryptoEngine.h"
#include "PasswordGenerator.h"
#include "ProgramPasswordStore.h"
#include "ServiceCore.h"
#include "StorageEngine.h"
#include "VaultPayload.h"

#include "Commands.h"
#include "Messages.h"
#include "Serializer.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

std::filesystem::path unique_temp_dir() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1);
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() /
               ("yuli_vault_mig_" + std::to_string(n) + "_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string path_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

void write_v2_database(const std::filesystem::path& db_path,
                       const std::string& password_blob,
                       const yuli::vault::core::ByteVec& iv,
                       const yuli::vault::core::ByteVec& tag) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path_utf8(db_path).c_str(), &db), SQLITE_OK);
    const char* ddl = R"SQL(
        CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        INSERT INTO settings(key, value) VALUES ('schema_version', '2');
        CREATE TABLE passwords (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            entry_name TEXT NOT NULL,
            account TEXT NOT NULL,
            username TEXT,
            password BLOB NOT NULL,
            website TEXT,
            note TEXT,
            iv BLOB NOT NULL,
            tag BLOB NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE tags (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            color TEXT,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE entry_tags (
            entry_id INTEGER NOT NULL,
            tag_id INTEGER NOT NULL,
            PRIMARY KEY (entry_id, tag_id)
        );
        CREATE TABLE generated_passwords (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            password BLOB NOT NULL,
            length INTEGER NOT NULL,
            iv BLOB NOT NULL,
            tag BLOB NOT NULL,
            created_at INTEGER NOT NULL
        );
    )SQL";
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, ddl, nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_stmt* ins = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO passwords (entry_name, account, username, password, website, note, "
        "iv, tag, created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?);",
        -1, &ins, nullptr), SQLITE_OK);
    sqlite3_bind_text(ins, 1, "GitHub", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, "alice", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, "Alice", -1, SQLITE_STATIC);
    sqlite3_bind_blob(ins, 4, password_blob.data(),
                      static_cast<int>(password_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 5, "github.com", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 6, "personal note", -1, SQLITE_STATIC);
    if (iv.empty()) {
        sqlite3_bind_zeroblob(ins, 7, 0);
    } else {
        sqlite3_bind_blob(ins, 7, iv.data(), static_cast<int>(iv.size()), SQLITE_TRANSIENT);
    }
    if (tag.empty()) {
        sqlite3_bind_zeroblob(ins, 8, 0);
    } else {
        sqlite3_bind_blob(ins, 8, tag.data(), static_cast<int>(tag.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(ins, 9, 1700000000);
    sqlite3_bind_int64(ins, 10, 1700000000);
    ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
    sqlite3_finalize(ins);
    sqlite3_close(db);
}

yuli::vault::core::ByteVec send_empty(
    yuli::vault::service::ServiceCore& core,
    yuli::vault::protocol::CommandId cmd) {
    yuli::vault::protocol::MessageHeader header;
    header.command = cmd;
    header.payload_size = 0;
    return core.handle_request(yuli::vault::core::ByteSpan{}, header);
}

template <typename Req>
yuli::vault::core::ByteVec send_req(
    yuli::vault::service::ServiceCore& core,
    yuli::vault::protocol::CommandId cmd,
    const Req& req) {
    auto payload = yuli::vault::protocol::serialize(req);
    yuli::vault::protocol::MessageHeader header;
    header.command = cmd;
    header.payload_size = static_cast<uint32_t>(payload.size());
    return core.handle_request(
        yuli::vault::core::ByteSpan(payload.data(), payload.size()), header);
}

}  // namespace

TEST(AppDataDirTest, CopiesLegacyPwdVaultAndKeepsOriginal) {
    const auto root = unique_temp_dir();
    const auto legacy = root / "PwdVault";
    std::filesystem::create_directories(legacy);
    {
        std::ofstream f(legacy / "marker.txt");
        f << "legacy-data";
    }

    std::string msg;
    const auto neu = yuli::vault::service::resolve_vault_data_dir(root, &msg);
    EXPECT_EQ(neu, root / "YuliVault");
    EXPECT_FALSE(msg.empty());
    EXPECT_TRUE(std::filesystem::exists(neu / "marker.txt"));
    EXPECT_TRUE(std::filesystem::exists(legacy / "marker.txt"));
    EXPECT_TRUE(std::filesystem::exists(neu / "migrated_from_pwdvault"));

    std::string ignored;
    const auto again = yuli::vault::service::resolve_vault_data_dir(root, &ignored);
    EXPECT_EQ(again, neu);
    EXPECT_TRUE(ignored.empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(SchemaV2MigrationTest, PlaintextV2BecomesVaultItems) {
    const auto dir = unique_temp_dir();
    const auto db_path = dir / "vault.db";
    write_v2_database(db_path, "plain-password", {}, {});

    yuli::vault::storage::StorageEngine engine(db_path);
    ASSERT_EQ(engine.schema_version(), 2);

    auto err = engine.migrate_v2_to_v3([](yuli::vault::core::VaultItem row) {
        row.payload = yuli::vault::core::encode_item_payload(row);
        row.iv.clear();
        row.tag.clear();
        return yuli::vault::core::Result<yuli::vault::core::VaultItem>::Ok(std::move(row));
    });
    ASSERT_TRUE(err.ok()) << err.what();
    EXPECT_EQ(engine.schema_version(), 3);

    auto listed = engine.list_entries();
    ASSERT_TRUE(listed.ok()) << listed.error().what();
    ASSERT_EQ(listed.value().size(), 1u);
    EXPECT_EQ(listed.value()[0].title, "GitHub");
    EXPECT_EQ(listed.value()[0].account, "alice");
    EXPECT_EQ(listed.value()[0].password, "plain-password");
    EXPECT_EQ(listed.value()[0].note, "personal note");
    EXPECT_EQ(listed.value()[0].type, yuli::vault::core::VaultItemType::Login);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(SchemaV2MigrationTest, EncryptedV2RewrapsOnUnlock) {
    using namespace yuli::vault;
    const auto dir = unique_temp_dir();
    const auto db_path = dir / "vault.db";
    const auto meta_path = dir / "vault.meta";
    constexpr const char* kPassword = "ProgramPass123!";

    crypto::CryptoEngine bootstrap(core::ByteSpan{});
    service::ProgramPasswordStore store(meta_path);
    auto key = store.initialize(kPassword, bootstrap);
    ASSERT_TRUE(key.ok()) << key.error().what();

    crypto::CryptoEngine entry_crypto(*key);
    const std::string secret = "legacy-secret";
    core::ByteSpan secret_span(
        reinterpret_cast<const std::byte*>(secret.data()), secret.size());
    auto enc = entry_crypto.encrypt(secret_span);
    ASSERT_TRUE(enc.ok()) << enc.error().what();
    const auto& blob = *enc;
    ASSERT_GE(blob.size(), 12u + 16u);
    core::ByteVec iv(blob.begin(), blob.begin() + 12);
    core::ByteVec tag(blob.end() - 16, blob.end());
    std::string ciphertext(
        reinterpret_cast<const char*>(blob.data() + 12),
        blob.size() - 12 - 16);

    write_v2_database(db_path, ciphertext, iv, tag);

    auto storage = std::make_unique<storage::StorageEngine>(db_path);
    EXPECT_EQ(storage->schema_version(), 2);

    auto core_ptr = std::make_unique<service::ServiceCore>(
        std::make_unique<crypto::CryptoEngine>(core::ByteSpan{}),
        std::move(storage),
        std::make_unique<generator::PasswordGenerator>(),
        meta_path);

    protocol::UnlockRequest unlock_req;
    unlock_req.password = kPassword;
    auto unlock_bytes = send_req(*core_ptr, protocol::CommandId::Unlock, unlock_req);
    auto unlock = protocol::deserialize<protocol::UnlockResponse>(
        core::ByteSpan(unlock_bytes.data(), unlock_bytes.size()));
    ASSERT_TRUE(unlock.ok()) << unlock.error().what();
    ASSERT_TRUE(unlock.value().success) << unlock.value().error_message;

    auto list_bytes = send_empty(*core_ptr, protocol::CommandId::ListEntries);
    auto listed = protocol::deserialize<protocol::ListEntriesResponse>(
        core::ByteSpan(list_bytes.data(), list_bytes.size()));
    ASSERT_TRUE(listed.ok()) << listed.error().what();
    ASSERT_EQ(listed.value().entries.size(), 1u);
    EXPECT_EQ(listed.value().entries[0].title, "GitHub");
    EXPECT_EQ(listed.value().entries[0].account, "alice");
    EXPECT_EQ(listed.value().entries[0].note, "personal note");
    EXPECT_EQ(listed.value().entries[0].password, secret);

    core_ptr.reset();
    storage::StorageEngine verify(db_path);
    EXPECT_EQ(verify.schema_version(), 3);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
