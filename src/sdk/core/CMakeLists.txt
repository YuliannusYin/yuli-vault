# coding: utf-8
# =============================================================================
# src/sdk/core/CMakeLists.txt
#
# PwdVault SDK 核心模块（header-only）。定义 yuli-vault-sdk-core INTERFACE 库，
# 提供：
#   - Types.h          核心数据类型与公共别名（ByteVec/ByteSpan/PasswordEntry 等）
#   - Error.h          通用错误类型（ErrorCode + Error）
#   - Result.h         通用结果模板 Result<T>
#   - IStorageEngine.h 存储引擎抽象接口
#   - ICryptoEngine.h  加密引擎抽象接口
#   - IPasswordGenerator.h 密码生成器抽象接口
#
# 由于仅含头文件，使用 INTERFACE 库；具体引擎实现将在 crypto/storage/generator
# 子目录中以 STATIC 库形式提供（后续 Task 4/5/6）。
# =============================================================================

add_library(yuli-vault-sdk-core INTERFACE)
add_library(YuliVault::SdkCore ALIAS yuli-vault-sdk-core)

target_include_directories(yuli-vault-sdk-core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 强制 C++20（std::span 在 Types.h 中使用）
target_compile_features(yuli-vault-sdk-core INTERFACE cxx_std_20)

# 链接项目通用警告配置（INTERFACE target，由顶层 cmake/CompilerWarnings.cmake 定义）
target_link_libraries(yuli-vault-sdk-core INTERFACE YuliVault::Warnings)

message(STATUS "Yuli Vault: src/sdk/core/ configured (INTERFACE)")
