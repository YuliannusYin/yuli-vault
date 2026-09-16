# coding: utf-8
# =============================================================================
# src/sdk/CMakeLists.txt
#
# PwdVault SDK 聚合入口。包含以下子模块：
#   - core/      核心类型与抽象接口（header-only）
#   - crypto/    加密引擎实现（AES-256-GCM + Argon2id）
#   - storage/   存储引擎实现（SQLite 持久化 + 内存实现用于测试）
#   - generator/ 密码生成器实现
#   - protocol/  IPC 协议层（UI 进程 ↔ 服务进程）— Task 7 加入
#
# 聚合 target 为 `yuli-vault-sdk`（别名 `YuliVault::Sdk`），链接所有子模块。
# =============================================================================

message(STATUS "Yuli Vault: src/sdk/ configured")

add_subdirectory(core)
add_subdirectory(crypto)
add_subdirectory(storage)
add_subdirectory(generator)
add_subdirectory(protocol)

# 聚合 SDK target：链接所有子模块，供 UI 与 service 进程统一引用
add_library(yuli-vault-sdk INTERFACE)
target_link_libraries(yuli-vault-sdk INTERFACE
    yuli-vault-sdk-core
    yuli-vault-crypto
    yuli-vault-storage
    yuli-vault-generator
    yuli-vault-protocol
)
add_library(YuliVault::Sdk ALIAS yuli-vault-sdk)
