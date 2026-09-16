// coding: utf-8
// =============================================================================
// IPasswordGenerator.h
//
// 密码生成器抽象接口。UI 与服务进程均可通过此接口生成密码并估算强度。
// =============================================================================
#pragma once

#include <string>

#include "Error.h"
#include "Result.h"
#include "Types.h"

namespace yuli::vault::core {

/// 密码生成器抽象接口。
class IPasswordGenerator {
public:
    /// 析构函数为纯虚：本类是纯接口，无默认实现。
    /// 子类必须提供具体析构定义。
    virtual ~IPasswordGenerator() = 0;

    /// 根据选项生成密码。
    /// \param options 生成选项（长度、字符集等）
    /// \return 成功时返回生成的密码字符串
    virtual Result<std::string> generate(const PasswordGeneratorOptions& options) = 0;

    /// 估算密码强度。
    ///
    /// 评估流程：
    ///   1. 基于字符集种类的纯熵估算（length × log2(pool_size)）
    ///   2. 模式惩罚（重复字符 / 顺序序列 / 键盘序列 / 分布不均）
    ///   3. 按惩罚后的 bits 计算等级（VeryWeak..VeryStrong）与 score（0..4）
    ///
    /// \param password 待评估的密码
    /// \return StrengthEstimate，含 bits、level、score、warnings；
    ///         空密码返回全 0 值且无 warnings
    virtual StrengthEstimate estimate_strength(const std::string& password) = 0;
};

// 纯虚析构函数的定义：链接时需要（C++ 标准要求纯虚析构函数有定义）。
inline IPasswordGenerator::~IPasswordGenerator() = default;

}  // namespace yuli::vault::core
