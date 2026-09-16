# coding: utf-8
# =============================================================================
# tests/generator/CMakeLists.txt
#
# PasswordGenerator 单元测试目标。
# =============================================================================

add_executable(test_password_generator
    test_password_generator.cpp
)

target_link_libraries(test_password_generator
    PRIVATE
        YuliVault::Generator
        GTest::gtest_main
)

# 集成到 ctest
add_test(NAME test_password_generator COMMAND test_password_generator)
