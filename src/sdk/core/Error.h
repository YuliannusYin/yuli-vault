// coding: utf-8
// =============================================================================
// Error.h
//
// PwdVault SDK 通用错误类型。所有引擎接口与协议层均使用 `Error` 描述失败原因。
// 与 Result.h 配合使用，避免异常跨进程边界（IPC 通道）传递。
// =============================================================================
#pragma once

#include <string>
#include <string_view>

namespace yuli::vault::core {

/// 错误类别枚举。
enum class ErrorCode {
    None,             ///< 无错误（成功）
    InvalidArgument,  ///< 参数非法
    NotFound,         ///< 条目不存在
    AlreadyExists,    ///< 条目已存在（主键/唯一约束冲突）
    Unauthorized,     ///< 未授权（主密码错误、密钥不匹配等）
    CryptoError,      ///< 加密/解密/密钥派生失败
    StorageError,     ///< 存储引擎错误（数据库 IO、约束失败等）
    IpcError,         ///< 进程间通信错误
    InternalError     ///< 其他内部错误
};

/// 错误对象，由错误码与人类可读消息组成。
class Error {
public:
    /// 构造一个无错误（None）对象。
    Error() = default;

    /// 构造一个指定错误码与消息的对象。
    /// \param c 错误码
    /// \param msg 人类可读的错误描述
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    /// 是否成功（错误码为 None）。
    bool ok() const { return code == ErrorCode::None; }

    /// bool 转换：与 ok() 等价（成功返回 true）。
    explicit operator bool() const { return ok(); }

    /// 返回格式化的错误描述："code: message"。
    /// 若无错误返回空字符串。
    std::string what() const {
        if (code == ErrorCode::None) {
            return std::string{};
        }
        return std::string{code_name()} + ": " + message;
    }

    /// 错误码的字符串名称（便于日志输出）。
    std::string_view code_name() const {
        switch (code) {
            case ErrorCode::None:             return "None";
            case ErrorCode::InvalidArgument:  return "InvalidArgument";
            case ErrorCode::NotFound:         return "NotFound";
            case ErrorCode::AlreadyExists:    return "AlreadyExists";
            case ErrorCode::Unauthorized:     return "Unauthorized";
            case ErrorCode::CryptoError:      return "CryptoError";
            case ErrorCode::StorageError:     return "StorageError";
            case ErrorCode::IpcError:         return "IpcError";
            case ErrorCode::InternalError:    return "InternalError";
        }
        return "Unknown";
    }

    ErrorCode code = ErrorCode::None;  ///< 错误码
    std::string message;                 ///< 人类可读错误消息
};

}  // namespace yuli::vault::core
