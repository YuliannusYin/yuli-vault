// coding: utf-8
// =============================================================================
// InMemoryStorageEngine.cpp
//
// 纯内存存储引擎实现。所有方法线程安全（mutex 串行化）。
// 事务通过快照实现：begin 时深拷贝全部状态，rollback 时恢复；
// 不支持嵌套事务。所有 mutating 方法在事务外也照常工作。
// =============================================================================
#include "InMemoryStorageEngine.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

#include "VaultPayload.h"

namespace yuli::vault::storage {

namespace {

/// 将字符串转小写（ASCII），用于大小写不敏感的子串匹配。
std::string to_lower_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// 子串匹配：case_sensitive 控制是否区分大小写。
bool contains_substring(const std::string& haystack, const std::string& needle,
                        bool case_sensitive) {
    if (needle.empty()) {
        return true;
    }
    if (case_sensitive) {
        return haystack.find(needle) != std::string::npos;
    }
    return to_lower_ascii(haystack).find(to_lower_ascii(needle)) !=
           std::string::npos;
}

}  // namespace

int64_t InMemoryStorageEngine::now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::vector<core::PasswordEntry>::iterator InMemoryStorageEngine::find_by_id(
    int64_t id) {
    return std::find_if(entries_.begin(), entries_.end(),
                        [id](const core::PasswordEntry& e) { return e.id == id; });
}

core::Result<core::PasswordEntry> InMemoryStorageEngine::add_entry(
    const core::PasswordEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    core::PasswordEntry out = entry;
    out.id = next_id_++;
    const int64_t ts = now_seconds();
    out.created_at = ts;
    out.updated_at = ts;

    // 同步标签关联：entry.tags 中的 id 写入 entry_tags_。
    std::vector<int64_t> tag_ids;
    tag_ids.reserve(out.tags.size());
    for (const auto& t : out.tags) {
        if (t.id != 0) tag_ids.push_back(t.id);
    }
    auto tag_err = set_entry_tags_unlocked(out.id, tag_ids);
    if (!tag_err.ok()) {
        return core::Result<core::PasswordEntry>::Err(tag_err);
    }
    // 返回前填充完整 tags（含 name/color 等元数据）
    out.tags = read_entry_tags_unlocked(out.id);

    entries_.push_back(out);
    return core::Result<core::PasswordEntry>::Ok(std::move(out));
}

core::Result<core::PasswordEntry> InMemoryStorageEngine::update_entry(
    const core::PasswordEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entry.id == 0) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::InvalidArgument,
                        "update_entry: id must not be 0"));
    }
    auto it = find_by_id(entry.id);
    if (it == entries_.end()) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::NotFound, "update_entry: id not found"));
    }
    core::PasswordEntry out = entry;
    out.created_at = it->created_at;  // 保留原始创建时间
    out.updated_at = now_seconds();

    // 同步标签关联（全量替换）
    std::vector<int64_t> tag_ids;
    tag_ids.reserve(out.tags.size());
    for (const auto& t : out.tags) {
        if (t.id != 0) tag_ids.push_back(t.id);
    }
    auto tag_err = set_entry_tags_unlocked(out.id, tag_ids);
    if (!tag_err.ok()) {
        return core::Result<core::PasswordEntry>::Err(tag_err);
    }
    out.tags = read_entry_tags_unlocked(out.id);

    *it = out;
    return core::Result<core::PasswordEntry>::Ok(std::move(out));
}

core::Error InMemoryStorageEngine::remove_entry(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_by_id(id);
    if (it == entries_.end()) {
        return core::Error(core::ErrorCode::NotFound,
                           "remove_entry: id not found");
    }
    entries_.erase(it);
    // 级联清理 entry_tags 关联
    auto remove_it = std::remove_if(entry_tags_.begin(), entry_tags_.end(),
                                    [id](const std::pair<int64_t, int64_t>& p) {
                                        return p.first == id;
                                    });
    entry_tags_.erase(remove_it, entry_tags_.end());
    return core::Error{};
}

core::Result<core::PasswordEntry> InMemoryStorageEngine::get_entry(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_by_id(id);
    if (it == entries_.end()) {
        return core::Result<core::PasswordEntry>::Err(
            core::Error(core::ErrorCode::NotFound, "get_entry: id not found"));
    }
    core::PasswordEntry out = *it;
    fill_entry_tags_unlocked(out);
    return core::Result<core::PasswordEntry>::Ok(std::move(out));
}

core::Result<std::vector<core::PasswordEntry>>
InMemoryStorageEngine::search_entries(const core::SearchQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto entry_matches_tags = [this](int64_t entry_id,
                                      const std::vector<int64_t>& tag_ids) {
        if (tag_ids.empty()) return true;
        for (auto tid : tag_ids) {
            // entry_id 是否关联了 tid
            auto found = std::find_if(
                entry_tags_.begin(), entry_tags_.end(),
                [entry_id, tid](const std::pair<int64_t, int64_t>& p) {
                    return p.first == entry_id && p.second == tid;
                });
            if (found != entry_tags_.end()) return true;  // OR 语义
        }
        return false;
    };

    std::vector<core::PasswordEntry> results;
    for (const auto& e : entries_) {
        if (!core::item_matches_text_query(e, query)) continue;
        if (!entry_matches_tags(e.id, query.tag_ids)) continue;
        results.push_back(e);
    }

    // 按 created_at 倒序，与 SQLite 实现保持一致。
    std::sort(results.begin(), results.end(),
              [](const core::PasswordEntry& a, const core::PasswordEntry& b) {
                  return a.created_at > b.created_at;
              });
    // 填充 tags
    for (auto& e : results) {
        fill_entry_tags_unlocked(e);
    }
    return core::Result<std::vector<core::PasswordEntry>>::Ok(std::move(results));
}

core::Result<std::vector<core::PasswordEntry>>
InMemoryStorageEngine::list_entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::PasswordEntry> results = entries_;
    std::sort(results.begin(), results.end(),
              [](const core::PasswordEntry& a, const core::PasswordEntry& b) {
                  return a.created_at > b.created_at;
              });
    for (auto& e : results) {
        fill_entry_tags_unlocked(e);
    }
    return core::Result<std::vector<core::PasswordEntry>>::Ok(std::move(results));
}

core::Error InMemoryStorageEngine::begin_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 不支持嵌套事务：若已在事务中则返回错误。
    if (txn_snapshot_.has_value()) {
        return core::Error(core::ErrorCode::StorageError,
                           "begin_transaction: nested transaction not supported");
    }
    Snapshot s;
    s.entries = entries_;
    s.next_id = next_id_;
    s.gen_records = gen_records_;
    s.gen_next_id = gen_next_id_;
    s.settings = settings_;
    s.tags = tags_;
    s.tag_next_id = tag_next_id_;
    s.entry_tags = entry_tags_;
    txn_snapshot_ = std::move(s);
    return core::Error{};
}

core::Error InMemoryStorageEngine::commit_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!txn_snapshot_.has_value()) {
        return core::Error(core::ErrorCode::StorageError,
                           "commit_transaction: no active transaction");
    }
    txn_snapshot_.reset();  // 丢弃快照，保留当前状态
    return core::Error{};
}

core::Error InMemoryStorageEngine::rollback_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!txn_snapshot_.has_value()) {
        return core::Error(core::ErrorCode::StorageError,
                           "rollback_transaction: no active transaction");
    }
    entries_ = std::move(txn_snapshot_->entries);
    next_id_ = txn_snapshot_->next_id;
    gen_records_ = std::move(txn_snapshot_->gen_records);
    gen_next_id_ = txn_snapshot_->gen_next_id;
    settings_ = std::move(txn_snapshot_->settings);
    tags_ = std::move(txn_snapshot_->tags);
    tag_next_id_ = txn_snapshot_->tag_next_id;
    entry_tags_ = std::move(txn_snapshot_->entry_tags);
    txn_snapshot_.reset();
    return core::Error{};
}

// =============================================================================
// 生成器历史记录
// =============================================================================

std::vector<core::GeneratedPasswordRecord>::iterator
InMemoryStorageEngine::find_gen_by_id(int64_t id) {
    return std::find_if(gen_records_.begin(), gen_records_.end(),
                        [id](const core::GeneratedPasswordRecord& r) { return r.id == id; });
}

core::Result<core::GeneratedPasswordRecord> InMemoryStorageEngine::add_generated_record(
    const core::GeneratedPasswordRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    core::GeneratedPasswordRecord out = record;
    out.id = gen_next_id_++;
    out.created_at = now_seconds();
    gen_records_.push_back(out);
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(out));
}

core::Result<core::GeneratedPasswordRecord> InMemoryStorageEngine::update_generated_record(
    const core::GeneratedPasswordRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record.id == 0) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::InvalidArgument,
                        "update_generated_record: id must not be 0"));
    }
    auto it = find_gen_by_id(record.id);
    if (it == gen_records_.end()) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            core::Error(core::ErrorCode::NotFound,
                        "update_generated_record: id not found"));
    }
    // 保留原始 created_at，仅更新 password/length/iv/tag
    core::GeneratedPasswordRecord out = record;
    out.created_at = it->created_at;
    *it = out;
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(out));
}

core::Result<std::vector<core::GeneratedPasswordRecord>>
InMemoryStorageEngine::list_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::GeneratedPasswordRecord> results = gen_records_;
    std::sort(results.begin(), results.end(),
              [](const core::GeneratedPasswordRecord& a,
                 const core::GeneratedPasswordRecord& b) {
                  if (a.created_at != b.created_at) return a.created_at > b.created_at;
                  return a.id > b.id;
              });
    return core::Result<std::vector<core::GeneratedPasswordRecord>>::Ok(std::move(results));
}

core::Error InMemoryStorageEngine::remove_generated_record(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_gen_by_id(id);
    if (it == gen_records_.end()) {
        return core::Error(core::ErrorCode::NotFound,
                           "remove_generated_record: id not found");
    }
    gen_records_.erase(it);
    return core::Error{};
}

core::Error InMemoryStorageEngine::clear_generated_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    gen_records_.clear();
    return core::Error{};
}

// =============================================================================
// 通用 KV 设置
// =============================================================================

std::vector<std::pair<std::string, std::string>>::iterator
InMemoryStorageEngine::find_setting(const std::string& key) {
    return std::find_if(settings_.begin(), settings_.end(),
                        [&key](const std::pair<std::string, std::string>& kv) {
                            return kv.first == key;
                        });
}

core::Result<std::string> InMemoryStorageEngine::get_setting(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_setting(key);
    if (it == settings_.end()) {
        return core::Result<std::string>::Ok(std::string{});
    }
    return core::Result<std::string>::Ok(it->second);
}

core::Error InMemoryStorageEngine::set_setting(const std::string& key,
                                                const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_setting(key);
    if (value.empty()) {
        if (it != settings_.end()) {
            settings_.erase(it);
        }
        return core::Error{};
    }
    if (it == settings_.end()) {
        settings_.emplace_back(key, value);
    } else {
        it->second = value;
    }
    return core::Error{};
}

// =============================================================================
// 标签（Tag）与 entry-tag 关联
// =============================================================================

std::vector<core::Tag>::iterator
InMemoryStorageEngine::find_tag_by_id_unlocked(int64_t id) {
    return std::find_if(tags_.begin(), tags_.end(),
                        [id](const core::Tag& t) { return t.id == id; });
}

std::vector<core::Tag> InMemoryStorageEngine::read_entry_tags_unlocked(
    int64_t entry_id) const {
    std::vector<core::Tag> result;
    for (const auto& [eid, tid] : entry_tags_) {
        if (eid != entry_id) continue;
        // 在 tags_ 中查找对应的 Tag
        auto it = std::find_if(tags_.begin(), tags_.end(),
                                [tid](const core::Tag& t) { return t.id == tid; });
        if (it != tags_.end()) {
            result.push_back(*it);
        }
    }
    // 按 name 升序，与 SQLite 实现保持一致
    std::sort(result.begin(), result.end(),
              [](const core::Tag& a, const core::Tag& b) { return a.name < b.name; });
    return result;
}

core::Error InMemoryStorageEngine::set_entry_tags_unlocked(
    int64_t entry_id, const std::vector<int64_t>& tag_ids) {
    // 1. 删除现有所有关联
    auto remove_it = std::remove_if(
        entry_tags_.begin(), entry_tags_.end(),
        [entry_id](const std::pair<int64_t, int64_t>& p) {
            return p.first == entry_id;
        });
    entry_tags_.erase(remove_it, entry_tags_.end());

    // 2. 去重写入新关联（跳过 id==0 与不存在的 tag_id）
    if (tag_ids.empty()) return core::Error{};
    std::vector<int64_t> unique_ids;
    unique_ids.reserve(tag_ids.size());
    for (auto tid : tag_ids) {
        if (tid == 0) continue;
        if (std::find(unique_ids.begin(), unique_ids.end(), tid) == unique_ids.end()) {
            unique_ids.push_back(tid);
        }
    }
    for (auto tid : unique_ids) {
        // 静默跳过不存在的 tag_id
        auto it = std::find_if(tags_.begin(), tags_.end(),
                                [tid](const core::Tag& t) { return t.id == tid; });
        if (it == tags_.end()) continue;
        entry_tags_.emplace_back(entry_id, tid);
    }
    return core::Error{};
}

void InMemoryStorageEngine::fill_entry_tags_unlocked(core::PasswordEntry& entry) const {
    entry.tags = read_entry_tags_unlocked(entry.id);
}

core::Result<core::Tag> InMemoryStorageEngine::add_tag(const core::Tag& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tag.name.empty()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "add_tag: name must not be empty"));
    }
    // 名称唯一性检查（大小写敏感）
    auto it = std::find_if(tags_.begin(), tags_.end(),
                            [&tag](const core::Tag& t) { return t.name == tag.name; });
    if (it != tags_.end()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::AlreadyExists,
                        "add_tag: tag name already exists"));
    }
    core::Tag out = tag;
    out.id = tag_next_id_++;
    const int64_t ts = now_seconds();
    out.created_at = ts;
    out.updated_at = ts;
    tags_.push_back(out);
    return core::Result<core::Tag>::Ok(std::move(out));
}

core::Result<core::Tag> InMemoryStorageEngine::update_tag(const core::Tag& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tag.id == 0) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "update_tag: id must not be 0"));
    }
    if (tag.name.empty()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::InvalidArgument, "update_tag: name must not be empty"));
    }
    auto it = find_tag_by_id_unlocked(tag.id);
    if (it == tags_.end()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "update_tag: id not found"));
    }
    // 名称唯一性检查（排除自身）
    auto dup = std::find_if(tags_.begin(), tags_.end(),
                            [&tag](const core::Tag& t) {
                                return t.name == tag.name && t.id != tag.id;
                            });
    if (dup != tags_.end()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::AlreadyExists,
                        "update_tag: tag name already exists"));
    }
    core::Tag out = tag;
    out.created_at = it->created_at;  // 保留原始创建时间
    out.updated_at = now_seconds();
    *it = out;
    return core::Result<core::Tag>::Ok(std::move(out));
}

core::Error InMemoryStorageEngine::remove_tag(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_tag_by_id_unlocked(id);
    if (it == tags_.end()) {
        return core::Error(core::ErrorCode::NotFound, "remove_tag: id not found");
    }
    tags_.erase(it);
    // 级联清理 entry_tags 关联
    auto remove_it = std::remove_if(entry_tags_.begin(), entry_tags_.end(),
                                    [id](const std::pair<int64_t, int64_t>& p) {
                                        return p.second == id;
                                    });
    entry_tags_.erase(remove_it, entry_tags_.end());
    return core::Error{};
}

core::Result<std::vector<core::Tag>> InMemoryStorageEngine::list_tags() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::Tag> results = tags_;
    std::sort(results.begin(), results.end(),
              [](const core::Tag& a, const core::Tag& b) { return a.name < b.name; });
    return core::Result<std::vector<core::Tag>>::Ok(std::move(results));
}

core::Result<core::Tag> InMemoryStorageEngine::get_tag(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = find_tag_by_id_unlocked(id);
    if (it == tags_.end()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "get_tag: id not found"));
    }
    return core::Result<core::Tag>::Ok(*it);
}

core::Result<core::Tag> InMemoryStorageEngine::find_tag_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(tags_.begin(), tags_.end(),
                            [&name](const core::Tag& t) { return t.name == name; });
    if (it == tags_.end()) {
        return core::Result<core::Tag>::Err(
            core::Error(core::ErrorCode::NotFound, "find_tag_by_name: not found"));
    }
    return core::Result<core::Tag>::Ok(*it);
}

core::Result<std::vector<core::Tag>>
InMemoryStorageEngine::get_entry_tags(int64_t entry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return core::Result<std::vector<core::Tag>>::Ok(read_entry_tags_unlocked(entry_id));
}

core::Error InMemoryStorageEngine::set_entry_tags(int64_t entry_id,
                                                    const std::vector<int64_t>& tag_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_entry_tags_unlocked(entry_id, tag_ids);
}

}  // namespace yuli::vault::storage
