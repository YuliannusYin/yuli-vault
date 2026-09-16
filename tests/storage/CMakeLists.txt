# coding: utf-8
# =============================================================================
# tests/storage/CMakeLists.txt
#
# 存储引擎单元测试可执行目标。链接 YuliVault::Storage（含 SQLite 与内存实现）
# 与 GoogleTest。注意：tests/CMakeLists.txt 由主流程统一聚合，
# 本文件假定已被父级 tests/CMakeLists.txt 通过 add_subdirectory 引入。
# =============================================================================

add_executable(test_storage_engine test_storage_engine.cpp)

target_link_libraries(test_storage_engine
    PRIVATE
        YuliVault::Storage
        SQLite3::SQLite3
        GTest::gtest_main
)

# 集成到 CTest
add_test(NAME test_storage_engine COMMAND test_storage_engine)

message(STATUS "Yuli Vault: tests/storage/ configured")
