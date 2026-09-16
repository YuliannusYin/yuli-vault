# coding: utf-8
# =============================================================================
# Yuli Vault - top-level CMake build
#
# 密码管理器项目（C++20 + Qt 6），模仿火绒安全软件的工程化架构：
#   - UI 进程（yuli-vault-ui.exe，Qt Widgets）
#   - 服务进程（yuli-vault-service.exe，后台逻辑）
#   - SDK 模块（core/storage/crypto/generator/protocol）
#   - 单元测试（GoogleTest）
#
# 依赖通过 vcpkg manifest mode 提供：OpenSSL、libsodium、SQLite3、GoogleTest
# 依赖通过 Qt 官方安装器或 Qt SDK 提供：Qt 6 (Widgets、Network、Concurrent)
#
# 推荐配置命令（PowerShell，需提前设置 VCPKG_ROOT 与 CMAKE_PREFIX_PATH）：
#   $env:VCPKG_ROOT = "<vcpkg 路径>"
#   $env:CMAKE_PREFIX_PATH = "<Qt 6 安装路径>\msvc2019_64"  # 或 msvc2022_64
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
# =============================================================================

cmake_minimum_required(VERSION 3.20)

project(YuliVault
    VERSION 4.0.0
    DESCRIPTION "Yuli Vault - local encrypted e-vault"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# 1. 默认构建类型为 Release（除非用户显式指定）
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "Yuli Vault: defaulting CMAKE_BUILD_TYPE to Release")
endif()

# 提供多配置生成器（如 Visual Studio）可选配置类型
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel" CACHE STRING "" FORCE)
endif()

# -----------------------------------------------------------------------------
# 2. 全局 C++ 标准设置（C++20）
#    使用 C++20 是因为 SDK 核心类型（Types.h）使用了 std::span。
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认隐藏符号，仅在目标上显式导出（modern CMake 风格）
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# -----------------------------------------------------------------------------
# 3. 输出目录配置
#   - 可执行文件 -> ${CMAKE_BINARY_DIR}/bin
#   - 库文件     -> ${CMAKE_BINARY_DIR}/lib
#   - 在多配置生成器下按配置类型分子目录
# -----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 多配置生成器（Visual Studio / Ninja Multi-Config）按配置类型分目录
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/bin/${OUTPUTCONFIG}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/lib/${OUTPUTCONFIG}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_BINARY_DIR}/lib/${OUTPUTCONFIG}")
endforeach()

# -----------------------------------------------------------------------------
# 4. Windows 平台 UTF-8 编码（MSVC /utf-8）
#    避免源码中中文字符串字面量被解释为 GBK 导致乱码
# -----------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8)
    # 使用 UTF-8 作为运行时编码（Win10+ 支持）
    add_compile_definitions(UNICODE _UNICODE)
endif()

# 默认使用 Windows 应用子系统的控制台版本（GUI 程序后续在目标上单独设置 WIN32)
# 控制台子系统能让 stdout 输出可见，便于服务进程调试

# -----------------------------------------------------------------------------
# 5. 引入辅助 CMake 模块
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)   # 项目通用警告级别
include(QtDeployment)        # windeployqt 封装

# -----------------------------------------------------------------------------
# 6. 查找 Qt 6（Widgets、Network、Concurrent）
#    需要 CMAKE_PREFIX_PATH 指向 Qt 6 安装目录（如 D:\Qt\6.x.x\msvc2022_64）
# -----------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent Svg)

# 强制 Qt 自动化工具（MOC/RCC/UIC）启用
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# -----------------------------------------------------------------------------
# 7. 通过 vcpkg 查找第三方库（vcpkg manifest mode 会自动下载并编译）
#    要求用户配置 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# -----------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)   # libsodium 在 vcpkg 中的 config 名称
find_package(SQLite3 REQUIRED)
find_package(GTest REQUIRED)

# 向父作用域暴露常用变量（供 src/ 子目录使用）
set(YULI_VAULT_QT_LIBS Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg CACHE INTERNAL "Qt 6 链接库")

# -----------------------------------------------------------------------------
# 8. 启用测试
# -----------------------------------------------------------------------------
enable_testing()

# -----------------------------------------------------------------------------
# 9. 子目录
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
else()
    message(WARNING "Yuli Vault: src/CMakeLists.txt is missing")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
else()
    message(STATUS "Yuli Vault: tests/ is missing; skipping tests")
endif()

# -----------------------------------------------------------------------------
# 10. 安装规则
#     - 可执行目标由 src/ui/CMakeLists.txt 与 src/service/CMakeLists.txt
#       通过 install(TARGETS ... RUNTIME DESTINATION bin) 自行注册，此处不重复。
#     - Qt 运行时部署（deploy_qt_runtime）在 src/ui/CMakeLists.txt 中调用，
#       因为 add_custom_command(TARGET ...) 要求目标在创建它的同一目录注册。
#     - 安装 LICENSE 文件（优先根目录，缺失时回退 legacy-python/LICENSE）。
# -----------------------------------------------------------------------------

# 安装 LICENSE 文件：优先使用根目录 LICENSE，否则回退到 legacy-python/LICENSE
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            DESTINATION .
            COMPONENT Runtime)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/legacy-python/LICENSE")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/legacy-python/LICENSE"
            DESTINATION .
            COMPONENT Runtime
            RENAME LICENSE)
    message(STATUS "Yuli Vault: using legacy-python/LICENSE")
endif()

# 安装 README 文件（若存在）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            DESTINATION .
            COMPONENT Runtime)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md"
            DESTINATION .
            COMPONENT Runtime)
endif()

message(STATUS "")
message(STATUS "========== Yuli Vault ==========")
message(STATUS "  项目版本       : ${PROJECT_VERSION}")
message(STATUS "  C++ 标准       : ${CMAKE_CXX_STANDARD}")
message(STATUS "  构建类型       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  生成器         : ${CMAKE_GENERATOR}")
message(STATUS "  Qt 版本        : ${Qt6_VERSION}")
message(STATUS "  OpenSSL 版本   : ${OPENSSL_VERSION}")
message(STATUS "  GoogleTest     : ${GTest_VERSION}")
message(STATUS "  可执行输出目录 : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "  库输出目录     : ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "========================================")
message(STATUS "")

# -----------------------------------------------------------------------------
# 11. 打包配置（Inno Setup 安装包）
#     项目要求 CMake 3.20+，而 CPack 内置 INNO 生成器在 CMake 3.27+ 才正式
#     支持。此处采用手写 .iss 方案：通过 add_custom_target 调用 iscc.exe
#     生成安装包，依赖两个可执行目标构建完成。
#
#     使用方式：
#       cmake --build build --config Release
#       cmake --build build --target package_inno    # 调用 iscc 生成 setup.exe
# -----------------------------------------------------------------------------
# 查找 Inno Setup 编译器（iscc.exe），找不到时仅给出提示，不阻断配置
# 覆盖三种常见安装位置：系统级（Program Files）、用户级（%LOCALAPPDATA%）
find_program(ISCC_EXECUTABLE
    NAMES iscc iscc.exe
    PATHS "C:/Program Files (x86)/Inno Setup 6"
          "C:/Program Files/Inno Setup 6"
          "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    DOC "Inno Setup 编译器路径"
)
if(ISCC_EXECUTABLE)
    message(STATUS "Yuli Vault: Inno Setup compiler -> ${ISCC_EXECUTABLE}")
else()
    message(STATUS "Yuli Vault: Inno Setup compiler (iscc.exe) not found; "
                   "package_inno will be unavailable.")
endif()

# 自定义打包目标：先用 windeployqt 填满 build/bin/<config>，再编译 .iss。
# WORKING_DIRECTORY 指向项目根目录，使 .iss 内的相对路径生效。
if(YULI_VAULT_WINDEPLOYQT)
    add_custom_target(package_inno
        COMMAND "${YULI_VAULT_WINDEPLOYQT}" ${YULI_VAULT_WINDEPLOYQT_ARGS}
                "$<TARGET_FILE:yuli-vault-ui>"
        COMMAND ${ISCC_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/packaging/yuli-vault.iss"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS yuli-vault-ui yuli-vault-service
        COMMENT "Yuli Vault: windeployqt + Inno Setup (packaging/yuli-vault.iss)..."
        VERBATIM
    )
else()
    add_custom_target(package_inno
        COMMAND ${ISCC_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/packaging/yuli-vault.iss"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS yuli-vault-ui yuli-vault-service
        COMMENT "Yuli Vault: compiling installer (packaging/yuli-vault.iss)..."
        VERBATIM
    )
endif()
