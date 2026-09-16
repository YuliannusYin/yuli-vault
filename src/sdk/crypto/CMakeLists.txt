# coding: utf-8
# =============================================================================
# src/sdk/crypto/CMakeLists.txt
#
# PwdVault 加密引擎模块。定义 yuli-vault-crypto STATIC 库，提供：
#   - CryptoEngine.h / CryptoEngine.cpp
#
# 依赖：
#   PUBLIC  : yuli-vault-sdk-core（头文件：ICryptoEngine/Types/Error/Result）
#   PRIVATE : OpenSSL::Crypto（AES-256-GCM via EVP）
#             unofficial-sodium::sodium（Argon2id + memzero + randombytes）
# =============================================================================

add_library(yuli-vault-crypto STATIC
    CryptoEngine.cpp
)
add_library(YuliVault::Crypto ALIAS yuli-vault-crypto)

target_include_directories(yuli-vault-crypto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yuli-vault-crypto
    PUBLIC
        yuli-vault-sdk-core           # 头文件依赖（ICryptoEngine、Types、Error、Result）
        YuliVault::Warnings
    PRIVATE
        OpenSSL::Crypto
        unofficial-sodium::sodium   # vcpkg 中 libsodium 的 target 名
)

# 强制 C++20（与 core 一致，Types.h 使用了 std::span）
target_compile_features(yuli-vault-crypto PUBLIC cxx_std_20)

message(STATUS "Yuli Vault: src/sdk/crypto/ configured")
