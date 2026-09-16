// coding: utf-8
// =============================================================================
// IStorageEngine.h
//
// 存储引擎抽象接口。服务进程通过此接口操作密码条目库；
// 具体实现可以是 SQLite 持久化实现（生产）或内存实现（单元测试）。
//
// 实现方须保证：
//   - 所有方法线程安全（服务进程可能并发调用）
//   - 事务方法（begin/commit/rollback）可重入嵌套或显式禁止嵌套（由实现决定）
//   - `add_entry` 返回的条目包含新分配的 id（!= 0）
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "Error.h"
#include "Result.h"
#include "Types.h"

namespace yuli::vault::core {

/// 存储引擎抽象接口。
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    /// 新增条目。
    /// \param entry 待新增条目，`id` 通常为 0（由实现分配）
    /// \return 成功时返回带分配 id 的条目；失败时返回错误
    virtual Result<PasswordEntry> add_entry(const PasswordEntry& entry) = 0;

    /// 更新已存在条目。`entry.id` 必须非 0。
    virtual Result<PasswordEntry> update_entry(const PasswordEntry& entry) = 0;

    /// 删除指定 id 的条目。
    virtual Error remove_entry(int64_t id) = 0;

    /// 按 id 获取单个条目。
    virtual Result<PasswordEntry> get_entry(int64_t id) = 0;

    /// 按 SearchQuery 条件搜索条目。
    virtual Result<std::vector<PasswordEntry>> search_entries(const SearchQuery& query) = 0;

    /// 列出全部条目（按 updated_at 倒序由实现决定）。
    virtual Result<std::vector<PasswordEntry>> list_entries() = 0;

    /// Current schema_version setting (2 = legacy passwords table, 3 = vault_items).
    virtual int schema_version() = 0;

    /// Rewrite a v2 `passwords` table into v3 `vault_items`. No-op if already v3.
    /// `rewrap` converts a v2 row (login fields + optional password-only iv/tag)
    /// into the v3 on-disk form (payload + iv/tag).
    virtual Error migrate_v2_to_v3(
        const std::function<Result<VaultItem>(VaultItem)>& rewrap) = 0;

    /// 开启事务。
    virtual Error begin_transaction() = 0;

    /// 提交事务。
    virtual Error commit_transaction() = 0;

    /// 回滚事务。
    virtual Error rollback_transaction() = 0;

    // -----------------------------------------------------------------------
    // 生成器历史记录
    // -----------------------------------------------------------------------
    // 与 PasswordEntry 类似的加密/解密策略：
    //   - 明文模式（未启用程序密码）：password 为明文，iv/tag 为空
    //   - 加密模式：password 为密文 BLOB，iv/tag 由 ICryptoEngine 填充
    // 调用方（ServiceCore）负责加解密；存储引擎只做 BLOB 存取。

    /// 新增一条生成记录。`record.id` 通常为 0（由实现分配）。
    /// \return 成功时返回带分配 id 与 created_at 的记录
    virtual Result<GeneratedPasswordRecord> add_generated_record(
        const GeneratedPasswordRecord& record) = 0;

    /// 更新已存在生成记录的加密字段（password/length/iv/tag）。`record.id` 必须非 0。
    /// created_at 保持不变（用于 enable/disable 程序密码时重加密但不丢失时间戳）。
    /// 未找到 id 时返回 NotFound。
    virtual Result<GeneratedPasswordRecord> update_generated_record(
        const GeneratedPasswordRecord& record) = 0;

    /// 列出全部生成记录（按 created_at 倒序）。
    virtual Result<std::vector<GeneratedPasswordRecord>> list_generated_records() = 0;

    /// 按 id 删除单条生成记录。
    virtual Error remove_generated_record(int64_t id) = 0;

    /// 清空全部生成记录。
    virtual Error clear_generated_records() = 0;

    // -----------------------------------------------------------------------
    // 通用 KV 设置存储
    // -----------------------------------------------------------------------
    // 用于持久化少量应用级设置（如生成器历史记录上限、schema 版本号）。
    // key/value 均为字符串，由调用方负责类型转换。

    /// 读取设置项；不存在时返回空字符串（不视为错误）。
    virtual Result<std::string> get_setting(const std::string& key) = 0;

    /// 写入或更新设置项；value 为空字符串等价于删除该 key。
    virtual Error set_setting(const std::string& key, const std::string& value) = 0;

    // -----------------------------------------------------------------------
    // 标签（Tag）管理
    // -----------------------------------------------------------------------
    // 标签独立存储，通过 entry_tags 关联表与 passwords 多对多关联。
    // 实现须保证：
    //   - `tag.name` 全库唯一
    //   - 删除条目时自动级联清理关联（CASCADE）
    //   - 删除标签时自动级联清理关联（CASCADE）

    /// 新增标签。`tag.id` 通常为 0（由实现分配）。
    /// \return 成功时返回带分配 id 与时间戳的标签；name 冲突时返回 AlreadyExists
    virtual Result<Tag> add_tag(const Tag& tag) = 0;

    /// 更新已存在标签的 name/color。`tag.id` 必须非 0；created_at 不变。
    virtual Result<Tag> update_tag(const Tag& tag) = 0;

    /// 按 id 删除标签，并级联清理 entry_tags 关联。
    virtual Error remove_tag(int64_t id) = 0;

    /// 列出全部标签（按 name 升序）。
    virtual Result<std::vector<Tag>> list_tags() = 0;

    /// 按 id 获取单个标签。
    virtual Result<Tag> get_tag(int64_t id) = 0;

    /// 按 name 查找标签；不存在返回 NotFound。
    virtual Result<Tag> find_tag_by_name(const std::string& name) = 0;

    /// 获取指定条目关联的所有标签。
    virtual Result<std::vector<Tag>> get_entry_tags(int64_t entry_id) = 0;

    /// 全量替换指定条目的标签关联。
    /// \param entry_id 条目 id
    /// \param tag_ids 标签 id 列表（重复项由实现去重；不存在的 id 静默跳过）
    virtual Error set_entry_tags(int64_t entry_id,
                                  const std::vector<int64_t>& tag_ids) = 0;
};

}  // namespace yuli::vault::core
