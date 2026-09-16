// coding: utf-8
// =============================================================================
// PasswordGenerator.h
//
// PwdVault 密码生成引擎的具体实现。基于 IPasswordGenerator 抽象接口，
// 由服务进程统一调用，UI 不直接持有本类实例（避免暴露算法状态）。
//
// 设计要点：
//   - 无状态：每次 generate() 调用相互独立
//   - 密码学安全 RNG：Windows 平台使用 BCryptGenRandom
//   - 消除 modulo bias：rejection sampling
//   - 保证字符集覆盖：每种已启用字符集至少出现一次（长度允许时）
// =============================================================================
#pragma once

#include "IPasswordGenerator.h"

namespace yuli::vault::generator {

/// 密码生成器具体实现。
///
/// 无状态对象，可被多线程共享调用（BCryptGenRandom 本身线程安全）。
class PasswordGenerator : public core::IPasswordGenerator {
public:
    PasswordGenerator() = default;
    ~PasswordGenerator() override = default;

    /// 根据选项生成密码。
    /// \param options 生成选项（长度、字符集、是否排除易混字符等）
    /// \return 成功时返回生成的密码字符串；失败时返回具体错误
    core::Result<std::string> generate(const core::PasswordGeneratorOptions& options) override;

    /// 估算密码强度（纯熵 + 模式惩罚）。
    /// \param password 待评估的密码
    /// \return StrengthEstimate，含 bits、level、score、warnings；空密码返回全 0
    core::StrengthEstimate estimate_strength(const std::string& password) override;
};

}  // namespace yuli::vault::generator
