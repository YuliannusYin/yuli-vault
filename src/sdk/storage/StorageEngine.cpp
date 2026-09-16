// coding: utf-8
// =============================================================================
// StorageEngine.cpp
//
// StorageEngine 的实现细节。所有公开方法均通过 mutex 串行化，确保线程安全。
// SQLite 资源由 RAII 句柄管理；prepared statement 在每次调用时临时构造、
// 作用域结束时自动 finalize。
// =============================================================================
#include "StorageEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "VaultPayload.h"

namespace yuli::vault::storage {

namespace {

/// 构造一个 StorageError，附带 SQLite 错误消息。
core::Error make_storage_error(const std::string& context, sqlite3* db) {
    std::string msg = context;
    if (db != nullptr) {
        msg += ": ";
        msg += sqlite3_errmsg(db);
    }
    return core::Error(core::ErrorCode::StorageError, std::move(msg));
}

/// 将 SQLite 的 rc 转换为对应的 Error。SQLITE_OK 与 SQLITE_DONE 视为成功。
core::Error rc_to_error(int rc, const std::string& context, sqlite3* db) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return core::Error{};
    }
    if (rc == SQLITE_CONSTRAINT) {
        return core::Error(core::ErrorCode::AlreadyExists,
                           context + ": constraint violation");
    }
    return make_storage_error(context, db);
}

/// 转义 LIKE 通配符（%、_、\），返回 %pattern% 形式的子串匹配串。
std::string escape_like_pattern(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.insert(out.begin(), '%');
    out.push_back('%');
    return out;
}

/// 转义 GLOB 通配符（*、?、[），返回 *pattern* 形式的子串匹配串。
/// GLOB 在 SQLite 中始终区分大小写，用于 case_sensitive=true 分支。
std::string escape_glob_pattern(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (char c : text) {
        if (c == '*' || c == '?' || c == '[') {
            out.push_back('[');
            out.push_back(c);
            out.push_back(']');
        } else {
            out.push_back(c);
        }
    }
    out.insert(out.begin(), '*');
    out.push_back('*');
    return out;
}

/// 绑定 BLOB 参数；空指针退化为 zeroblob(0)，避免被当作 SQL NULL。
void bind_blob_safe(sqlite3_stmt* stmt, int idx, const void* data, int bytes) {
    if (data == nullptr) {
        sqlite3_bind_zeroblob(stmt, idx, 0);
    } else {
        sqlite3_bind_blob(stmt, idx, data, bytes, SQLITE_TRANSIENT);
    }
}

}  // namespace

// =============================================================================
// RAII deleter 实现
// =============================================================================

void StorageEngine::SqliteDbDeleter::operator()(sqlite3* db) const {
    if (db != nullptr) {
        sqlite3_close_v2(db);
    }
}

void StorageEngine::SqliteStmtDeleter::operator()(sqlite3_stmt* stmt) const {
    if (stmt != nullptr) {
        sqlite3_finalize(stmt);
    }
}

// =============================================================================
// 构造与析构
// =============================================================================

StorageEngine::StorageEngine(const std::filesystem::path& db_path) {
    sqlite3* raw = nullptr;
    // SQLITE_OPEN_NOMUTEX：使用多线程模式，由本类的 mutex 提供线程安全。
    // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE：默认读写并允许新建。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(db_path.string().c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        // 即便打开失败 raw 也可能被分配，需关闭以免泄漏。
        if (raw != nullptr) {
            sqlite3_close_v2(raw);
        }
        db_ = nullptr;
        return;
    }
    db_.reset(raw);

    // 设置忙等待超时（毫秒）：避免并发场景下立即返回 SQLITE_BUSY。
    sqlite3_busy_timeout(raw, 3000);

    // 初始化 schema；失败则关闭连接置空。
    core::Error err = init_schema();
    if (!err.ok()) {
        db_.reset();
    }
}

StorageEngine::~StorageEngine() = default;

// =============================================================================
// 内部辅助
// =============================================================================

core::Error StorageEngine::exec_sql(const char* sql) {
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError,
                           "database not opened");
    }
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string msg = err_msg ? err_msg : sqlite3_errmsg(db_.get());
        sqlite3_free(err_msg);
        return core::Error(core::ErrorCode::StorageError,
                           std::string("exec '") + sql + "' failed: " + msg);
    }
    return core::Error{};
}

core::Error StorageEngine::write_schema_version(int version) {
    const char* kUpsert = R"SQL(
        INSERT INTO settings (key, value) VALUES ('schema_version', ?)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kUpsert, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("write_schema_version: prepare", db_.get());
    }
    StmtHandle sp(stmt);
    const std::string v = std::to_string(version);
    sqlite3_bind_text(sp.get(), 1, v.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(sp.get());
    if (rc != SQLITE_DONE) {
        return rc_to_error(rc, "write_schema_version: step", db_.get());
    }
    schema_version_ = version;
    return core::Error{};
}

core::Error StorageEngine::create_v3_item_tables() {
    core::Error err = exec_sql(R"SQL(
        CREATE TABLE IF NOT EXISTS vault_items (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            type        INTEGER NOT NULL,
            title       TEXT NOT NULL,
            payload     BLOB NOT NULL,
            iv          BLOB NOT NULL,
            tag         BLOB NOT NULL,
            created_at  INTEGER NOT NULL,
            updated_at  INTEGER NOT NULL
        );
    )SQL");
    if (!err.ok()) return err;
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_vault_items_title ON vault_items(title);");
    if (!err.ok()) return err;
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_vault_items_type ON vault_items(type);");
    if (!err.ok()) return err;

    err = exec_sql(R"SQL(
        CREATE TABLE IF NOT EXISTS entry_tags (
            entry_id    INTEGER NOT NULL,
            tag_id      INTEGER NOT NULL,
            PRIMARY KEY (entry_id, tag_id),
            FOREIGN KEY (entry_id) REFERENCES vault_items(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)   REFERENCES tags(id) ON DELETE CASCADE
        );
    )SQL");
    if (!err.ok()) return err;
    return exec_sql("CREATE INDEX IF NOT EXISTS idx_entry_tags_tag ON entry_tags(tag_id);");
}

core::Error StorageEngine::init_schema() {
    core::Error err = exec_sql(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");");
    if (!err.ok()) return err;

    std::string current_version;
    {
        sqlite3_stmt* probe = nullptr;
        int rc = sqlite3_prepare_v2(db_.get(),
            "SELECT value FROM settings WHERE key='schema_version';",
            -1, &probe, nullptr);
        if (rc == SQLITE_OK && probe != nullptr) {
            StmtHandle sp(probe);
            if (sqlite3_step(sp.get()) == SQLITE_ROW) {
                const unsigned char* v = sqlite3_column_text(sp.get(), 0);
                if (v) current_version.assign(reinterpret_cast<const char*>(v));
            }
        }
    }

    int parsed = 0;
    if (!current_version.empty()) {
        try {
            parsed = std::stoi(current_version);
        } catch (...) {
            parsed = 0;
        }
    }

    const char* kCreateTagsTable = R"SQL(
        CREATE TABLE IF NOT EXISTS tags (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            name        TEXT NOT NULL UNIQUE,
            color       TEXT,
            created_at  INTEGER NOT NULL,
            updated_at  INTEGER NOT NULL
        );
    )SQL";
    err = exec_sql(kCreateTagsTable);
    if (!err.ok()) return err;
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_tags_name ON tags(name);");
    if (!err.ok()) return err;

    const char* kCreateGenTable = R"SQL(
        CREATE TABLE IF NOT EXISTS generated_passwords (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            password    BLOB NOT NULL,
            length      INTEGER NOT NULL,
            iv          BLOB NOT NULL,
            tag         BLOB NOT NULL,
            created_at  INTEGER NOT NULL
        );
    )SQL";
    err = exec_sql(kCreateGenTable);
    if (!err.ok()) return err;
    err = exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_genpw_created_at ON generated_passwords(created_at);");
    if (!err.ok()) return err;

    err = exec_sql("PRAGMA foreign_keys = ON;");
    if (!err.ok()) return err;

    if (parsed >= 3) {
        err = create_v3_item_tables();
        if (!err.ok()) return err;
        schema_version_ = 3;
        return core::Error{};
    }

    if (parsed == 2) {
        // Keep legacy passwords + entry_tags until ServiceCore migrates after unlock.
        schema_version_ = 2;
        return core::Error{};
    }

    // Fresh database (or pre-v2): create v3 objects. Do not DROP user data if a
    // passwords table already exists — treat that as v2 and wait for migrate.
    {
        sqlite3_stmt* probe = nullptr;
        int rc = sqlite3_prepare_v2(db_.get(),
            "SELECT name FROM sqlite_master WHERE type='table' AND name='passwords';",
            -1, &probe, nullptr);
        bool has_passwords = false;
        if (rc == SQLITE_OK && probe != nullptr) {
            StmtHandle sp(probe);
            has_passwords = sqlite3_step(sp.get()) == SQLITE_ROW;
        }
        if (has_passwords) {
            schema_version_ = 2;
            return write_schema_version(2);
        }
    }

    err = create_v3_item_tables();
    if (!err.ok()) return err;
    return write_schema_version(3);
}

core::ByteVec StorageEngine::read_blob_column(sqlite3_stmt* stmt, int col) {
    const void* ptr = sqlite3_column_blob(stmt, col);
    int bytes = sqlite3_column_bytes(stmt, col);
    if (ptr == nullptr || bytes <= 0) {
        return {};
    }
    const auto* byte_ptr = static_cast<const std::byte*>(ptr);
    return core::ByteVec(byte_ptr, byte_ptr + static_cast<size_t>(bytes));
}

core::PasswordEntry StorageEngine::read_v3_row(sqlite3_stmt* stmt) {
    // 0 id, 1 type, 2 title, 3 payload, 4 iv, 5 tag, 6 created_at, 7 updated_at
    core::PasswordEntry e;
    e.id = sqlite3_column_int64(stmt, 0);
    e.type = static_cast<core::VaultItemType>(
        static_cast<uint8_t>(sqlite3_column_int(stmt, 1)));
    if (const unsigned char* v = sqlite3_column_text(stmt, 2)) {
        e.title.assign(reinterpret_cast<const char*>(v));
    }
    e.payload = read_blob_column(stmt, 3);
    e.iv = read_blob_column(stmt, 4);
    e.tag = read_blob_column(stmt, 5);
    e.created_at = sqlite3_column_int64(stmt, 6);
    e.updated_at = sqlite3_column_int64(stmt, 7);
    (void)core::try_decode_item_payload(e, e.payload);
    return e;
}

core::PasswordEntry StorageEngine::read_v2_row(sqlite3_stmt* stmt) {
    // 0 id, 1 entry_name, 2 account, 3 username, 4 password,
    // 5 website, 6 note, 7 iv, 8 tag, 9 created_at, 10 updated_at
    core::PasswordEntry e;
    e.type = core::VaultItemType::Login;
    e.id = sqlite3_column_int64(stmt, 0);
    if (const unsigned char* v = sqlite3_column_text(stmt, 1)) {
        e.title.assign(reinterpret_cast<const char*>(v));
    }
    if (const unsigned char* v = sqlite3_column_text(stmt, 2)) {
        e.account.assign(reinterpret_cast<const char*>(v));
    }
    if (const unsigned char* v = sqlite3_column_text(stmt, 3)) {
        e.username.assign(reinterpret_cast<const char*>(v));
    }
    if (const void* v = sqlite3_column_blob(stmt, 4)) {
        int n = sqlite3_column_bytes(stmt, 4);
        e.password.assign(static_cast<const char*>(v), static_cast<size_t>(n));
    }
    if (const unsigned char* v = sqlite3_column_text(stmt, 5)) {
        e.website.assign(reinterpret_cast<const char*>(v));
    }
    if (const unsigned char* v = sqlite3_column_text(stmt, 6)) {
        e.note.assign(reinterpret_cast<const char*>(v));
    }
    e.iv = read_blob_column(stmt, 7);
    e.tag = read_blob_column(stmt, 8);
    e.created_at = sqlite3_column_int64(stmt, 9);
    e.updated_at = sqlite3_column_int64(stmt, 10);
    return e;
}

core::PasswordEntry StorageEngine::read_row(sqlite3_stmt* stmt) const {
    if (schema_version_ >= 3) {
        return read_v3_row(stmt);
    }
    return read_v2_row(stmt);
}

core::ByteVec StorageEngine::payload_for_store(const core::VaultItem& entry) const {
    if (!entry.payload.empty()) {
        return entry.payload;
    }
    return core::encode_item_payload(entry);
}

int StorageEngine::schema_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return schema_version_;
}

core::Tag StorageEngine::read_tag_row(sqlite3_stmt* stmt) {
    // 列顺序：0 id, 1 name, 2 color, 3 created_at, 4 updated_at
    core::Tag t;
    t.id = sqlite3_column_int64(stmt, 0);
    if (const unsigned char* v = sqlite3_column_text(stmt, 1)) {
        t.name.assign(reinterpret_cast<const char*>(v));
    }
    if (const unsigned char* v = sqlite3_column_text(stmt, 2)) {
        t.color.assign(reinterpret_cast<const char*>(v));
    }
    t.created_at = sqlite3_column_int64(stmt, 3);
    t.updated_at = sqlite3_column_int64(stmt, 4);
    return t;
}

int64_t StorageEngine::now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// =============================================================================
// CRUD
// =============================================================================

core::Result<core::PasswordEntry> StorageEngine::add_entry(
    const core::PasswordEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    if (schema_version_ < 3) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::StorageError,
                        "add_entry: schema v2 requires migrate_v2_to_v3"));
    }

    const char* kSql = R"SQL(
        INSERT INTO vault_items
            (type, title, payload, iv, tag, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )SQL";

    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::PasswordEntry>::Err(
            make_storage_error("add_entry: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    const core::ByteVec payload = payload_for_store(entry);
    sqlite3_bind_int(stmt.get(), 1, static_cast<int>(entry.type));
    sqlite3_bind_text(stmt.get(), 2, entry.title.c_str(), -1, SQLITE_TRANSIENT);
    bind_blob_safe(stmt.get(), 3, payload.data(), static_cast<int>(payload.size()));
    bind_blob_safe(stmt.get(), 4, entry.iv.data(), static_cast<int>(entry.iv.size()));
    bind_blob_safe(stmt.get(), 5, entry.tag.data(), static_cast<int>(entry.tag.size()));

    const int64_t ts = now_seconds();
    sqlite3_bind_int64(stmt.get(), 6, ts);
    sqlite3_bind_int64(stmt.get(), 7, ts);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return core::Result<core::PasswordEntry>::Err(
            rc_to_error(rc, "add_entry: step", db_.get()));
    }

    core::PasswordEntry out = entry;
    out.id = sqlite3_last_insert_rowid(db_.get());
    out.created_at = ts;
    out.updated_at = ts;

    // 同步标签关联：根据 entry.tags 中的 id 写入 entry_tags。
    // 对 id==0 的新标签，调用方（service）应先调用 add_tag 落库并替换为真实 id。
    std::vector<int64_t> tag_ids;
    tag_ids.reserve(entry.tags.size());
    for (const auto& t : entry.tags) {
        if (t.id != 0) tag_ids.push_back(t.id);
    }
    auto tag_err = set_entry_tags_unlocked(out.id, tag_ids);
    if (!tag_err.ok()) {
        return core::Result<core::PasswordEntry>::Err(tag_err);
    }
    out.tags = read_entry_tags_unlocked(out.id);
    return core::Result<core::PasswordEntry>::Ok(std::move(out));
}

core::Result<core::PasswordEntry> StorageEngine::update_entry(
    const core::PasswordEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    if (entry.id == 0) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::InvalidArgument,
                        "update_entry: id must not be 0"));
    }

    const char* kSql = R"SQL(
        UPDATE vault_items SET
            type        = ?,
            title       = ?,
            payload     = ?,
            iv          = ?,
            tag         = ?,
            updated_at  = ?
        WHERE id = ?;
    )SQL";

    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::PasswordEntry>::Err(
            make_storage_error("update_entry: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    const core::ByteVec payload = payload_for_store(entry);
    sqlite3_bind_int(stmt.get(), 1, static_cast<int>(entry.type));
    sqlite3_bind_text(stmt.get(), 2, entry.title.c_str(), -1, SQLITE_TRANSIENT);
    bind_blob_safe(stmt.get(), 3, payload.data(), static_cast<int>(payload.size()));
    bind_blob_safe(stmt.get(), 4, entry.iv.data(), static_cast<int>(entry.iv.size()));
    bind_blob_safe(stmt.get(), 5, entry.tag.data(), static_cast<int>(entry.tag.size()));
    const int64_t ts = now_seconds();
    sqlite3_bind_int64(stmt.get(), 6, ts);
    sqlite3_bind_int64(stmt.get(), 7, entry.id);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return core::Result<core::PasswordEntry>::Err(
            rc_to_error(rc, "update_entry: step", db_.get()));
    }

    if (sqlite3_changes(db_.get()) == 0) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::NotFound,
                        "update_entry: id not found"));
    }

    // 同步标签关联（全量替换）
    std::vector<int64_t> tag_ids;
    tag_ids.reserve(entry.tags.size());
    for (const auto& t : entry.tags) {
        if (t.id != 0) tag_ids.push_back(t.id);
    }
    auto tag_err = set_entry_tags_unlocked(entry.id, tag_ids);
    if (!tag_err.ok()) {
        return core::Result<core::PasswordEntry>::Err(tag_err);
    }

    core::PasswordEntry out = entry;
    out.updated_at = ts;
    out.tags = read_entry_tags_unlocked(entry.id);
    // created_at 不被 UPDATE 修改，DB 中保留原值；
    // 这里沿用调用方传入的值，调用方通常已通过 get_entry 拿到完整条目。
    return core::Result<core::PasswordEntry>::Ok(std::move(out));
}

core::Error StorageEngine::remove_entry(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }

    const char* kSql = "DELETE FROM vault_items WHERE id = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("remove_entry: prepare", db_.get());
    }
    StmtHandle stmt(raw_stmt);

    sqlite3_bind_int64(stmt.get(), 1, id);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return rc_to_error(rc, "remove_entry: step", db_.get());
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return core::Error(core::ErrorCode::NotFound,
                           "remove_entry: id not found");
    }
    return core::Error{};
}

core::Result<core::PasswordEntry> StorageEngine::get_entry(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }

    const char* kSql = R"SQL(
        SELECT id, type, title, payload, iv, tag, created_at, updated_at
        FROM vault_items WHERE id = ?;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::PasswordEntry>::Err(
            make_storage_error("get_entry: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    sqlite3_bind_int64(stmt.get(), 1, id);
    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        core::PasswordEntry e = read_row(stmt.get());
        fill_entry_tags_unlocked(e);
        return core::Result<core::PasswordEntry>::Ok(std::move(e));
    }
    if (rc == SQLITE_DONE) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::NotFound, "get_entry: id not found"));
    }
    return core::Result<core::PasswordEntry>::Err(
        rc_to_error(rc, "get_entry: step", db_.get()));
}

core::Result<std::vector<core::PasswordEntry>> StorageEngine::search_entries(
    const core::SearchQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::vector<core::PasswordEntry>>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }

    std::ostringstream sql;
    sql << "SELECT id, type, title, payload, iv, tag, created_at, updated_at "
           "FROM vault_items";
    const bool has_tag_clause = !query.tag_ids.empty();
    if (has_tag_clause) {
        sql << " WHERE id IN (SELECT entry_id FROM entry_tags WHERE tag_id IN (";
        for (size_t i = 0; i < query.tag_ids.size(); ++i) {
            if (i > 0) sql << ",";
            sql << "?";
        }
        sql << "))";
    }
    sql << " ORDER BY created_at DESC;";

    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), sql.str().c_str(), -1,
                                &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<std::vector<core::PasswordEntry>>::Err(
            make_storage_error("search_entries: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    int bind_idx = 1;
    if (has_tag_clause) {
        for (auto tid : query.tag_ids) {
            sqlite3_bind_int64(stmt.get(), bind_idx++, tid);
        }
    }

    std::vector<core::PasswordEntry> results;
    while (true) {
        rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            core::PasswordEntry e = read_row(stmt.get());
            fill_entry_tags_unlocked(e);
            if (core::item_matches_text_query(e, query)) {
                results.push_back(std::move(e));
            }
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return core::Result<std::vector<core::PasswordEntry>>::Err(
                rc_to_error(rc, "search_entries: step", db_.get()));
        }
    }
    return core::Result<std::vector<core::PasswordEntry>>::Ok(std::move(results));
}

core::Result<std::vector<core::PasswordEntry>> StorageEngine::list_entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::vector<core::PasswordEntry>>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }

    const char* kSql = R"SQL(
        SELECT id, type, title, payload, iv, tag, created_at, updated_at
        FROM vault_items ORDER BY created_at DESC;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<std::vector<core::PasswordEntry>>::Err(
            make_storage_error("list_entries: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    std::vector<core::PasswordEntry> results;
    while (true) {
        rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            core::PasswordEntry e = read_row(stmt.get());
            fill_entry_tags_unlocked(e);
            results.push_back(std::move(e));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return core::Result<std::vector<core::PasswordEntry>>::Err(
                rc_to_error(rc, "list_entries: step", db_.get()));
        }
    }
    return core::Result<std::vector<core::PasswordEntry>>::Ok(std::move(results));
}

// =============================================================================
// 事务
// =============================================================================

core::Error StorageEngine::begin_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_sql("BEGIN TRANSACTION;");
}

core::Error StorageEngine::commit_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_sql("COMMIT;");
}

core::Error StorageEngine::rollback_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_sql("ROLLBACK;");
}

// =============================================================================
// 生成器历史记录
// =============================================================================

core::GeneratedPasswordRecord StorageEngine::read_generated_row(sqlite3_stmt* stmt) {
    core::GeneratedPasswordRecord r;
    r.id = sqlite3_column_int64(stmt, 0);
    // password 列：以 BLOB 读取后转为 std::string（密文也是字节序列）
    const void* pw_ptr = sqlite3_column_blob(stmt, 1);
    int pw_bytes = sqlite3_column_bytes(stmt, 1);
    if (pw_ptr && pw_bytes > 0) {
        r.password.assign(static_cast<const char*>(pw_ptr),
                          static_cast<size_t>(pw_bytes));
    }
    r.length = sqlite3_column_int(stmt, 2);
    r.iv = read_blob_column(stmt, 3);
    r.tag = read_blob_column(stmt, 4);
    r.created_at = sqlite3_column_int64(stmt, 5);
    return r;
}

core::Result<core::GeneratedPasswordRecord> StorageEngine::add_generated_record(
    const core::GeneratedPasswordRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }

    const char* kSql = R"SQL(
        INSERT INTO generated_passwords (password, length, iv, tag, created_at)
        VALUES (?, ?, ?, ?, ?);
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            make_storage_error("add_generated_record: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    // password 绑定为 BLOB（无论是明文还是密文，按原始字节存储）
    bind_blob_safe(stmt.get(), 1,
                   record.password.data(),
                   static_cast<int>(record.password.size()));
    sqlite3_bind_int(stmt.get(), 2, record.length);
    bind_blob_safe(stmt.get(), 3,
                   record.iv.data(), static_cast<int>(record.iv.size()));
    bind_blob_safe(stmt.get(), 4,
                   record.tag.data(), static_cast<int>(record.tag.size()));
    const int64_t ts = now_seconds();
    sqlite3_bind_int64(stmt.get(), 5, ts);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            rc_to_error(rc, "add_generated_record: step", db_.get()));
    }

    core::GeneratedPasswordRecord out = record;
    out.id = sqlite3_last_insert_rowid(db_.get());
    out.created_at = ts;
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(out));
}

core::Result<core::GeneratedPasswordRecord> StorageEngine::update_generated_record(
    const core::GeneratedPasswordRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    if (record.id == 0) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::InvalidArgument,
                        "update_generated_record: id must not be 0"));
    }

    // 仅更新 password/length/iv/tag，保留 created_at 不变
    const char* kSql = R"SQL(
        UPDATE generated_passwords
        SET password = ?, length = ?, iv = ?, tag = ?
        WHERE id = ?;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            make_storage_error("update_generated_record: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    bind_blob_safe(stmt.get(), 1,
                   record.password.data(),
                   static_cast<int>(record.password.size()));
    sqlite3_bind_int(stmt.get(), 2, record.length);
    bind_blob_safe(stmt.get(), 3,
                   record.iv.data(), static_cast<int>(record.iv.size()));
    bind_blob_safe(stmt.get(), 4,
                   record.tag.data(), static_cast<int>(record.tag.size()));
    sqlite3_bind_int64(stmt.get(), 5, record.id);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            rc_to_error(rc, "update_generated_record: step", db_.get()));
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::NotFound,
                        "update_generated_record: id not found"));
    }

    // created_at 不变，直接返回调用方传入的 record
    return core::Result<core::GeneratedPasswordRecord>::Ok(record);
}

core::Result<std::vector<core::GeneratedPasswordRecord>>
StorageEngine::list_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::vector<core::GeneratedPasswordRecord>>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }

    const char* kSql = R"SQL(
        SELECT id, password, length, iv, tag, created_at
        FROM generated_passwords
        ORDER BY created_at DESC, id DESC;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<std::vector<core::GeneratedPasswordRecord>>::Err(
            make_storage_error("list_generated_records: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    std::vector<core::GeneratedPasswordRecord> results;
    while (true) {
        rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            results.push_back(read_generated_row(stmt.get()));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return core::Result<std::vector<core::GeneratedPasswordRecord>>::Err(
                rc_to_error(rc, "list_generated_records: step", db_.get()));
        }
    }
    return core::Result<std::vector<core::GeneratedPasswordRecord>>::Ok(std::move(results));
}

core::Error StorageEngine::remove_generated_record(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    const char* kSql = "DELETE FROM generated_passwords WHERE id = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("remove_generated_record: prepare", db_.get());
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_int64(stmt.get(), 1, id);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return rc_to_error(rc, "remove_generated_record: step", db_.get());
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return core::Error(core::ErrorCode::NotFound,
                           "remove_generated_record: id not found");
    }
    return core::Error{};
}

core::Error StorageEngine::clear_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    return exec_sql("DELETE FROM generated_passwords;");
}

// =============================================================================
// 通用 KV 设置
// =============================================================================

core::Result<std::string> StorageEngine::get_setting(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::string>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    const char* kSql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<std::string>::Err(
            make_storage_error("get_setting: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(stmt.get(), 0);
        std::string value = txt ? reinterpret_cast<const char*>(txt) : "";
        return core::Result<std::string>::Ok(std::move(value));
    }
    if (rc == SQLITE_DONE) {
        // key 不存在，返回空字符串（不视为错误）
        return core::Result<std::string>::Ok(std::string{});
    }
    return core::Result<std::string>::Err(
        rc_to_error(rc, "get_setting: step", db_.get()));
}

core::Error StorageEngine::set_setting(const std::string& key,
                                         const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    // value 为空 → 删除该 key（语义：空等价于不存在）
    if (value.empty()) {
        const char* kDel = "DELETE FROM settings WHERE key = ?;";
        sqlite3_stmt* raw_stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.get(), kDel, -1, &raw_stmt, nullptr);
        if (rc != SQLITE_OK) {
            return make_storage_error("set_setting: delete prepare", db_.get());
        }
        StmtHandle stmt(raw_stmt);
        sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return rc_to_error(rc, "set_setting: delete step", db_.get());
        }
        return core::Error{};
    }
    const char* kSql = R"SQL(
        INSERT INTO settings (key, value) VALUES (?, ?)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("set_setting: upsert prepare", db_.get());
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return rc_to_error(rc, "set_setting: upsert step", db_.get());
    }
    return core::Error{};
}

// =============================================================================
// 标签（Tag）与 entry-tag 关联
// =============================================================================
// 调用方需持锁的辅助函数实现
// =============================================================================

std::vector<core::Tag> StorageEngine::read_entry_tags_unlocked(int64_t entry_id) {
    std::vector<core::Tag> tags;
    if (db_ == nullptr) return tags;

    const char* kSql = R"SQL(
        SELECT t.id, t.name, t.color, t.created_at, t.updated_at
        FROM tags t
        INNER JOIN entry_tags et ON et.tag_id = t.id
        WHERE et.entry_id = ?
        ORDER BY t.name ASC;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) return tags;
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_int64(stmt.get(), 1, entry_id);

    while (true) {
        rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            tags.push_back(read_tag_row(stmt.get()));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            break;  // 出错时返回已收集到的标签
        }
    }
    return tags;
}

core::Error StorageEngine::set_entry_tags_unlocked(int64_t entry_id,
                                                     const std::vector<int64_t>& tag_ids) {
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    // 1. 删除现有所有关联
    {
        const char* kDel = "DELETE FROM entry_tags WHERE entry_id = ?;";
        sqlite3_stmt* raw_stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.get(), kDel, -1, &raw_stmt, nullptr);
        if (rc != SQLITE_OK) {
            return make_storage_error("set_entry_tags: delete prepare", db_.get());
        }
        StmtHandle stmt(raw_stmt);
        sqlite3_bind_int64(stmt.get(), 1, entry_id);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return rc_to_error(rc, "set_entry_tags: delete step", db_.get());
        }
    }
    // 2. 去重写入新关联（跳过 id==0 与不存在的 tag_id）
    if (tag_ids.empty()) return core::Error{};

    const char* kIns = "INSERT OR IGNORE INTO entry_tags (entry_id, tag_id) VALUES (?, ?);";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kIns, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("set_entry_tags: insert prepare", db_.get());
    }
    StmtHandle stmt(raw_stmt);

    // 去重（保留首次出现顺序）
    std::vector<int64_t> unique_ids;
    unique_ids.reserve(tag_ids.size());
    for (auto tid : tag_ids) {
        if (tid == 0) continue;
        if (std::find(unique_ids.begin(), unique_ids.end(), tid) == unique_ids.end()) {
            unique_ids.push_back(tid);
        }
    }

    for (auto tid : unique_ids) {
        sqlite3_bind_int64(stmt.get(), 1, entry_id);
        sqlite3_bind_int64(stmt.get(), 2, tid);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            // 关联表外键约束失败（tag_id 不存在）会被 SQLITE_CONSTRAINT 捕获，
            // 这里以静默跳过策略处理；其他错误返回。
            if (rc == SQLITE_CONSTRAINT) {
                sqlite3_reset(stmt.get());
                continue;
            }
            return rc_to_error(rc, "set_entry_tags: insert step", db_.get());
        }
        sqlite3_reset(stmt.get());
    }
    return core::Error{};
}

void StorageEngine::fill_entry_tags_unlocked(core::PasswordEntry& entry) {
    entry.tags = read_entry_tags_unlocked(entry.id);
}

// -----------------------------------------------------------------------------
// 公开 Tag CRUD
// -----------------------------------------------------------------------------

core::Result<core::Tag> StorageEngine::add_tag(const core::Tag& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    if (tag.name.empty()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "add_tag: name must not be empty"));
    }

    const char* kSql = R"SQL(
        INSERT INTO tags (name, color, created_at, updated_at)
        VALUES (?, ?, ?, ?);
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::Tag>::Err(
            make_storage_error("add_tag: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, tag.name.c_str(), -1, SQLITE_TRANSIENT);
    if (tag.color.empty()) {
        sqlite3_bind_null(stmt.get(), 2);
    } else {
        sqlite3_bind_text(stmt.get(), 2, tag.color.c_str(), -1, SQLITE_TRANSIENT);
    }
    const int64_t ts = now_seconds();
    sqlite3_bind_int64(stmt.get(), 3, ts);
    sqlite3_bind_int64(stmt.get(), 4, ts);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            return core::Result<core::Tag>::Err(
                core::Error(core::ErrorCode::AlreadyExists,
                            "add_tag: tag name already exists"));
        }
        return core::Result<core::Tag>::Err(
            rc_to_error(rc, "add_tag: step", db_.get()));
    }

    core::Tag out = tag;
    out.id = sqlite3_last_insert_rowid(db_.get());
    out.created_at = ts;
    out.updated_at = ts;
    return core::Result<core::Tag>::Ok(std::move(out));
}

core::Result<core::Tag> StorageEngine::update_tag(const core::Tag& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    if (tag.id == 0) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "update_tag: id must not be 0"));
    }
    if (tag.name.empty()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "update_tag: name must not be empty"));
    }

    const char* kSql = R"SQL(
        UPDATE tags SET name = ?, color = ?, updated_at = ? WHERE id = ?;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::Tag>::Err(
            make_storage_error("update_tag: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, tag.name.c_str(), -1, SQLITE_TRANSIENT);
    if (tag.color.empty()) {
        sqlite3_bind_null(stmt.get(), 2);
    } else {
        sqlite3_bind_text(stmt.get(), 2, tag.color.c_str(), -1, SQLITE_TRANSIENT);
    }
    const int64_t ts = now_seconds();
    sqlite3_bind_int64(stmt.get(), 3, ts);
    sqlite3_bind_int64(stmt.get(), 4, tag.id);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            return core::Result<core::Tag>::Err(
                core::Error(core::ErrorCode::AlreadyExists,
                            "update_tag: tag name already exists"));
        }
        return core::Result<core::Tag>::Err(
            rc_to_error(rc, "update_tag: step", db_.get()));
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "update_tag: id not found"));
    }

    core::Tag out = tag;
    out.updated_at = ts;
    return core::Result<core::Tag>::Ok(std::move(out));
}

core::Error StorageEngine::remove_tag(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    // entry_tags 关联由 ON DELETE CASCADE 自动清理
    const char* kSql = "DELETE FROM tags WHERE id = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_storage_error("remove_tag: prepare", db_.get());
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_int64(stmt.get(), 1, id);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        return rc_to_error(rc, "remove_tag: step", db_.get());
    }
    if (sqlite3_changes(db_.get()) == 0) {
        return core::Error(core::ErrorCode::NotFound, "remove_tag: id not found");
    }
    return core::Error{};
}

core::Result<std::vector<core::Tag>> StorageEngine::list_tags() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::vector<core::Tag>>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    const char* kSql = R"SQL(
        SELECT id, name, color, created_at, updated_at
        FROM tags ORDER BY name ASC;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<std::vector<core::Tag>>::Err(
            make_storage_error("list_tags: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);

    std::vector<core::Tag> results;
    while (true) {
        rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            results.push_back(read_tag_row(stmt.get()));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return core::Result<std::vector<core::Tag>>::Err(
                rc_to_error(rc, "list_tags: step", db_.get()));
        }
    }
    return core::Result<std::vector<core::Tag>>::Ok(std::move(results));
}

core::Result<core::Tag> StorageEngine::get_tag(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    const char* kSql = R"SQL(
        SELECT id, name, color, created_at, updated_at
        FROM tags WHERE id = ?;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::Tag>::Err(
            make_storage_error("get_tag: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_int64(stmt.get(), 1, id);
    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return core::Result<core::Tag>::Ok(read_tag_row(stmt.get()));
    }
    if (rc == SQLITE_DONE) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "get_tag: id not found"));
    }
    return core::Result<core::Tag>::Err(
        rc_to_error(rc, "get_tag: step", db_.get()));
}

core::Result<core::Tag> StorageEngine::find_tag_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    const char* kSql = R"SQL(
        SELECT id, name, color, created_at, updated_at
        FROM tags WHERE name = ?;
    )SQL";
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), kSql, -1, &raw_stmt, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<core::Tag>::Err(
            make_storage_error("find_tag_by_name: prepare", db_.get()));
    }
    StmtHandle stmt(raw_stmt);
    sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return core::Result<core::Tag>::Ok(read_tag_row(stmt.get()));
    }
    if (rc == SQLITE_DONE) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "find_tag_by_name: not found"));
    }
    return core::Result<core::Tag>::Err(
        rc_to_error(rc, "find_tag_by_name: step", db_.get()));
}

core::Result<std::vector<core::Tag>> StorageEngine::get_entry_tags(int64_t entry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Result<std::vector<core::Tag>>::Err(
            core::Error(core::ErrorCode::StorageError, "database not opened"));
    }
    return core::Result<std::vector<core::Tag>>::Ok(read_entry_tags_unlocked(entry_id));
}

core::Error StorageEngine::set_entry_tags(int64_t entry_id,
                                            const std::vector<int64_t>& tag_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_entry_tags_unlocked(entry_id, tag_ids);
}

core::Error StorageEngine::migrate_v2_to_v3(
    const std::function<core::Result<core::VaultItem>(core::VaultItem)>& rewrap) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        return core::Error(core::ErrorCode::StorageError, "database not opened");
    }
    if (schema_version_ >= 3) {
        return core::Error{};
    }

    // foreign_keys cannot be toggled inside a transaction.
    core::Error err = exec_sql("PRAGMA foreign_keys = OFF;");
    if (!err.ok()) return err;

    err = exec_sql("BEGIN IMMEDIATE;");
    if (!err.ok()) {
        (void)exec_sql("PRAGMA foreign_keys = ON;");
        return err;
    }

    auto abort_migrate = [this](core::Error e) {
        (void)exec_sql("ROLLBACK;");
        (void)exec_sql("PRAGMA foreign_keys = ON;");
        schema_version_ = 2;
        return e;
    };

    err = exec_sql("DROP TABLE IF EXISTS vault_items;");
    if (!err.ok()) return abort_migrate(err);

    err = exec_sql(R"SQL(
        CREATE TABLE vault_items (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            type        INTEGER NOT NULL,
            title       TEXT NOT NULL,
            payload     BLOB NOT NULL,
            iv          BLOB NOT NULL,
            tag         BLOB NOT NULL,
            created_at  INTEGER NOT NULL,
            updated_at  INTEGER NOT NULL
        );
    )SQL");
    if (!err.ok()) return abort_migrate(err);

    sqlite3_stmt* probe = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(),
        "SELECT id, entry_name, account, username, password, website, note, "
        "iv, tag, created_at, updated_at FROM passwords;",
        -1, &probe, nullptr);
    if (rc != SQLITE_OK) {
        return abort_migrate(make_storage_error(
            "migrate_v2_to_v3: select passwords", db_.get()));
    }
    StmtHandle list_stmt(probe);

    const char* kInsert = R"SQL(
        INSERT INTO vault_items
            (id, type, title, payload, iv, tag, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )SQL";

    while (true) {
        rc = sqlite3_step(list_stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return abort_migrate(
                rc_to_error(rc, "migrate_v2_to_v3: step passwords", db_.get()));
        }
        core::VaultItem row = read_v2_row(list_stmt.get());
        auto converted = rewrap(std::move(row));
        if (!converted) {
            return abort_migrate(converted.error());
        }
        const core::VaultItem& item = *converted;
        sqlite3_stmt* ins = nullptr;
        rc = sqlite3_prepare_v2(db_.get(), kInsert, -1, &ins, nullptr);
        if (rc != SQLITE_OK) {
            return abort_migrate(make_storage_error(
                "migrate_v2_to_v3: insert prepare", db_.get()));
        }
        StmtHandle ins_stmt(ins);
        const core::ByteVec payload = payload_for_store(item);
        sqlite3_bind_int64(ins_stmt.get(), 1, item.id);
        sqlite3_bind_int(ins_stmt.get(), 2, static_cast<int>(item.type));
        sqlite3_bind_text(ins_stmt.get(), 3, item.title.c_str(), -1, SQLITE_TRANSIENT);
        bind_blob_safe(ins_stmt.get(), 4, payload.data(), static_cast<int>(payload.size()));
        bind_blob_safe(ins_stmt.get(), 5, item.iv.data(), static_cast<int>(item.iv.size()));
        bind_blob_safe(ins_stmt.get(), 6, item.tag.data(), static_cast<int>(item.tag.size()));
        sqlite3_bind_int64(ins_stmt.get(), 7, item.created_at);
        sqlite3_bind_int64(ins_stmt.get(), 8, item.updated_at);
        rc = sqlite3_step(ins_stmt.get());
        if (rc != SQLITE_DONE) {
            return abort_migrate(
                rc_to_error(rc, "migrate_v2_to_v3: insert step", db_.get()));
        }
    }
    list_stmt.reset();

    bool has_entry_tags = false;
    {
        sqlite3_stmt* probe_tags = nullptr;
        rc = sqlite3_prepare_v2(db_.get(),
            "SELECT name FROM sqlite_master WHERE type='table' AND name='entry_tags';",
            -1, &probe_tags, nullptr);
        if (rc == SQLITE_OK && probe_tags != nullptr) {
            StmtHandle sp(probe_tags);
            has_entry_tags = sqlite3_step(sp.get()) == SQLITE_ROW;
        }
    }
    if (has_entry_tags) {
        err = exec_sql("ALTER TABLE entry_tags RENAME TO entry_tags_v2;");
        if (!err.ok()) return abort_migrate(err);
    }
    err = exec_sql(R"SQL(
        CREATE TABLE entry_tags (
            entry_id    INTEGER NOT NULL,
            tag_id      INTEGER NOT NULL,
            PRIMARY KEY (entry_id, tag_id),
            FOREIGN KEY (entry_id) REFERENCES vault_items(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)   REFERENCES tags(id) ON DELETE CASCADE
        );
    )SQL");
    if (!err.ok()) return abort_migrate(err);
    if (has_entry_tags) {
        err = exec_sql(
            "INSERT INTO entry_tags (entry_id, tag_id) SELECT entry_id, tag_id FROM entry_tags_v2;");
        if (!err.ok()) return abort_migrate(err);
        err = exec_sql("DROP TABLE entry_tags_v2;");
        if (!err.ok()) return abort_migrate(err);
    }
    err = exec_sql("DROP TABLE passwords;");
    if (!err.ok()) return abort_migrate(err);
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_vault_items_title ON vault_items(title);");
    if (!err.ok()) return abort_migrate(err);
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_vault_items_type ON vault_items(type);");
    if (!err.ok()) return abort_migrate(err);
    err = exec_sql("CREATE INDEX IF NOT EXISTS idx_entry_tags_tag ON entry_tags(tag_id);");
    if (!err.ok()) return abort_migrate(err);
    err = write_schema_version(3);
    if (!err.ok()) return abort_migrate(err);
    err = exec_sql("COMMIT;");
    if (!err.ok()) return abort_migrate(err);
    (void)exec_sql("PRAGMA foreign_keys = ON;");
    return core::Error{};
}

}  // namespace yuli::vault::storage
