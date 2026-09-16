// coding: utf-8
// =============================================================================
// InMemoryStorageEngine.h
//
// IStorageEngine 的纯内存实现。主要用于：
//   - 单元测试：避免文件系统依赖，加速用例执行。
//   - 未来扩展：可作为缓存层或临时会话存储。
//
// 实现：
//   - 用 std::vector<PasswordEntry> 持有所有条目（含 tags 字段，由关联表填充）。
//   - 用 std::vector<Tag> 持有标签；用 std::vector<pair<int64,int64>> 维护
//     entry-tag 关联（多对多）。
//   - 自增 id 计数器从 1 开始（0 保留给"未入库"语义）。
//   - 事务通过快照实现：begin 时保存所有状态副本，rollback 时恢复；
//     不支持嵌套事务。
//   - 搜索使用 std::string::find 做简单子串匹配；
//     case_sensitive=false 时将两端文本统一转小写后比较。
// =============================================================================
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "IStorageEngine.h"

namespace yuli::vault::storage {

class InMemoryStorageEngine : public core::IStorageEngine {
public:
    InMemoryStorageEngine() = default;
    ~InMemoryStorageEngine() override = default;

    InMemoryStorageEngine(const InMemoryStorageEngine&) = delete;
    InMemoryStorageEngine& operator=(const InMemoryStorageEngine&) = delete;

    core::Result<core::PasswordEntry> add_entry(const core::PasswordEntry& entry) override;
    core::Result<core::PasswordEntry> update_entry(const core::PasswordEntry& entry) override;
    core::Error remove_entry(int64_t id) override;
    core::Result<core::PasswordEntry> get_entry(int64_t id) override;
    core::Result<std::vector<core::PasswordEntry>> search_entries(
        const core::SearchQuery& query) override;
    core::Result<std::vector<core::PasswordEntry>> list_entries() override;
    int schema_version() override { return 3; }
    core::Error migrate_v2_to_v3(
        const std::function<core::Result<core::VaultItem>(core::VaultItem)>&) override {
        return core::Error{};
    }

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
    /// 事务开启时保存的状态快照。
    struct Snapshot {
        std::vector<core::PasswordEntry> entries;
        int64_t next_id = 1;
        std::vector<core::GeneratedPasswordRecord> gen_records;
        int64_t gen_next_id = 1;
        std::vector<std::pair<std::string, std::string>> settings;
        std::vector<core::Tag> tags;
        int64_t tag_next_id = 1;
        std::vector<std::pair<int64_t, int64_t>> entry_tags;
    };

    std::vector<core::PasswordEntry> entries_;
    int64_t next_id_ = 1;
    std::vector<core::GeneratedPasswordRecord> gen_records_;
    int64_t gen_next_id_ = 1;
    std::vector<std::pair<std::string, std::string>> settings_;
    std::vector<core::Tag> tags_;
    int64_t tag_next_id_ = 1;
    /// entry_id → tag_id 关联列表（多对多）
    std::vector<std::pair<int64_t, int64_t>> entry_tags_;
    std::optional<Snapshot> txn_snapshot_;
    std::mutex mutex_;

    /// 在 entries_ 中线性查找指定 id 的迭代器。
    std::vector<core::PasswordEntry>::iterator find_by_id(int64_t id);

    /// 在 gen_records_ 中线性查找指定 id 的迭代器。
    std::vector<core::GeneratedPasswordRecord>::iterator find_gen_by_id(int64_t id);

    /// 在 settings_ 中线性查找指定 key 的迭代器。
    std::vector<std::pair<std::string, std::string>>::iterator find_setting(
        const std::string& key);

    /// 在 tags_ 中线性查找指定 id 的迭代器（调用方持锁）。
    std::vector<core::Tag>::iterator find_tag_by_id_unlocked(int64_t id);

    /// 读取指定 entry_id 关联的所有标签（调用方持锁）。
    std::vector<core::Tag> read_entry_tags_unlocked(int64_t entry_id) const;

    /// 全量替换 entry_id 的标签关联（调用方持锁）。
    /// 不存在的 tag_id 静默跳过；重复项去重。
    core::Error set_entry_tags_unlocked(int64_t entry_id,
                                         const std::vector<int64_t>& tag_ids);

    /// 为单条 PasswordEntry 填充 tags 字段（调用方持锁）。
    void fill_entry_tags_unlocked(core::PasswordEntry& entry) const;

    /// 当前 Unix 时间戳（秒）。
    static int64_t now_seconds();
};

}  // namespace yuli::vault::storage
