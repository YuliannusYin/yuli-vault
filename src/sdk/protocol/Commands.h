// coding: utf-8
// =============================================================================
// Commands.h
//
// PwdVault IPC 协议命令枚举。UI 进程与 service 进程通过命名管道通信，
// 每条消息携带一个 CommandId 标识其语义。新增命令只需在此处追加枚举值
// （保持值唯一且不删除/复用旧值，以维持协议向前兼容性）。
//
// 命令 ID 命名空间分组（高字节）：
//   0x00xx  系统级（心跳、关闭）
//   0x01xx  会话级（解锁、锁定、程序密码管理、vault 状态查询）
//   0x02xx  条目 CRUD
//   0x03xx  密码生成与强度评估
//   0x04xx  标签（Tag）管理与条目-标签关联
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace yuli::vault::protocol {

/// IPC 命令枚举。值固定为 uint16_t，便于在消息头中直接序列化。
enum class CommandId : uint16_t {
    Ping             = 0x0001,  ///< 心跳检测（UI 周期性 ping service）
    Shutdown         = 0x0002,  ///< UI 通知 service 优雅退出

    Unlock           = 0x0101,  ///< 已锁定状态下解锁（验证程序密码）
    Lock             = 0x0102,  ///< 主动锁定，清除内存中的加密密钥
    EnableProgramPassword  = 0x0103,  ///< 启用程序密码（明文库→加密库）
    DisableProgramPassword = 0x0104,  ///< 禁用程序密码（加密库→明文库）
    ChangeProgramPassword  = 0x0105,  ///< 修改程序密码（重新包装加密密钥）
    GetVaultStatus         = 0x0106,  ///< 查询 vault 状态（密码是否启用/是否锁定）

    AddEntry         = 0x0200,  ///< 新增密码条目
    UpdateEntry      = 0x0201,  ///< 更新已存在条目
    RemoveEntry      = 0x0202,  ///< 按 id 删除条目
    GetEntry         = 0x0203,  ///< 按 id 获取单条条目
    SearchEntries    = 0x0204,  ///< 按 SearchQuery 搜索
    ListEntries      = 0x0205,  ///< 列出全部条目

    GeneratePassword = 0x0300,  ///< 按选项生成密码
    EstimateStrength = 0x0301,  ///< 评估给定密码的强度（熵 bit 数）
    ListGeneratedRecords  = 0x0302,  ///< 列出生成器历史记录
    RemoveGeneratedRecord = 0x0303,  ///< 按 id 删除单条生成记录
    ClearGeneratedRecords = 0x0304,  ///< 清空全部生成记录
    GetGeneratorSettings  = 0x0305,  ///< 查询生成器设置（历史记录上限）
    SetGeneratorLimit     = 0x0306,  ///< 设置历史记录上限（0=无限制）

    AddTag           = 0x0400,  ///< 新增标签
    UpdateTag        = 0x0401,  ///< 更新已存在标签
    RemoveTag        = 0x0402,  ///< 按 id 删除标签
    ListTags         = 0x0403,  ///< 列出全部标签
    GetTag           = 0x0404,  ///< 按 id 获取单条标签
    FindTagByName    = 0x0405,  ///< 按 name 查找标签
    GetEntryTags     = 0x0406,  ///< 获取指定条目的全部标签
    SetEntryTags     = 0x0407,  ///< 全量替换指定条目的标签关联
};

/// 返回命令的可读名称，便于日志输出与调试。
/// 未知命令返回 "Unknown"。
std::string_view command_name(CommandId cmd) noexcept;

}  // namespace yuli::vault::protocol
