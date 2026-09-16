# coding: utf-8
# =============================================================================
# CompilerWarnings.cmake
#
# 定义项目通用警告级别，提供一个 INTERFACE 目标 YuliVault::Warnings，
# 各模块通过 target_link_libraries(<target> PRIVATE YuliVault::Warnings) 引入。
#
# 风格说明：
#   - GCC/Clang: -Wall -Wextra -Wpedantic，并禁用少量噪音警告
#   - MSVC     : /W4，并显式关闭若干无意义的 C4xxx 警告
# =============================================================================

# 创建 INTERFACE 库作为警告选项的载体（modern CMake 风格）
add_library(yuli_vault_warnings INTERFACE)
add_library(YuliVault::Warnings ALIAS yuli_vault_warnings)

target_compile_features(yuli_vault_warnings INTERFACE cxx_std_17)

# -----------------------------------------------------------------------------
# GCC / Clang 警告配置
# -----------------------------------------------------------------------------
set(_gcc_clang_warnings
    -Wall                       # 启用大部分常见警告
    -Wextra                    # 启用 -Wall 未启用的额外警告
    -Wpedantic                 # 严格遵循 ISO C++ 标准
    -Wshadow                   # 变量遮蔽警告
    -Wnon-virtual-dtor         # 抽象类非虚析构警告
    -Woverloaded-virtual       # 虚函数重载隐藏警告
    -Wconversion               # 隐式类型转换可能丢失精度
    -Wsign-conversion          # 有符号/无符号转换
    -Wold-style-cast           # C 风格转换
    -Wnull-dereference         # 空指针解引用
    -Wdouble-promotion         # float 隐式提升为 double
    -Wformat=2                 # printf/scanf 格式串检查
)

# 禁用噪音警告（GCC/Clang）
set(_gcc_clang_disabled
    -Wno-unused-parameter      # 接口未使用参数很常见，不算错误
    -Wno-missing-field-initializers  # 部分字段使用默认值很常见
)

# -----------------------------------------------------------------------------
# MSVC 警告配置：/W4 + 关闭若干噪音警告
# -----------------------------------------------------------------------------
set(_msvc_warnings
    /W4                         # 最高警告级别（不含 /Wall，那太吵）
    /permissive-                # 严格标准一致性
    /Zc:__cplusplus             # 使 __cplusplus 宏报告正确版本
    /utf-8                      # 源码与执行字符集均为 UTF-8
    /EHsc                       # C++ 异常处理
)

set(_msvc_disabled
    /wd4127                     # 条件表达式是常量（如 assert 宏）
    /wd4458                     # 类成员遮蔽全局名
    /wd4459                     # 全局声明遮蔽
    /wd4505                     # 未引用的本地函数被移除
    /wd4514                     # 未引用的内联函数被移除
    /wd4623                     # 默认构造被隐式删除
    /wd4625                     # 复制构造被隐式删除
    /wd4626                     # 赋值运算符被隐式删除
    /wd4365                     # signed/unsigned 转换（与 -Wsign-conversion 重复）
    /wd4668                     # 未定义的预处理器宏出现在条件中
    /wd4820                     # 结构填充字节
    /wd5026                     # 移动构造被隐式删除
    /wd5027                     # 移动赋值被隐式删除
)

# 选择性应用
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    target_compile_options(yuli_vault_warnings INTERFACE ${_msvc_warnings} ${_msvc_disabled})
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(yuli_vault_warnings INTERFACE ${_gcc_clang_warnings} ${_gcc_clang_disabled})
else()
    message(STATUS "CompilerWarnings: 未识别的编译器 '${CMAKE_CXX_COMPILER_ID}'，跳过警告配置")
endif()

# 提供便利函数：将警告应用到目标（旧式 API，等价于 link YuliVault::Warnings）
function(yuli_vault_enable_warnings target)
    if(TARGET YuliVault::Warnings)
        target_link_libraries(${target} PRIVATE YuliVault::Warnings)
    endif()
endfunction()
