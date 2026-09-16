# coding: utf-8
# =============================================================================
# tests/crypto/CMakeLists.txt
#
# PwdVault 加密引擎单元测试。链接 YuliVault::Crypto 与 GoogleTest。
# =============================================================================

add_executable(test_crypto_engine test_crypto_engine.cpp)

target_link_libraries(test_crypto_engine
    PRIVATE
        YuliVault::Crypto
        GTest::gtest_main
)

# 注册到 CTest（顶层 CMakeLists.txt 已 enable_testing()）
add_test(NAME test_crypto_engine COMMAND test_crypto_engine)
