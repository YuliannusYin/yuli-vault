# coding: utf-8
# =============================================================================
# src/sdk/protocol/CMakeLists.txt
#
# PwdVault IPC 协议层。定义 yuli-vault-protocol STATIC 库，提供：
#   - Commands.h     命令枚举 CommandId 与 command_name()
#   - Messages.h     消息头 MessageHeader 与各 Request/Response 结构
#   - Serializer.h/.cpp 二进制序列化/反序列化与消息帧封包/解包
#
# UI 与 service 进程均链接此库以共享协议定义。新增命令只需在 Commands.h
# 追加枚举值、在 Messages.h 追加 Request/Response 结构、在 Serializer 中
# 追加 serialize/deserialize 特化即可。
# =============================================================================

add_library(yuli-vault-protocol STATIC
    Serializer.cpp
)

target_include_directories(yuli-vault-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yuli-vault-protocol
    PUBLIC
        yuli-vault-sdk-core
        YuliVault::Warnings
)

# 强制 C++20（与项目顶层一致，因依赖 std::span / core::ByteSpan）
target_compile_features(yuli-vault-protocol PUBLIC cxx_std_20)

add_library(YuliVault::Protocol ALIAS yuli-vault-protocol)

message(STATUS "Yuli Vault: src/sdk/protocol/ configured")
