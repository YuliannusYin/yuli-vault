# coding: utf-8
# =============================================================================
# tests/ui/CMakeLists.txt
#
# PwdVault UI 工具层单元测试。当前覆盖 MarkdownUtil 的转义与链接协议校验。
#
# 实现要点：
#   - yuli-vault-ui 是 WIN32 可执行目标（含 main.cpp、Qt Widgets 资源等），
#     不适合直接链接到测试，因此采用与 tests/integration 相同的策略：
#     直接把 MarkdownUtil.cpp 编译进测试目标。
#   - MarkdownUtil 仅依赖 Qt6::Core（QString / QRegularExpression / QStringList），
#     不依赖 Widgets，故只需链接 Qt6::Core。
# =============================================================================
add_executable(test_markdown_util
    test_markdown_util.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MarkdownUtil.cpp
)

# MarkdownUtil.h 所在目录
target_include_directories(test_markdown_util PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ui
)

target_link_libraries(test_markdown_util
    PRIVATE
        Qt6::Core
        YuliVault::Warnings
        GTest::gtest_main
)

target_compile_features(test_markdown_util PRIVATE cxx_std_20)

# 集成到 CTest
add_test(NAME test_markdown_util COMMAND test_markdown_util)

message(STATUS "Yuli Vault: tests/ui/ configured")
