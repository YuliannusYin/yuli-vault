// coding: utf-8
// =============================================================================
// Result.h
//
// PwdVault SDK 通用结果类型 `Result<T>`，封装「值或错误」的二元返回。
//
// 设计动机：
//   - 跨进程 IPC 通道不能传递 C++ 异常，所有引擎接口统一返回 Result。
//   - 强制调用方显式处理错误（vs. 异常的隐式传播）。
//   - 类似 Rust 的 `Result<T, E>` 或 std::expected（C++23）的精简版。
//
// 注意：成员函数命名为 `Ok`/`Err`（非全大写 `OK`/`ERROR`），避免与
//       Windows SDK 的 `OK`/`ERROR` 宏冲突。为防止其他翻译单元在包含
//       本头之前意外 `#define OK`/`#define ERROR`，此处做防御性 #undef。
// =============================================================================
#pragma once

#include <optional>
#include <utility>

#include "Error.h"

// 防御性取消可能存在的 Windows 宏，避免污染本头的命名。
// 见 https://learn.microsoft.com/windows/win32/winprog/using-the-windows-headers
#ifdef OK
#undef OK
#endif
#ifdef ERROR
#undef ERROR
#endif

namespace yuli::vault::core {

/// 通用结果模板：要么持有值 `T`，要么持有 `Error`。
///
/// 用法示例：
/// \code
///   Result<int> r = Result<int>::Ok(42);
///   if (r) {
///       use(r.value());
///   } else {
///       log(r.error().what());
///   }
/// \endcode
template <typename T>
class Result {
public:
    /// 构造一个成功结果，持有值 \p v。
    static Result Ok(T v) {
        Result r;
        r.value_.emplace(std::move(v));
        r.error_ = Error{};
        return r;
    }

    /// 构造一个失败结果，持有错误 \p e（不持有值）。
    static Result Err(Error e) {
        Result r;
        r.error_ = std::move(e);
        return r;
    }

    /// 是否成功（无错误）。
    bool ok() const { return error_.ok(); }

    /// bool 转换：与 ok() 等价。
    explicit operator bool() const { return ok(); }

    /// 访问持有的值（const 左值引用）。调用前必须确保 ok() 为 true。
    const T& value() const& { return *value_; }

    /// 访问持有的值（左值引用）。调用前必须确保 ok() 为 true。
    T& value() & { return *value_; }

    /// 访问持有的值（右值引用，可移动）。调用前必须确保 ok() 为 true。
    T&& value() && { return std::move(*value_); }

    /// 箭头操作符，便于 `r->method()` 形式访问成员。
    const T* operator->() const { return &(*value_); }
    T* operator->() { return &(*value_); }

    /// 解引用，便于 `*r` 形式访问。
    const T& operator*() const& { return *value_; }
    T& operator*() & { return *value_; }
    T&& operator*() && { return std::move(*value_); }

    /// 访问错误对象。失败时返回具体错误；成功时返回 None 错误。
    const Error& error() const& { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

/// `Result<void>` 特化：仅承载成功/失败状态，不持有值。
template <>
class Result<void> {
public:
    /// 构造一个成功结果。
    static Result Ok() {
        Result r;
        r.error_ = Error{};
        return r;
    }

    /// 构造一个失败结果，持有错误 \p e。
    static Result Err(Error e) {
        Result r;
        r.error_ = std::move(e);
        return r;
    }

    /// 是否成功（无错误）。
    bool ok() const { return error_.ok(); }

    /// bool 转换：与 ok() 等价。
    explicit operator bool() const { return ok(); }

    /// 访问错误对象。失败时返回具体错误；成功时返回 None 错误。
    const Error& error() const& { return error_; }

private:
    Error error_;
};

}  // namespace yuli::vault::core
