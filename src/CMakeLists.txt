# coding: utf-8
# =============================================================================
# src/CMakeLists.txt
#
# PwdVault 源代码子目录入口。组织三个一级模块：
#   - sdk/      共享 SDK（UI 与服务都链接）
#   - service/  服务进程（yuli-vault-service.exe）
#   - ui/       UI 进程（yuli-vault-ui.exe）
#
# 当 Task 3+ 实现具体源码时，各子目录会定义实际的 target；目前仅占位，
# 让顶层 `cmake -B build -S .` 能通过 configure（前提是 Qt 6、vcpkg 依赖已就绪）。
# =============================================================================

message(STATUS "Yuli Vault: src/ configured")

# 各子模块按依赖顺序加入：sdk → service / ui
add_subdirectory(sdk)
add_subdirectory(service)
add_subdirectory(ui)
