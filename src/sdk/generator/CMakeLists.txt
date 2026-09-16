# coding: utf-8
# =============================================================================
# src/sdk/generator/CMakeLists.txt
#
# PwdVault 密码生成引擎。定义 yuli-vault-generator STATIC 库，实现
# IPasswordGenerator 抽象接口：
#   - PasswordGenerator.h/.cpp  密码生成与强度估算
#
# RNG 选用 Windows 原生 BCryptGenRandom（密码学安全），链接系统库 bcrypt。
# =============================================================================

add_library(yuli-vault-generator STATIC
    PasswordGenerator.cpp
)

target_include_directories(yuli-vault-generator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yuli-vault-generator
    PUBLIC
        yuli-vault-sdk-core
        YuliVault::Warnings
    PRIVATE
        bcrypt   # Windows 系统库（CNG: BCryptGenRandom）
)

add_library(YuliVault::Generator ALIAS yuli-vault-generator)

message(STATUS "Yuli Vault: src/sdk/generator/ configured")
