# coding: utf-8
# =============================================================================
# src/sdk/storage/CMakeLists.txt
#
# PwdVault 存储引擎模块。定义 yuli-vault-storage STATIC 库，提供：
#   - StorageEngine.h/.cpp         SQLite3 持久化实现（生产）
#   - InMemoryStorageEngine.h/.cpp 纯内存实现（测试 / 缓存）
#
# 通过抽象接口 IStorageEngine 被服务进程调用；具体实现可替换。
# 依赖：SQLite3（PRIVATE，仅 .cpp 内部使用，不外泄到头文件）。
# =============================================================================

add_library(yuli-vault-storage STATIC
    StorageEngine.cpp
    InMemoryStorageEngine.cpp
)

target_include_directories(yuli-vault-storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(yuli-vault-storage
    PUBLIC
        yuli-vault-sdk-core
        YuliVault::Warnings
    PRIVATE
        SQLite3::SQLite3
)

add_library(YuliVault::Storage ALIAS yuli-vault-storage)

message(STATUS "Yuli Vault: src/sdk/storage/ configured")
