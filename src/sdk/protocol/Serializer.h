// coding: utf-8
// =============================================================================
// Serializer.h
//
// PwdVault IPC 协议二进制序列化/反序列化接口。
//
// 设计要点：
//   - 使用二进制序列化（非 JSON），体积小、解析快、零外部依赖。
//   - 字节序：统一小端序。x86/Windows on ARM 均为小端，可直接 memcpy。
//   - 长度前缀：变长字段（std::string、core::ByteVec、std::vector）一律以
//     uint32_t 长度前缀打头，紧接原始字节。
//   - 定长字段（u16/u32/u64/i64/bool/ErrorCode）直接 memcpy 写入。
//   - 复合结构按字段声明顺序逐字段序列化，不写额外 tag。
//   - MessageHeader 固定 16 字节，作为帧的固定前缀；payload 紧随其后。
//   - 版本兼容性：version 字段位于 header；同版本内向后兼容追加字段时，
//     反序列化按当前 schema 读取，多出的尾部字节忽略；不足字段返回错误。
//
// 用法示例：
// \code
//   UnlockRequest req{ "program-pwd" };
//   core::ByteVec payload = serialize(req);
//   core::ByteVec frame  = pack_message(CommandId::Unlock, 42, payload);
//   // ... 通过命名管道发送 frame ...
//
//   auto ph = parse_header(received_span);
//   if (ph) {
//       const auto& [hdr, offset] = ph.value();
//       core::ByteSpan payload_span = received_span.subspan(offset, hdr.payload_size);
//       auto resp = deserialize<UnlockResponse>(payload_span);
//   }
// \endcode
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Commands.h"
#include "Error.h"
#include "Messages.h"
#include "Result.h"
#include "Types.h"

namespace yuli::vault::protocol {

// ---------------------------------------------------------------------------
// 主模板声明（无定义）。每个使用到的类型必须有显式特化。
// ---------------------------------------------------------------------------

/// 将 \p v 序列化为字节向量。
template <typename T>
core::ByteVec serialize(const T& v);

/// 从 \p data 反序列化出 T。
/// 失败时返回持有 ErrorCode::InvalidArgument 的 Result。
template <typename T>
core::Result<T> deserialize(core::ByteSpan data);

// ---------------------------------------------------------------------------
// 基础类型特化声明
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<uint16_t>(const uint16_t&);
template <> core::Result<uint16_t> deserialize<uint16_t>(core::ByteSpan);

template <> core::ByteVec serialize<uint32_t>(const uint32_t&);
template <> core::Result<uint32_t> deserialize<uint32_t>(core::ByteSpan);

template <> core::ByteVec serialize<uint64_t>(const uint64_t&);
template <> core::Result<uint64_t> deserialize<uint64_t>(core::ByteSpan);

template <> core::ByteVec serialize<int64_t>(const int64_t&);
template <> core::Result<int64_t> deserialize<int64_t>(core::ByteSpan);

template <> core::ByteVec serialize<bool>(const bool&);
template <> core::Result<bool> deserialize<bool>(core::ByteSpan);

template <> core::ByteVec serialize<std::string>(const std::string&);
template <> core::Result<std::string> deserialize<std::string>(core::ByteSpan);

template <> core::ByteVec serialize<core::ByteVec>(const core::ByteVec&);
template <> core::Result<core::ByteVec> deserialize<core::ByteVec>(core::ByteSpan);

// ---------------------------------------------------------------------------
// core 类型特化声明
// ---------------------------------------------------------------------------

/// ErrorCode 按 uint32_t 序列化。
template <> core::ByteVec serialize<core::ErrorCode>(const core::ErrorCode&);
template <> core::Result<core::ErrorCode> deserialize<core::ErrorCode>(core::ByteSpan);

template <> core::ByteVec serialize<core::PasswordEntry>(const core::PasswordEntry&);
template <> core::Result<core::PasswordEntry> deserialize<core::PasswordEntry>(core::ByteSpan);

template <> core::ByteVec serialize<core::Tag>(const core::Tag&);
template <> core::Result<core::Tag> deserialize<core::Tag>(core::ByteSpan);

template <> core::ByteVec serialize<core::SearchQuery>(const core::SearchQuery&);
template <> core::Result<core::SearchQuery> deserialize<core::SearchQuery>(core::ByteSpan);

template <> core::ByteVec serialize<core::PasswordGeneratorOptions>(const core::PasswordGeneratorOptions&);
template <> core::Result<core::PasswordGeneratorOptions> deserialize<core::PasswordGeneratorOptions>(core::ByteSpan);

template <> core::ByteVec serialize<core::StrengthEstimate>(const core::StrengthEstimate&);
template <> core::Result<core::StrengthEstimate> deserialize<core::StrengthEstimate>(core::ByteSpan);

// ---------------------------------------------------------------------------
// 请求消息特化声明
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<PingRequest>(const PingRequest&);
template <> core::Result<PingRequest> deserialize<PingRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ShutdownRequest>(const ShutdownRequest&);
template <> core::Result<ShutdownRequest> deserialize<ShutdownRequest>(core::ByteSpan);

template <> core::ByteVec serialize<UnlockRequest>(const UnlockRequest&);
template <> core::Result<UnlockRequest> deserialize<UnlockRequest>(core::ByteSpan);

template <> core::ByteVec serialize<LockRequest>(const LockRequest&);
template <> core::Result<LockRequest> deserialize<LockRequest>(core::ByteSpan);

template <> core::ByteVec serialize<EnableProgramPasswordRequest>(const EnableProgramPasswordRequest&);
template <> core::Result<EnableProgramPasswordRequest> deserialize<EnableProgramPasswordRequest>(core::ByteSpan);

template <> core::ByteVec serialize<DisableProgramPasswordRequest>(const DisableProgramPasswordRequest&);
template <> core::Result<DisableProgramPasswordRequest> deserialize<DisableProgramPasswordRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ChangeProgramPasswordRequest>(const ChangeProgramPasswordRequest&);
template <> core::Result<ChangeProgramPasswordRequest> deserialize<ChangeProgramPasswordRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GetVaultStatusRequest>(const GetVaultStatusRequest&);
template <> core::Result<GetVaultStatusRequest> deserialize<GetVaultStatusRequest>(core::ByteSpan);

template <> core::ByteVec serialize<AddEntryRequest>(const AddEntryRequest&);
template <> core::Result<AddEntryRequest> deserialize<AddEntryRequest>(core::ByteSpan);

template <> core::ByteVec serialize<UpdateEntryRequest>(const UpdateEntryRequest&);
template <> core::Result<UpdateEntryRequest> deserialize<UpdateEntryRequest>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveEntryRequest>(const RemoveEntryRequest&);
template <> core::Result<RemoveEntryRequest> deserialize<RemoveEntryRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GetEntryRequest>(const GetEntryRequest&);
template <> core::Result<GetEntryRequest> deserialize<GetEntryRequest>(core::ByteSpan);

template <> core::ByteVec serialize<SearchEntriesRequest>(const SearchEntriesRequest&);
template <> core::Result<SearchEntriesRequest> deserialize<SearchEntriesRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ListEntriesRequest>(const ListEntriesRequest&);
template <> core::Result<ListEntriesRequest> deserialize<ListEntriesRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GeneratePasswordRequest>(const GeneratePasswordRequest&);
template <> core::Result<GeneratePasswordRequest> deserialize<GeneratePasswordRequest>(core::ByteSpan);

template <> core::ByteVec serialize<EstimateStrengthRequest>(const EstimateStrengthRequest&);
template <> core::Result<EstimateStrengthRequest> deserialize<EstimateStrengthRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ListGeneratedRecordsRequest>(const ListGeneratedRecordsRequest&);
template <> core::Result<ListGeneratedRecordsRequest> deserialize<ListGeneratedRecordsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveGeneratedRecordRequest>(const RemoveGeneratedRecordRequest&);
template <> core::Result<RemoveGeneratedRecordRequest> deserialize<RemoveGeneratedRecordRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ClearGeneratedRecordsRequest>(const ClearGeneratedRecordsRequest&);
template <> core::Result<ClearGeneratedRecordsRequest> deserialize<ClearGeneratedRecordsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GetGeneratorSettingsRequest>(const GetGeneratorSettingsRequest&);
template <> core::Result<GetGeneratorSettingsRequest> deserialize<GetGeneratorSettingsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<SetGeneratorLimitRequest>(const SetGeneratorLimitRequest&);
template <> core::Result<SetGeneratorLimitRequest> deserialize<SetGeneratorLimitRequest>(core::ByteSpan);

// Tag 相关请求
template <> core::ByteVec serialize<AddTagRequest>(const AddTagRequest&);
template <> core::Result<AddTagRequest> deserialize<AddTagRequest>(core::ByteSpan);

template <> core::ByteVec serialize<UpdateTagRequest>(const UpdateTagRequest&);
template <> core::Result<UpdateTagRequest> deserialize<UpdateTagRequest>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveTagRequest>(const RemoveTagRequest&);
template <> core::Result<RemoveTagRequest> deserialize<RemoveTagRequest>(core::ByteSpan);

template <> core::ByteVec serialize<ListTagsRequest>(const ListTagsRequest&);
template <> core::Result<ListTagsRequest> deserialize<ListTagsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GetTagRequest>(const GetTagRequest&);
template <> core::Result<GetTagRequest> deserialize<GetTagRequest>(core::ByteSpan);

template <> core::ByteVec serialize<FindTagByNameRequest>(const FindTagByNameRequest&);
template <> core::Result<FindTagByNameRequest> deserialize<FindTagByNameRequest>(core::ByteSpan);

template <> core::ByteVec serialize<GetEntryTagsRequest>(const GetEntryTagsRequest&);
template <> core::Result<GetEntryTagsRequest> deserialize<GetEntryTagsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<SetEntryTagsRequest>(const SetEntryTagsRequest&);
template <> core::Result<SetEntryTagsRequest> deserialize<SetEntryTagsRequest>(core::ByteSpan);

template <> core::ByteVec serialize<core::GeneratedPasswordRecord>(const core::GeneratedPasswordRecord&);
template <> core::Result<core::GeneratedPasswordRecord> deserialize<core::GeneratedPasswordRecord>(core::ByteSpan);

// ---------------------------------------------------------------------------
// 响应消息特化声明
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<PingResponse>(const PingResponse&);
template <> core::Result<PingResponse> deserialize<PingResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ShutdownResponse>(const ShutdownResponse&);
template <> core::Result<ShutdownResponse> deserialize<ShutdownResponse>(core::ByteSpan);

template <> core::ByteVec serialize<UnlockResponse>(const UnlockResponse&);
template <> core::Result<UnlockResponse> deserialize<UnlockResponse>(core::ByteSpan);

template <> core::ByteVec serialize<LockResponse>(const LockResponse&);
template <> core::Result<LockResponse> deserialize<LockResponse>(core::ByteSpan);

template <> core::ByteVec serialize<EnableProgramPasswordResponse>(const EnableProgramPasswordResponse&);
template <> core::Result<EnableProgramPasswordResponse> deserialize<EnableProgramPasswordResponse>(core::ByteSpan);

template <> core::ByteVec serialize<DisableProgramPasswordResponse>(const DisableProgramPasswordResponse&);
template <> core::Result<DisableProgramPasswordResponse> deserialize<DisableProgramPasswordResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ChangeProgramPasswordResponse>(const ChangeProgramPasswordResponse&);
template <> core::Result<ChangeProgramPasswordResponse> deserialize<ChangeProgramPasswordResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GetVaultStatusResponse>(const GetVaultStatusResponse&);
template <> core::Result<GetVaultStatusResponse> deserialize<GetVaultStatusResponse>(core::ByteSpan);

template <> core::ByteVec serialize<AddEntryResponse>(const AddEntryResponse&);
template <> core::Result<AddEntryResponse> deserialize<AddEntryResponse>(core::ByteSpan);

template <> core::ByteVec serialize<UpdateEntryResponse>(const UpdateEntryResponse&);
template <> core::Result<UpdateEntryResponse> deserialize<UpdateEntryResponse>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveEntryResponse>(const RemoveEntryResponse&);
template <> core::Result<RemoveEntryResponse> deserialize<RemoveEntryResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GetEntryResponse>(const GetEntryResponse&);
template <> core::Result<GetEntryResponse> deserialize<GetEntryResponse>(core::ByteSpan);

template <> core::ByteVec serialize<SearchEntriesResponse>(const SearchEntriesResponse&);
template <> core::Result<SearchEntriesResponse> deserialize<SearchEntriesResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ListEntriesResponse>(const ListEntriesResponse&);
template <> core::Result<ListEntriesResponse> deserialize<ListEntriesResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GeneratePasswordResponse>(const GeneratePasswordResponse&);
template <> core::Result<GeneratePasswordResponse> deserialize<GeneratePasswordResponse>(core::ByteSpan);

template <> core::ByteVec serialize<EstimateStrengthResponse>(const EstimateStrengthResponse&);
template <> core::Result<EstimateStrengthResponse> deserialize<EstimateStrengthResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ListGeneratedRecordsResponse>(const ListGeneratedRecordsResponse&);
template <> core::Result<ListGeneratedRecordsResponse> deserialize<ListGeneratedRecordsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveGeneratedRecordResponse>(const RemoveGeneratedRecordResponse&);
template <> core::Result<RemoveGeneratedRecordResponse> deserialize<RemoveGeneratedRecordResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ClearGeneratedRecordsResponse>(const ClearGeneratedRecordsResponse&);
template <> core::Result<ClearGeneratedRecordsResponse> deserialize<ClearGeneratedRecordsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GetGeneratorSettingsResponse>(const GetGeneratorSettingsResponse&);
template <> core::Result<GetGeneratorSettingsResponse> deserialize<GetGeneratorSettingsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<SetGeneratorLimitResponse>(const SetGeneratorLimitResponse&);
template <> core::Result<SetGeneratorLimitResponse> deserialize<SetGeneratorLimitResponse>(core::ByteSpan);

// Tag 相关响应
template <> core::ByteVec serialize<AddTagResponse>(const AddTagResponse&);
template <> core::Result<AddTagResponse> deserialize<AddTagResponse>(core::ByteSpan);

template <> core::ByteVec serialize<UpdateTagResponse>(const UpdateTagResponse&);
template <> core::Result<UpdateTagResponse> deserialize<UpdateTagResponse>(core::ByteSpan);

template <> core::ByteVec serialize<RemoveTagResponse>(const RemoveTagResponse&);
template <> core::Result<RemoveTagResponse> deserialize<RemoveTagResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ListTagsResponse>(const ListTagsResponse&);
template <> core::Result<ListTagsResponse> deserialize<ListTagsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GetTagResponse>(const GetTagResponse&);
template <> core::Result<GetTagResponse> deserialize<GetTagResponse>(core::ByteSpan);

template <> core::ByteVec serialize<FindTagByNameResponse>(const FindTagByNameResponse&);
template <> core::Result<FindTagByNameResponse> deserialize<FindTagByNameResponse>(core::ByteSpan);

template <> core::ByteVec serialize<GetEntryTagsResponse>(const GetEntryTagsResponse&);
template <> core::Result<GetEntryTagsResponse> deserialize<GetEntryTagsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<SetEntryTagsResponse>(const SetEntryTagsResponse&);
template <> core::Result<SetEntryTagsResponse> deserialize<SetEntryTagsResponse>(core::ByteSpan);

template <> core::ByteVec serialize<ErrorResponse>(const ErrorResponse&);
template <> core::Result<ErrorResponse> deserialize<ErrorResponse>(core::ByteSpan);

// ---------------------------------------------------------------------------
// 消息帧封包/解包
// ---------------------------------------------------------------------------

/// 构造完整 IPC 消息帧：MessageHeader(16) + payload。
/// \param cmd 命令 ID
/// \param request_id 请求 ID（用于异步匹配请求与响应）
/// \param payload 已序列化的负载字节（可为空）
/// \return 完整帧的字节向量
core::ByteVec pack_message(CommandId cmd, uint32_t request_id, core::ByteSpan payload);

/// 从字节流解析消息头。
/// \param data 至少包含 16 字节的输入
/// \return 成功时返回 {header, payload_offset}（payload_offset 恒为 16）；
///         数据不足或 magic 不匹配时返回 IpcError。
core::Result<std::pair<MessageHeader, size_t>> parse_header(core::ByteSpan data);

}  // namespace yuli::vault::protocol
