# coding: utf-8
# =============================================================================
# tests/integration/CMakeLists.txt
#
# PwdVault 端到端集成测试。直接实例化 ServiceCore + InMemoryStorageEngine +
# CryptoEngine + PasswordGenerator，跳过命名管道 I/O，通过
# ServiceCore::handle_request 直接调用业务逻辑。
#
# 实现要点：
#   - service 模块以 yuli-vault-service 可执行目标形式存在，不是独立库，
#     测试需直接编译 ServiceCore.cpp 与 ProgramPasswordStore.cpp 这两个源文件。
#   - ServiceCore.cpp 与 ProgramPasswordStore.cpp 仅依赖 SDK 头文件与 libsodium
#     （sodium_memzero / randombytes_buf），不依赖 main.cpp 中的工具函数，
#     可安全直接加入测试目标源文件列表。
#   - YuliVault::Sdk 聚合 core/crypto/storage/generator/protocol 五个子模块，
#     链接它即可获得全部 SDK 头文件路径与实现库（含 OpenSSL、SQLite、bcrypt）。
#   - 额外直接链接 unofficial-sodium::sodium：ServiceCore.cpp 与 ProgramPasswordStore.cpp
#     中调用 sodium_memzero / randombytes_buf，而 yuli-vault-crypto 以 PRIVATE 方式
#     链接 libsodium，不会传递给下游目标（与 src/service/CMakeLists.txt 一致）。
# =============================================================================

add_executable(test_e2e_flow
    test_e2e_flow.cpp
    test_migration.cpp
)

# 直接编译 service 模块的实现源文件（不修改 src/service/CMakeLists.txt）
target_sources(test_e2e_flow PRIVATE
    ${CMAKE_SOURCE_DIR}/src/service/ServiceCore.cpp
    ${CMAKE_SOURCE_DIR}/src/service/ProgramPasswordStore.cpp
)

# service 头文件（ServiceCore.h / ProgramPasswordStore.h）所在目录
target_include_directories(test_e2e_flow PRIVATE
    ${CMAKE_SOURCE_DIR}/src/service
)

target_link_libraries(test_e2e_flow
    PRIVATE
        YuliVault::Sdk               # 聚合 core/crypto/storage/generator/protocol
        YuliVault::Warnings
        unofficial-sodium::sodium    # ServiceCore/ProgramPasswordStore 直接调用 sodium_memzero
        SQLite3::SQLite3
        GTest::gtest_main
)

target_compile_features(test_e2e_flow PRIVATE cxx_std_20)

# 集成到 CTest
add_test(NAME test_e2e_flow COMMAND test_e2e_flow)

message(STATUS "Yuli Vault: tests/integration/ configured")
