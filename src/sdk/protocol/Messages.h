// coding: utf-8
// =============================================================================
// Messages.h
//
// PwdVault IPC 协议消息结构。每个 CommandId 对应一对 Request/Response 结构。
// 消息在传输时由 Serializer.h/cpp 序列化为字节流，并由 MessageHeader 描述
// 帧的元信息（magic/version/command/request_id/payload_size）。
//
// 设计要点：
//   - 所有结构为普通聚合（POD-like），由 Serializer 通过模板特化进行二进制
//     序列化，避免引入 JSON/protobuf 等外部依赖。
//   - 字符串字段使用 std::string，二进制字段使用 core::ByteVec，序列化时
//     以长度前缀（uint32_t）+ 原始字节的方式编码（见 Serializer.h）。
//   - MessageHeader 大小固定 16 字节，由 static_assert 强制保证，便于
//     parse_header 在管道字节流中按固定偏移量定位 payload。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Commands.h"
#include "Error.h"
#include "Types.h"

namespace yuli::vault::protocol {

/// 协议魔数 "PDVV"（小端序：0x56 0x44 0x56 0x50）。
/// 用于在字节流头部快速识别 PwdVault IPC 帧，避免误解析其他来源数据。
inline constexpr uint32_t kMagic = 0x50564456u;

/// 协议版本号。当前为 1。
///   - 未来若引入不兼容变更（如字段重排、新枚举语义），递增此值。
///   - 同一版本内向后兼容追加字段，需在 Serializer 中处理缺省值。
/// Current protocol version. Bumped to 2 when VaultItem gained a type byte.
inline constexpr uint16_t kProtocolVersion = 2;

/// 消息头（固定 16 字节）。
///
/// 字段布局（小端序）：
///   offset  size  field
///   0       4     magic          必须为 kMagic
///   4       2     version        协议版本，当前为 1
///   6       2     command        CommandId（按 uint16_t 传输）
///   8       4     request_id     用于异步匹配请求与响应
///   12      4     payload_size   负载字节数（不含 header）
struct MessageHeader {
    uint32_t magic = kMagic;
    uint16_t version = kProtocolVersion;
    CommandId command = CommandId::Ping;
    uint32_t request_id = 0;
    uint32_t payload_size = 0;
};
static_assert(sizeof(MessageHeader) == 16,
              "MessageHeader must be exactly 16 bytes for fixed-offset parsing");

// ---------------------------------------------------------------------------
// 请求消息
// ---------------------------------------------------------------------------

/// 心跳请求（无负载）。响应为 PingResponse 携带 service 端时间戳。
struct PingRequest {};

/// 关闭请求（无负载）。service 收到后执行优雅退出。
struct ShutdownRequest {};

/// 解锁请求。验证程序密码以恢复会话（仅在程序密码已启用且 vault 处于锁定状态时使用）。
struct UnlockRequest {
    std::string password;
};

/// 锁定请求（无负载）。清除 service 内存中的加密密钥。
struct LockRequest {};

/// 启用程序密码请求。将明文库转为加密库：生成加密密钥并用程序密码包装后写入 vault.meta，
/// 同时重新加密所有现有条目。
struct EnableProgramPasswordRequest {
    std::string password;
};

/// 禁用程序密码请求。验证当前程序密码后，解密所有条目转回明文存储，并删除 vault.meta。
struct DisableProgramPasswordRequest {
    std::string password;
};

/// 修改程序密码请求。验证旧密码后，用新密码重新包装加密密钥（加密密钥本身不变，条目无需重新加密）。
struct ChangeProgramPasswordRequest {
    std::string old_password;
    std::string new_password;
};

/// 查询 vault 状态请求（无负载）。
struct GetVaultStatusRequest {};

/// 新增条目请求。entry.id 通常为 0，由 service 分配后返回。
struct AddEntryRequest {
    core::PasswordEntry entry;
};

/// 更新已存在条目请求。entry.id 必须非 0。
struct UpdateEntryRequest {
    core::PasswordEntry entry;
};

/// 删除条目请求。
struct RemoveEntryRequest {
    int64_t id = 0;
};

/// 获取单条条目请求。
struct GetEntryRequest {
    int64_t id = 0;
};

/// 搜索条目请求。
struct SearchEntriesRequest {
    core::SearchQuery query;
};

/// 列出全部条目请求（无负载）。
struct ListEntriesRequest {};

/// 生成密码请求。
struct GeneratePasswordRequest {
    core::PasswordGeneratorOptions options;
};

/// 评估密码强度请求。
struct EstimateStrengthRequest {
    std::string password;
};

/// 列出生成器历史记录请求（无负载）。
struct ListGeneratedRecordsRequest {};

/// 删除单条生成记录请求。
struct RemoveGeneratedRecordRequest {
    int64_t id = 0;
};

/// 清空全部生成记录请求（无负载）。
struct ClearGeneratedRecordsRequest {};

/// 查询生成器设置请求（无负载）。
struct GetGeneratorSettingsRequest {};

/// 设置历史记录上限请求。
/// \note limit = 0 表示无限制；正整数表示保留最近 N 条。
struct SetGeneratorLimitRequest {
    int32_t limit = 0;
};

// ---------------------------------------------------------------------------
// 标签（Tag）相关请求
// ---------------------------------------------------------------------------

/// 新增标签请求。tag.id 通常为 0，由 service 分配后返回。
struct AddTagRequest {
    core::Tag tag;
};

/// 更新已存在标签请求。tag.id 必须非 0。
struct UpdateTagRequest {
    core::Tag tag;
};

/// 删除标签请求。
struct RemoveTagRequest {
    int64_t id = 0;
};

/// 列出全部标签请求（无负载）。
struct ListTagsRequest {};

/// 按 id 获取单条标签请求。
struct GetTagRequest {
    int64_t id = 0;
};

/// 按 name 查找标签请求。
struct FindTagByNameRequest {
    std::string name;
};

/// 获取指定条目的全部标签请求。
struct GetEntryTagsRequest {
    int64_t entry_id = 0;
};

/// 全量替换指定条目的标签关联请求。
/// \note 重复项由实现去重；不存在的 tag_id 静默跳过。
struct SetEntryTagsRequest {
    int64_t entry_id = 0;
    std::vector<int64_t> tag_ids;
};

// ---------------------------------------------------------------------------
// 响应消息
// ---------------------------------------------------------------------------

/// 心跳响应。携带 service 端 Unix 时间戳（秒）。
struct PingResponse {
    uint64_t server_timestamp = 0;
};

/// 关闭响应（无负载）。
struct ShutdownResponse {};

/// 解锁响应。
struct UnlockResponse {
    bool success = false;
    std::string error_message;
};

/// 锁定响应（无负载）。
struct LockResponse {};

/// 启用程序密码响应。
struct EnableProgramPasswordResponse {
    bool success = false;
    std::string error_message;
};

/// 禁用程序密码响应。
struct DisableProgramPasswordResponse {
    bool success = false;
    std::string error_message;
};

/// 修改程序密码响应。
struct ChangeProgramPasswordResponse {
    bool success = false;
    std::string error_message;
};

/// vault 状态查询响应。
struct GetVaultStatusResponse {
    bool password_enabled = false;  ///< 是否已启用程序密码（vault.meta 是否存在）
    bool is_locked = false;          ///< 是否处于锁定状态（password_enabled && !unlocked）
};

/// 新增条目响应。entry.id 为 service 分配的主键。
struct AddEntryResponse {
    core::PasswordEntry entry;
};

/// 更新条目响应。entry 为更新后的最新值。
struct UpdateEntryResponse {
    core::PasswordEntry entry;
};

/// 删除条目响应（无负载）。
struct RemoveEntryResponse {};

/// 获取条目响应。
struct GetEntryResponse {
    core::PasswordEntry entry;
};

/// 搜索条目响应。
struct SearchEntriesResponse {
    std::vector<core::PasswordEntry> entries;
};

/// 列出条目响应。
struct ListEntriesResponse {
    std::vector<core::PasswordEntry> entries;
};

/// 生成密码响应。
struct GeneratePasswordResponse {
    std::string password;
};

/// 评估强度响应。携带完整的 StrengthEstimate（bits / level / score / warnings）。
struct EstimateStrengthResponse {
    core::StrengthEstimate estimate;
};

/// 列出生成记录响应。records 已按 created_at 倒序排列。
struct ListGeneratedRecordsResponse {
    std::vector<core::GeneratedPasswordRecord> records;
};

/// 删除单条生成记录响应（无负载）。
struct RemoveGeneratedRecordResponse {};

/// 清空全部生成记录响应（无负载）。
struct ClearGeneratedRecordsResponse {};

/// 查询生成器设置响应。
struct GetGeneratorSettingsResponse {
    int32_t history_limit = 0;  ///< 历史记录上限；0 表示无限制
};

/// 设置历史记录上限响应。
struct SetGeneratorLimitResponse {
    bool success = false;
};

// ---------------------------------------------------------------------------
// 标签（Tag）相关响应
// ---------------------------------------------------------------------------

/// 新增标签响应。tag.id 为 service 分配的主键。
struct AddTagResponse {
    core::Tag tag;
};

/// 更新标签响应。tag 为更新后的最新值。
struct UpdateTagResponse {
    core::Tag tag;
};

/// 删除标签响应（无负载）。
struct RemoveTagResponse {};

/// 列出全部标签响应。tags 已按 name 升序排列。
struct ListTagsResponse {
    std::vector<core::Tag> tags;
};

/// 获取单条标签响应。
struct GetTagResponse {
    core::Tag tag;
};

/// 按 name 查找标签响应。
struct FindTagByNameResponse {
    core::Tag tag;
};

/// 获取条目标签响应。
struct GetEntryTagsResponse {
    std::vector<core::Tag> tags;
};

/// 设置条目标签关联响应（无负载）。
struct SetEntryTagsResponse {};

// ---------------------------------------------------------------------------
// 通用错误响应
// ---------------------------------------------------------------------------

/// 通用错误响应。当请求处理失败且无具体响应结构时使用。
struct ErrorResponse {
    core::ErrorCode code = core::ErrorCode::None;
    std::string message;
};

}  // namespace yuli::vault::protocol
