// coding: utf-8
// =============================================================================
// test_storage_engine.cpp
//
// 存储引擎单元测试。覆盖：
//   - InMemoryStorageEngine：CRUD、搜索、事务、list（无文件系统依赖）
//   - StorageEngine（真实 SQLite，:memory:）：BLOB 字段往返、基础 CRUD、事务
//
// 测试框架：GoogleTest。
// =============================================================================
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "InMemoryStorageEngine.h"
#include "StorageEngine.h"
#include "Types.h"
#include "VaultPayload.h"

namespace {

/// 构造一个测试用 PasswordEntry（id=0 表示新条目）。
yuli::vault::core::PasswordEntry make_entry(const std::string& website,
                                         const std::string& username,
                                         const std::string& note = "") {
    yuli::vault::core::PasswordEntry e;
    e.title = website;  // 用 website 作为显示标题
    e.account = username;     // 用 username 作为登录账号
    e.website = website;
    e.username = username;
    e.password = "cipher-blob-bytes";  // 模拟已加密的密文（实际由 ICryptoEngine 产出）
    e.note = note;
    // 模拟 12 字节 IV 与 16 字节 GCM tag
    e.iv.assign(12, yuli::vault::core::ByteVec::value_type{0xAB});
    e.tag.assign(16, yuli::vault::core::ByteVec::value_type{0xCD});
    return e;
}

}  // namespace

// =============================================================================
// InMemoryStorageEngine 测试
// =============================================================================

TEST(InMemoryStorageEngineTest, AddEntryAssignsIncrementingId) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto e1 = make_entry("github.com", "alice");
    auto e2 = make_entry("gitlab.com", "bob");

    auto r1 = engine.add_entry(e1);
    ASSERT_TRUE(r1.ok()) << r1.error().what();
    auto r2 = engine.add_entry(e2);
    ASSERT_TRUE(r2.ok()) << r2.error().what();

    EXPECT_NE(r1.value().id, 0);
    EXPECT_NE(r2.value().id, 0);
    EXPECT_LT(r1.value().id, r2.value().id);
    EXPECT_GT(r1.value().created_at, 0);
    EXPECT_EQ(r1.value().created_at, r1.value().updated_at);
}

TEST(InMemoryStorageEngineTest, GetEntryReturnsInsertedAndNotFound) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto e = make_entry("github.com", "alice", "personal account");

    auto added = engine.add_entry(e);
    ASSERT_TRUE(added.ok());

    auto got = engine.get_entry(added.value().id);
    ASSERT_TRUE(got.ok()) << got.error().what();
    EXPECT_EQ(got.value().id, added.value().id);
    EXPECT_EQ(got.value().website, "github.com");
    EXPECT_EQ(got.value().username, "alice");
    EXPECT_EQ(got.value().note, "personal account");
    EXPECT_EQ(got.value().password, "cipher-blob-bytes");
    EXPECT_EQ(got.value().iv.size(), 12u);
    EXPECT_EQ(got.value().tag.size(), 16u);

    auto miss = engine.get_entry(99999);
    ASSERT_FALSE(miss.ok());
    EXPECT_EQ(miss.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, UpdateEntryModifiesFieldsAndBumpsTimestamp) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto added = engine.add_entry(make_entry("github.com", "alice"));
    ASSERT_TRUE(added.ok());
    const int64_t created_at = added.value().created_at;

    // 等待 1 秒以确保 updated_at > created_at（时间戳为秒分辨率）。
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto to_update = added.value();
    to_update.username = "alice_new";
    to_update.note = "updated note";
    auto updated = engine.update_entry(to_update);
    ASSERT_TRUE(updated.ok()) << updated.error().what();
    EXPECT_EQ(updated.value().username, "alice_new");
    EXPECT_EQ(updated.value().note, "updated note");
    EXPECT_EQ(updated.value().created_at, created_at);
    EXPECT_GT(updated.value().updated_at, created_at);

    auto got = engine.get_entry(added.value().id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().username, "alice_new");
    EXPECT_EQ(got.value().note, "updated note");
    EXPECT_EQ(got.value().created_at, created_at);
    EXPECT_GT(got.value().updated_at, created_at);
}

TEST(InMemoryStorageEngineTest, UpdateEntryNotFoundReturnsError) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto e = make_entry("github.com", "alice");
    e.id = 42;
    auto r = engine.update_entry(e);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, RemoveEntryAndSubsequentGetFails) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto added = engine.add_entry(make_entry("github.com", "alice"));
    ASSERT_TRUE(added.ok());

    auto rm = engine.remove_entry(added.value().id);
    EXPECT_TRUE(rm.ok()) << rm.what();

    auto miss = engine.get_entry(added.value().id);
    ASSERT_FALSE(miss.ok());
    EXPECT_EQ(miss.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, RemoveEntryNotFoundReturnsError) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto rm = engine.remove_entry(99999);
    ASSERT_FALSE(rm.ok());
    EXPECT_EQ(rm.code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, ListEntriesReturnsAll) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "u1")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("b.com", "u2")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("c.com", "u3")).ok());

    auto list = engine.list_entries();
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().size(), 3u);
}

TEST(InMemoryStorageEngineTest, SearchByWebsite) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("github.com", "alice")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("gitlab.com", "bob")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("example.com", "carol")).ok());

    yuli::vault::core::SearchQuery q;
    q.text = "git";
    q.fields = {"website"};
    q.case_sensitive = false;
    auto r = engine.search_entries(q);
    ASSERT_TRUE(r.ok()) << r.error().what();
    ASSERT_EQ(r.value().size(), 2u);
    for (const auto& e : r.value()) {
        EXPECT_NE(e.website.find("git"), std::string::npos);
    }
}

TEST(InMemoryStorageEngineTest, SearchByUsername) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "alice")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("b.com", "bob")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("c.com", "alice_smith")).ok());

    yuli::vault::core::SearchQuery q;
    q.text = "alice";
    q.fields = {"username"};
    q.case_sensitive = false;
    auto r = engine.search_entries(q);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 2u);
}

TEST(InMemoryStorageEngineTest, SearchByNote) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "u1", "work account")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("b.com", "u2", "personal")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("c.com", "u3", "WORK email")).ok());

    yuli::vault::core::SearchQuery q;
    q.text = "work";
    q.fields = {"note"};
    q.case_sensitive = false;
    auto r = engine.search_entries(q);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 2u);  // "work account" + "WORK email"
}

TEST(InMemoryStorageEngineTest, SearchCaseSensitive) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("github.com", "Alice")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("example.com", "alice")).ok());

    yuli::vault::core::SearchQuery q_cs;
    q_cs.text = "Alice";
    q_cs.fields = {"username"};
    q_cs.case_sensitive = true;
    auto r_cs = engine.search_entries(q_cs);
    ASSERT_TRUE(r_cs.ok());
    ASSERT_EQ(r_cs.value().size(), 1u);
    EXPECT_EQ(r_cs.value()[0].username, "Alice");

    yuli::vault::core::SearchQuery q_ci;
    q_ci.text = "Alice";
    q_ci.fields = {"username"};
    q_ci.case_sensitive = false;
    auto r_ci = engine.search_entries(q_ci);
    ASSERT_TRUE(r_ci.ok());
    EXPECT_EQ(r_ci.value().size(), 2u);
}

TEST(InMemoryStorageEngineTest, SearchEmptyFieldsSearchesAll) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.add_entry(make_entry("github.com", "alice")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("example.com", "bob", "secret note")).ok());

    yuli::vault::core::SearchQuery q;
    q.text = "github";  // 只匹配 website
    q.fields = {};       // 空表示搜索全部字段
    q.case_sensitive = false;
    auto r = engine.search_entries(q);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 1u);
}

TEST(InMemoryStorageEngineTest, TransactionRollbackDiscardsAdds) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.begin_transaction().ok());
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "u1")).ok());
    ASSERT_TRUE(engine.add_entry(make_entry("b.com", "u2")).ok());
    ASSERT_TRUE(engine.rollback_transaction().ok());

    auto list = engine.list_entries();
    ASSERT_TRUE(list.ok());
    EXPECT_TRUE(list.value().empty());
}

TEST(InMemoryStorageEngineTest, TransactionCommitPersistsAdds) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    ASSERT_TRUE(engine.begin_transaction().ok());
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "u1")).ok());
    ASSERT_TRUE(engine.commit_transaction().ok());

    auto list = engine.list_entries();
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().size(), 1u);
}

TEST(InMemoryStorageEngineTest, TransactionRollbackRestoresIdCounter) {
    // 回滚后再次 add_entry，新 id 不应与已回滚的条目 id 冲突，
    // 也不应跳过号段（确保 next_id_ 也被恢复）。
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto first = engine.add_entry(make_entry("a.com", "u1"));
    ASSERT_TRUE(first.ok());
    const int64_t first_id = first.value().id;

    ASSERT_TRUE(engine.begin_transaction().ok());
    auto txn_added = engine.add_entry(make_entry("b.com", "u2"));
    ASSERT_TRUE(txn_added.ok());
    ASSERT_TRUE(engine.rollback_transaction().ok());

    auto after = engine.add_entry(make_entry("c.com", "u3"));
    ASSERT_TRUE(after.ok());
    EXPECT_GT(after.value().id, first_id);

    auto list = engine.list_entries();
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().size(), 2u);
}

// =============================================================================
// StorageEngine（真实 SQLite，:memory:）测试
// =============================================================================

TEST(StorageEngineSqliteTest, AddAndGetRoundtripsBlobFields) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    auto e = make_entry("github.com", "alice", "personal");
    // 故意填充含 0 字节的 BLOB，验证二进制安全性。
    e.password = std::string("enc\0ry\0pted", 11);
    e.iv = yuli::vault::core::ByteVec(12, yuli::vault::core::ByteVec::value_type{0x01});
    e.tag = yuli::vault::core::ByteVec(16, yuli::vault::core::ByteVec::value_type{0x02});

    auto added = engine.add_entry(e);
    ASSERT_TRUE(added.ok()) << added.error().what();
    EXPECT_NE(added.value().id, 0);

    auto got = engine.get_entry(added.value().id);
    ASSERT_TRUE(got.ok()) << got.error().what();
    EXPECT_EQ(got.value().website, "github.com");
    EXPECT_EQ(got.value().username, "alice");
    EXPECT_EQ(got.value().note, "personal");
    // BLOB 字段必须按字节完整往返（含嵌入的 \0）。
    EXPECT_EQ(got.value().password, e.password);
    EXPECT_EQ(got.value().iv, e.iv);
    EXPECT_EQ(got.value().tag, e.tag);
    EXPECT_EQ(got.value().iv.size(), 12u);
    EXPECT_EQ(got.value().tag.size(), 16u);
    EXPECT_GT(got.value().created_at, 0);
    EXPECT_EQ(got.value().created_at, got.value().updated_at);
}

TEST(StorageEngineSqliteTest, UpdateAndRemoveOnSqlite) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    auto added = engine.add_entry(make_entry("github.com", "alice"));
    ASSERT_TRUE(added.ok());
    const int64_t id = added.value().id;
    const int64_t created_at = added.value().created_at;

    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto to_update = added.value();
    to_update.note = "after update";
    auto updated = engine.update_entry(to_update);
    ASSERT_TRUE(updated.ok()) << updated.error().what();
    EXPECT_EQ(updated.value().created_at, created_at);
    EXPECT_GT(updated.value().updated_at, created_at);

    auto got = engine.get_entry(id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().note, "after update");

    ASSERT_TRUE(engine.remove_entry(id).ok());
    auto miss = engine.get_entry(id);
    ASSERT_FALSE(miss.ok());
    EXPECT_EQ(miss.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(StorageEngineSqliteTest, SqliteTransactionRollbackAndCommit) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    ASSERT_TRUE(engine.begin_transaction().ok());
    ASSERT_TRUE(engine.add_entry(make_entry("a.com", "u1")).ok());
    ASSERT_TRUE(engine.rollback_transaction().ok());

    auto list = engine.list_entries();
    ASSERT_TRUE(list.ok());
    EXPECT_TRUE(list.value().empty());

    ASSERT_TRUE(engine.begin_transaction().ok());
    ASSERT_TRUE(engine.add_entry(make_entry("b.com", "u2")).ok());
    ASSERT_TRUE(engine.commit_transaction().ok());

    auto list2 = engine.list_entries();
    ASSERT_TRUE(list2.ok());
    EXPECT_EQ(list2.value().size(), 1u);
}

// ===========================================================================
// InMemoryStorageEngine：生成器历史记录 CRUD
// ===========================================================================

namespace {

yuli::vault::core::GeneratedPasswordRecord make_gen_record(const std::string& password,
                                                          int32_t length) {
    yuli::vault::core::GeneratedPasswordRecord r;
    r.password = password;
    r.length = length;
    // iv / tag 留空（明文模式）；created_at 由引擎分配
    return r;
}

}  // namespace

TEST(InMemoryStorageEngineTest, AddGeneratedRecordAssignsIdAndTimestamp) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto r = make_gen_record("Pwd-Alpha", 10);
    auto added = engine.add_generated_record(r);
    ASSERT_TRUE(added.ok()) << added.error().what();
    EXPECT_GT(added.value().id, 0);
    EXPECT_GT(added.value().created_at, 0);
    EXPECT_EQ(added.value().password, "Pwd-Alpha");
    EXPECT_EQ(added.value().length, 10);
}

TEST(InMemoryStorageEngineTest, ListGeneratedRecordsSortedByCreatedAtDesc) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto a1 = engine.add_generated_record(make_gen_record("first", 5));
    ASSERT_TRUE(a1.ok());
    // 睡 1s 让 created_at 不同，避免出现相同时间戳
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto a2 = engine.add_generated_record(make_gen_record("second", 6));
    ASSERT_TRUE(a2.ok());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto a3 = engine.add_generated_record(make_gen_record("third", 5));
    ASSERT_TRUE(a3.ok());

    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok()) << list.error().what();
    ASSERT_EQ(list.value().size(), 3u);
    // 最新在前：third → second → first
    EXPECT_EQ(list.value()[0].password, "third");
    EXPECT_EQ(list.value()[1].password, "second");
    EXPECT_EQ(list.value()[2].password, "first");
}

TEST(InMemoryStorageEngineTest, RemoveGeneratedRecordById) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto added = engine.add_generated_record(make_gen_record("to-delete", 11));
    ASSERT_TRUE(added.ok());
    const int64_t id = added.value().id;

    EXPECT_TRUE(engine.remove_generated_record(id).ok());

    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok());
    EXPECT_TRUE(list.value().empty());
}

TEST(InMemoryStorageEngineTest, UpdateGeneratedRecordPreservesCreatedAt) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto added = engine.add_generated_record(make_gen_record("plain-pwd", 10));
    ASSERT_TRUE(added.ok()) << added.error().what();
    const int64_t id = added.value().id;
    const int64_t original_ts = added.value().created_at;
    ASSERT_GT(original_ts, 0);

    // 模拟 enable_program_password 重加密：仅改 password/iv/tag，保留 id 与 created_at
    yuli::vault::core::GeneratedPasswordRecord updated = added.value();
    updated.password = "cipher-blob";
    updated.length = 10;
    updated.iv = yuli::vault::core::ByteVec(12, yuli::vault::core::ByteVec::value_type{0xAA});
    updated.tag = yuli::vault::core::ByteVec(16, yuli::vault::core::ByteVec::value_type{0xBB});

    auto upd = engine.update_generated_record(updated);
    ASSERT_TRUE(upd.ok()) << upd.error().what();
    EXPECT_EQ(upd.value().id, id);
    EXPECT_EQ(upd.value().created_at, original_ts);  // 关键：时间戳未变
    EXPECT_EQ(upd.value().password, "cipher-blob");
    EXPECT_EQ(upd.value().length, 10);
    EXPECT_EQ(upd.value().iv.size(), 12u);
    EXPECT_EQ(upd.value().tag.size(), 16u);

    // 通过 list 再次确认持久化生效
    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].id, id);
    EXPECT_EQ(list.value()[0].created_at, original_ts);
    EXPECT_EQ(list.value()[0].password, "cipher-blob");
}

TEST(InMemoryStorageEngineTest, UpdateGeneratedRecordNotFound) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    yuli::vault::core::GeneratedPasswordRecord r;
    r.id = 9999;  // 不存在的 id
    r.password = "x";
    r.length = 1;
    auto upd = engine.update_generated_record(r);
    ASSERT_FALSE(upd.ok());
    EXPECT_EQ(upd.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, UpdateGeneratedRecordRejectsZeroId) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto added = engine.add_generated_record(make_gen_record("ok", 2));
    ASSERT_TRUE(added.ok());

    auto r = make_gen_record("zero-id", 7);
    r.id = 0;  // 显式置 0
    auto upd = engine.update_generated_record(r);
    ASSERT_FALSE(upd.ok());
    EXPECT_EQ(upd.error().code, yuli::vault::core::ErrorCode::InvalidArgument);
}

TEST(InMemoryStorageEngineTest, RemoveGeneratedRecordNotFound) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto err = engine.remove_generated_record(9999);
    ASSERT_FALSE(err.ok());
    EXPECT_EQ(err.code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(InMemoryStorageEngineTest, ClearGeneratedRecordsRemovesAll) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    engine.add_generated_record(make_gen_record("a", 1));
    engine.add_generated_record(make_gen_record("b", 2));
    engine.add_generated_record(make_gen_record("c", 3));
    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().size(), 3u);

    EXPECT_TRUE(engine.clear_generated_records().ok());

    auto list2 = engine.list_generated_records();
    ASSERT_TRUE(list2.ok());
    EXPECT_TRUE(list2.value().empty());
}

// ===========================================================================
// InMemoryStorageEngine：settings KV
// ===========================================================================

TEST(InMemoryStorageEngineTest, SetAndGetSettingRoundTrip) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    EXPECT_TRUE(engine.set_setting("generator.history_limit", "20").ok());
    auto v = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(*v, "20");
}

TEST(InMemoryStorageEngineTest, GetSettingMissingKeyReturnsEmpty) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    auto v = engine.get_setting("nonexistent.key");
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE(v->empty());
}

TEST(InMemoryStorageEngineTest, SetSettingOverwritesExistingValue) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    engine.set_setting("generator.history_limit", "10");
    engine.set_setting("generator.history_limit", "50");
    auto v = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(*v, "50");
}

TEST(InMemoryStorageEngineTest, SetSettingEmptyValueDeletesKey) {
    // 空值在 InMemory / SQLite 实现中均视为删除
    yuli::vault::storage::InMemoryStorageEngine engine;
    engine.set_setting("generator.history_limit", "20");
    engine.set_setting("generator.history_limit", "");
    auto v = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE(v->empty());
}

// ===========================================================================
// StorageEngine SQLite：生成器记录持久化
// ===========================================================================

TEST(StorageEngineSqliteTest, GeneratedRecordRoundTripSqlite) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    yuli::vault::core::GeneratedPasswordRecord r;
    r.password = std::string("enc\0data", 8);  // 含 0 字节，验证 BLOB 安全
    r.length = 8;
    r.iv = yuli::vault::core::ByteVec(12, yuli::vault::core::ByteVec::value_type{0xAA});
    r.tag = yuli::vault::core::ByteVec(16, yuli::vault::core::ByteVec::value_type{0xBB});

    auto added = engine.add_generated_record(r);
    ASSERT_TRUE(added.ok()) << added.error().what();
    EXPECT_GT(added.value().id, 0);

    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok()) << list.error().what();
    ASSERT_EQ(list.value().size(), 1u);
    // 注意：list 返回顺序为最新在前
    const auto& got = list.value()[0];
    EXPECT_EQ(got.password, r.password);
    EXPECT_EQ(got.iv, r.iv);
    EXPECT_EQ(got.tag, r.tag);
    EXPECT_EQ(got.length, r.length);
    EXPECT_GT(got.created_at, 0);
}

TEST(StorageEngineSqliteTest, GeneratedRecordRemoveAndClearSqlite) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    auto a1 = engine.add_generated_record(make_gen_record("p1", 2));
    auto a2 = engine.add_generated_record(make_gen_record("p2", 2));
    ASSERT_TRUE(a1.ok() && a2.ok());

    // 单条删除
    EXPECT_TRUE(engine.remove_generated_record(a1.value().id).ok());
    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].password, "p2");

    // 清空
    EXPECT_TRUE(engine.clear_generated_records().ok());
    auto list2 = engine.list_generated_records();
    ASSERT_TRUE(list2.ok());
    EXPECT_TRUE(list2.value().empty());
}

TEST(StorageEngineSqliteTest, GeneratedRecordUpdatePreservesCreatedAtSqlite) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    yuli::vault::core::GeneratedPasswordRecord r;
    r.password = "plain-pwd";
    r.length = 9;
    // 明文模式 iv/tag 为空
    auto added = engine.add_generated_record(r);
    ASSERT_TRUE(added.ok()) << added.error().what();
    const int64_t id = added.value().id;
    const int64_t original_ts = added.value().created_at;
    ASSERT_GT(original_ts, 0);

    // 模拟 enable_program_password 重加密：password/iv/tag 全变，length 不变
    yuli::vault::core::GeneratedPasswordRecord updated = added.value();
    updated.password = std::string("enc\0blob", 8);  // 含 0 字节验证 BLOB 安全
    updated.iv = yuli::vault::core::ByteVec(12, yuli::vault::core::ByteVec::value_type{0xAA});
    updated.tag = yuli::vault::core::ByteVec(16, yuli::vault::core::ByteVec::value_type{0xBB});

    auto upd = engine.update_generated_record(updated);
    ASSERT_TRUE(upd.ok()) << upd.error().what();
    EXPECT_EQ(upd.value().id, id);
    EXPECT_EQ(upd.value().created_at, original_ts);  // 关键：时间戳未变
    EXPECT_EQ(upd.value().password, updated.password);
    EXPECT_EQ(upd.value().iv, updated.iv);
    EXPECT_EQ(upd.value().tag, updated.tag);

    // 重新查库验证
    auto list = engine.list_generated_records();
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].id, id);
    EXPECT_EQ(list.value()[0].created_at, original_ts);
    EXPECT_EQ(list.value()[0].password, updated.password);
    EXPECT_EQ(list.value()[0].iv, updated.iv);
    EXPECT_EQ(list.value()[0].tag, updated.tag);
}

TEST(StorageEngineSqliteTest, GeneratedRecordUpdateNotFoundSqlite) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});
    yuli::vault::core::GeneratedPasswordRecord r;
    r.id = 4242;  // 不存在
    r.password = "x";
    r.length = 1;
    auto upd = engine.update_generated_record(r);
    ASSERT_FALSE(upd.ok());
    EXPECT_EQ(upd.error().code, yuli::vault::core::ErrorCode::NotFound);
}

TEST(StorageEngineSqliteTest, SettingsKvSqliteRoundTrip) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});

    EXPECT_TRUE(engine.set_setting("generator.history_limit", "100").ok());
    auto v = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(*v, "100");

    // 覆盖
    engine.set_setting("generator.history_limit", "20");
    auto v2 = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v2.ok());
    EXPECT_EQ(*v2, "20");

    // 空值视为删除
    engine.set_setting("generator.history_limit", "");
    auto v3 = engine.get_setting("generator.history_limit");
    ASSERT_TRUE(v3.ok());
    EXPECT_TRUE(v3->empty());
}

TEST(StorageEngineSqliteTest, SchemaVersionIs3OnFreshDatabase) {
    yuli::vault::storage::StorageEngine engine(std::filesystem::path{":memory:"});
    EXPECT_EQ(engine.schema_version(), 3);
}

TEST(InMemoryStorageEngineTest, SchemaVersionIs3AndMigrateIsNoop) {
    yuli::vault::storage::InMemoryStorageEngine engine;
    EXPECT_EQ(engine.schema_version(), 3);
    auto err = engine.migrate_v2_to_v3([](yuli::vault::core::VaultItem row) {
        return yuli::vault::core::Result<yuli::vault::core::VaultItem>::Ok(std::move(row));
    });
    EXPECT_TRUE(err.ok()) << err.what();
}

TEST(VaultPayloadTest, EncodeDecodeRoundTrip) {
    yuli::vault::core::VaultItem item;
    item.type = yuli::vault::core::VaultItemType::Login;
    item.title = "GitHub";
    item.account = "alice";
    item.username = "Alice";
    item.password = "s3cret";
    item.website = "github.com";
    item.note = "work";
    const auto blob = yuli::vault::core::encode_item_payload(item);
    yuli::vault::core::VaultItem out;
    out.title = "GitHub";
    ASSERT_TRUE(yuli::vault::core::decode_item_payload(out, blob).ok());
    EXPECT_EQ(out.type, item.type);
    EXPECT_EQ(out.account, item.account);
    EXPECT_EQ(out.username, item.username);
    EXPECT_EQ(out.password, item.password);
    EXPECT_EQ(out.website, item.website);
    EXPECT_EQ(out.note, item.note);
    EXPECT_EQ(out.title, "GitHub");
}

