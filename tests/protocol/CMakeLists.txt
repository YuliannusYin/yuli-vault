# coding: utf-8
# =============================================================================
# tests/protocol/CMakeLists.txt
#
# PwdVault IPC 协议层单元测试。链接 YuliVault::Protocol 与 GoogleTest。
# =============================================================================

add_executable(test_protocol test_protocol.cpp)

target_link_libraries(test_protocol
    PRIVATE
        YuliVault::Protocol
        GTest::gtest_main
)

# 注册到 CTest（顶层 CMakeLists.txt 已 enable_testing()）
add_test(NAME test_protocol COMMAND test_protocol)
