// coding: utf-8
// =============================================================================
// StorageEngine.h
//
// Yuli Vault SQLite storage engine.
//
// Schema v3 (settings.schema_version = 3):
//   vault_items (id, type, title, payload, iv, tag, created_at, updated_at)
//   tags / entry_tags (FK → vault_items) / generated_passwords / settings
//
// A copied PwdVault v2 database keeps the `passwords` table until
// migrate_v2_to_v3() rewrites rows in a single transaction. That path never
// DROP-rebuilds user data. Payload blobs are opaque; ServiceCore encrypts.
// =============================================================================
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>

#include "IStorageEngine.h"

// 前置声明 sqlite3，避免在头文件中暴露 SQLite 头给外部包含方。
struct sqlite3;
struct sqlite3_stmt;

namespace yuli::vault::storage {

/// SQLite 持久化存储引擎。
class StorageEngine : public core::IStorageEngine {
public:
    /// 打开（必要时创建）SQLite 数据库并初始化 schema。
    /// \param db_path 数据库文件路径；传入 ":memory:" 可使用纯内存数据库（测试用）
    /// \throws 无异常——构造失败通过内部状态记录，后续方法返回 StorageError。
    explicit StorageEngine(const std::filesystem::path& db_path);

    /// 关闭 SQLite 连接（由 RAII 自动完成）。
    ~StorageEngine() override;

    // 禁用拷贝：sqlite3 连接不可拷贝。
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    core::Result<core::PasswordEntry> add_entry(const core::PasswordEntry& entry) override;
    core::Result<core::PasswordEntry> update_entry(const core::PasswordEntry& entry) override;
    core::Error remove_entry(int64_t id) override;
    core::Result<core::PasswordEntry> get_entry(int64_t id) override;
    core::Result<std::vector<core::PasswordEntry>> search_entries(
        const core::SearchQuery& query) override;
    core::Result<std::vector<core::PasswordEntry>> list_entries() override;
    int schema_version() override;
    core::Error migrate_v2_to_v3(
        const std::function<core::Result<core::VaultItem>(core::VaultItem)>& rewrap) override;

    core::Error begin_transaction() override;
    core::Error commit_transaction() override;
    core::Error rollback_transaction() override;

    core::Result<core::GeneratedPasswordRecord> add_generated_record(
        const core::GeneratedPasswordRecord& record) override;
    core::Result<core::GeneratedPasswordRecord> update_generated_record(
        const core::GeneratedPasswordRecord& record) override;
    core::Result<std::vector<core::GeneratedPasswordRecord>> list_generated_records() override;
    core::Error remove_generated_record(int64_t id) override;
    core::Error clear_generated_records() override;
    core::Result<std::string> get_setting(const std::string& key) override;
    core::Error set_setting(const std::string& key, const std::string& value) override;

    // Tag 与 entry-tag 关联
    core::Result<core::Tag> add_tag(const core::Tag& tag) override;
    core::Result<core::Tag> update_tag(const core::Tag& tag) override;
    core::Error remove_tag(int64_t id) override;
    core::Result<std::vector<core::Tag>> list_tags() override;
    core::Result<core::Tag> get_tag(int64_t id) override;
    core::Result<core::Tag> find_tag_by_name(const std::string& name) override;
    core::Result<std::vector<core::Tag>> get_entry_tags(int64_t entry_id) override;
    core::Error set_entry_tags(int64_t entry_id,
                                const std::vector<int64_t>& tag_ids) override;

private:
    /// 自定义 deleter：封装 sqlite3_close_v2。
    struct SqliteDbDeleter {
        void operator()(sqlite3* db) const;
    };
    using DbHandle = std::unique_ptr<sqlite3, SqliteDbDeleter>;

    /// 自定义 deleter：封装 sqlite3_finalize。
    struct SqliteStmtDeleter {
        void operator()(sqlite3_stmt* stmt) const;
    };
    using StmtHandle = std::unique_ptr<sqlite3_stmt, SqliteStmtDeleter>;

    DbHandle db_;
    std::mutex mutex_;
    int schema_version_ = 0;

    /// 执行一条无参数 SQL（如 BEGIN/COMMIT/CREATE TABLE）。
    core::Error exec_sql(const char* sql);

    /// 初始化 schema（建表 + 建索引 + 写入 schema_version）。
    core::Error init_schema();

    core::Error create_v3_item_tables();
    core::Error write_schema_version(int version);
    core::ByteVec payload_for_store(const core::VaultItem& entry) const;

    /// v3 vault_items row: id, type, title, payload, iv, tag, created_at, updated_at
    static core::PasswordEntry read_v3_row(sqlite3_stmt* stmt);
    /// v2 passwords row: id, entry_name, account, username, password, website, note, iv, tag, created, updated
    static core::PasswordEntry read_v2_row(sqlite3_stmt* stmt);

    /// 从当前 step 后的结果行读取一条 PasswordEntry（不含 tags）。
    core::PasswordEntry read_row(sqlite3_stmt* stmt) const;

    /// 从当前 step 后的结果行读取一条 Tag。
    static core::Tag read_tag_row(sqlite3_stmt* stmt);

    /// 从当前 step 后的结果行读取一条 GeneratedPasswordRecord。
    static core::GeneratedPasswordRecord read_generated_row(sqlite3_stmt* stmt);

    /// 将一个 BLOB 列读出为 ByteVec。
    static core::ByteVec read_blob_column(sqlite3_stmt* stmt, int col);

    /// 当前 Unix 时间戳（秒）。
    static int64_t now_seconds();

    /// 读取指定 entry_id 的所有关联标签（不加锁，由调用方持锁）。
    std::vector<core::Tag> read_entry_tags_unlocked(int64_t entry_id);

    /// 全量替换 entry_id 的标签关联（不加锁，由调用方持锁）。
    core::Error set_entry_tags_unlocked(int64_t entry_id,
                                         const std::vector<int64_t>& tag_ids);

    /// 为单条 PasswordEntry 填充 tags 字段（不加锁）。
    void fill_entry_tags_unlocked(core::PasswordEntry& entry);
};

}  // namespace yuli::vault::storage
