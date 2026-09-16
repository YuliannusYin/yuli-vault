# coding: utf-8
# =============================================================================
# src/service/CMakeLists.txt
#
# PwdVault 服务进程（yuli-vault-service.exe）。
#   - main.cpp                  进程入口：参数解析、引擎初始化、保活循环、信号处理
#   - IpcServer.cpp             命名管道服务端（OVERLAPPED I/O，多客户端并发）
#   - ServiceCore.cpp           业务调度核心（命令分发、解锁/锁定、entry 加解密）
#   - ProgramPasswordStore.cpp  程序密码与 KEK 持久化（vault.meta）
#
# 子系统：控制台（CONSOLE），不使用 WIN32，便于调试时 stdout 可见。
# 链接 YuliVault::Sdk（聚合 core/crypto/storage/generator/protocol）。
# 额外直接链接 libsodium：ServiceCore.cpp 与 ProgramPasswordStore.cpp 直接调用
# sodium_memzero / randombytes_buf，而 yuli-vault-crypto 以 PRIVATE 方式链接
# libsodium，不会传递给下游目标。
# =============================================================================

add_executable(yuli-vault-service
    main.cpp
    IpcServer.cpp
    ServiceCore.cpp
    ProgramPasswordStore.cpp
    AppDataDir.h
)

target_link_libraries(yuli-vault-service
    PRIVATE
        YuliVault::Sdk
        YuliVault::Warnings
        unofficial-sodium::sodium
)

target_compile_features(yuli-vault-service PRIVATE cxx_std_20)

set_target_properties(yuli-vault-service PROPERTIES
    OUTPUT_NAME "yuli-vault-service"
)

# 安装规则
install(TARGETS yuli-vault-service RUNTIME DESTINATION bin)

message(STATUS "Yuli Vault: src/service/ configured (yuli-vault-service)")
