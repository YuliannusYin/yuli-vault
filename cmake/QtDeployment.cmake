# coding: utf-8
# =============================================================================
# QtDeployment.cmake
#
# 封装 Qt 运行时部署逻辑，提供 deploy_qt_runtime(target) 函数。
#
# 部署分两个阶段：
#   1. POST_BUILD 阶段：构建目标后立即复制 Qt 运行时 DLL 与 platforms 插件
#      到输出目录，使开发期间可直接运行 .exe（无需 cmake --install）。
#   2. install 阶段：若 windeployqt 可用，调用它做完整部署（含翻译、
#      imageformats 等），用于正式打包。
#
# 典型用法（在目标定义后）：
#   deploy_qt_runtime(yuli-vault-ui)
# =============================================================================

# 查找 windeployqt / qtpaths（官方 Qt 或 vcpkg tools/Qt6/bin）。
function(_yuli_vault_qt_tool_hints out_var)
    set(_hints)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin")
    endif()
    if(CMAKE_BINARY_DIR)
        list(APPEND _hints
            "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/tools/Qt6/bin")
    endif()
    if(TARGET Qt6::qmake)
        get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
        if(_qmake_executable)
            get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
            list(APPEND _hints "${_qt_bin_dir}")
        endif()
    endif()
    set(${out_var} "${_hints}" PARENT_SCOPE)
endfunction()

function(_yuli_vault_find_windeployqt out_var)
    if(TARGET Qt6::windeployqt)
        get_target_property(_imported Qt6::windeployqt IMPORTED_LOCATION)
        if(NOT _imported)
            get_target_property(_imported Qt6::windeployqt IMPORTED_LOCATION_RELEASE)
        endif()
        if(_imported)
            set(${out_var} "${_imported}" PARENT_SCOPE)
            return()
        endif()
        set(${out_var} "Qt6::windeployqt" PARENT_SCOPE)
        return()
    endif()

    _yuli_vault_qt_tool_hints(_hints)
    find_program(_windeployqt_exe
        NAMES windeployqt windeployqt.exe windeployqt6 windeployqt6.exe
        HINTS ${_hints}
    )
    if(_windeployqt_exe)
        set(${out_var} "${_windeployqt_exe}" PARENT_SCOPE)
        return()
    endif()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(_yuli_vault_find_qtpaths out_var)
    _yuli_vault_qt_tool_hints(_hints)
    find_program(_qtpaths_exe
        NAMES qtpaths qtpaths.exe qtpaths6 qtpaths6.exe
        HINTS ${_hints}
    )
    set(${out_var} "${_qtpaths_exe}" PARENT_SCOPE)
endfunction()

# 查找 Qt 插件目录。
# 兼容多种安装布局：
#   - 标准 Qt 安装：Qt6_DIR=<prefix>/lib/cmake/Qt6，插件在 <prefix>/plugins
#   - vcpkg 安装：  Qt6_DIR=<prefix>/x64-windows/share/Qt6，
#                   qmake 在 <prefix>/x64-windows/tools/Qt6/bin，
#                   插件在 <prefix>/x64-windows/Qt6/plugins
function(_yuli_vault_find_qt_plugins_dir out_var)
    # 1. 优先用 Qt6 CMake 配置提供的 QT6_INSTALL_PLUGINS 变量
    if(DEFINED QT6_INSTALL_PLUGINS AND EXISTS "${QT6_INSTALL_PLUGINS}/platforms/qwindows.dll")
        set(${out_var} "${QT6_INSTALL_PLUGINS}" PARENT_SCOPE)
        return()
    endif()

    # 2. 从 Qt6_DIR 推断（Qt6_DIR 指向 Qt6Config.cmake 所在目录）
    if(DEFINED Qt6_DIR)
        # vcpkg: Qt6_DIR=<root>/x64-windows/share/Qt6，插件在 <root>/x64-windows/Qt6/plugins
        get_filename_component(_dir "${Qt6_DIR}" DIRECTORY)  # .../x64-windows/share
        get_filename_component(_dir "${_dir}" DIRECTORY)      # .../x64-windows
        if(EXISTS "${_dir}/Qt6/plugins/platforms/qwindows.dll")
            set(${out_var} "${_dir}/Qt6/plugins" PARENT_SCOPE)
            return()
        endif()
        # 标准 Qt: Qt6_DIR=<prefix>/lib/cmake/Qt6，插件在 <prefix>/plugins
        get_filename_component(_dir "${Qt6_DIR}" DIRECTORY)  # .../lib/cmake
        get_filename_component(_dir "${_dir}" DIRECTORY)      # .../lib
        get_filename_component(_dir "${_dir}" DIRECTORY)      # .../<prefix>
        if(EXISTS "${_dir}/plugins/platforms/qwindows.dll")
            set(${out_var} "${_dir}/plugins" PARENT_SCOPE)
            return()
        endif()
    endif()

    # 3. 从 qmake 路径推断（兜底）
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    if(_qmake_executable)
        get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)
        get_filename_component(_qt_prefix_dir "${_qt_bin_dir}" DIRECTORY)
        # vcpkg tools 风格：<prefix>/tools/Qt6/bin → 向上找到 <prefix>，插件在 <prefix>/Qt6/plugins
        get_filename_component(_vcpkg_tools_dir "${_qt_prefix_dir}" DIRECTORY)  # .../tools
        if(EXISTS "${_vcpkg_tools_dir}/../Qt6/plugins/platforms/qwindows.dll")
            get_filename_component(_candidate "${_vcpkg_tools_dir}/../Qt6/plugins" ABSOLUTE)
            if(EXISTS "${_candidate}/platforms/qwindows.dll")
                set(${out_var} "${_candidate}" PARENT_SCOPE)
                return()
            endif()
        endif()
        # 标准风格：<prefix>/bin → <prefix>/plugins
        if(EXISTS "${_qt_prefix_dir}/plugins/platforms/qwindows.dll")
            set(${out_var} "${_qt_prefix_dir}/plugins" PARENT_SCOPE)
            return()
        endif()
    endif()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# deploy_qt_runtime(target)
#
# 为指定可执行目标注册 Qt 运行时部署：
#   - POST_BUILD：构建后立即复制 platforms 插件到输出目录，使开发期间
#     可直接运行 .exe（不依赖 windeployqt，兼容 vcpkg 安装的 qtbase）。
#   - install 阶段：若 windeployqt 可用，调用它做完整部署（含翻译、
#     imageformats、styles 等），用于正式打包；不可用时仅复制 platforms。
#
# 参数：
#   target     - 必须是可执行目标（如 yuli-vault-ui、yuli-vault-service）
# -----------------------------------------------------------------------------
function(deploy_qt_runtime target)
    if(NOT WIN32)
        message(STATUS "deploy_qt_runtime: 非 Windows 平台，跳过 ${target}")
        return()
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "deploy_qt_runtime: 目标 '${target}' 不存在")
    endif()

    _yuli_vault_find_qt_plugins_dir(_qt_plugins_dir)
    _yuli_vault_find_windeployqt(_windeployqt)
    _yuli_vault_find_qtpaths(_qtpaths)

    set(_wdq_args
        --no-translations
        --no-system-d3d-compiler
        --no-opengl-sw
        --compiler-runtime
    )
    if(_qtpaths)
        list(APPEND _wdq_args --qtpaths "${_qtpaths}")
    endif()

    if(_windeployqt)
        set(YULI_VAULT_WINDEPLOYQT "${_windeployqt}" CACHE FILEPATH
            "windeployqt executable used by POST_BUILD and package_inno" FORCE)
        if(_qtpaths)
            set(YULI_VAULT_QTPATHS "${_qtpaths}" CACHE FILEPATH
                "qtpaths executable passed to windeployqt" FORCE)
        endif()
        set(YULI_VAULT_WINDEPLOYQT_ARGS "${_wdq_args}" CACHE INTERNAL
            "windeployqt flags")
    endif()

    # ----------------------------------------------------------------------
    # POST_BUILD: full deploy when windeployqt exists; otherwise copy plugins.
    # ----------------------------------------------------------------------
    if(_windeployqt)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${_windeployqt}"
                    ${_wdq_args}
                    "$<TARGET_FILE:${target}>"
            COMMENT "deploy_qt_runtime: windeployqt $<TARGET_FILE_NAME:${target}>"
            VERBATIM
        )
        message(STATUS "deploy_qt_runtime: POST_BUILD windeployqt -> ${_windeployqt}")
    elseif(_qt_plugins_dir)
        foreach(_plugin_dir IN ITEMS platforms imageformats styles tls iconengines generic networkinformation)
            if(EXISTS "${_qt_plugins_dir}/${_plugin_dir}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                            "$<TARGET_FILE_DIR:${target}>/${_plugin_dir}"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${_qt_plugins_dir}/${_plugin_dir}"
                            "$<TARGET_FILE_DIR:${target}>/${_plugin_dir}"
                    COMMENT "deploy_qt_runtime: copy Qt plugin ${_plugin_dir}"
                    VERBATIM
                )
            endif()
        endforeach()
        message(STATUS "deploy_qt_runtime: windeployqt not found; POST_BUILD copies plugin dirs")
    else()
        message(WARNING "deploy_qt_runtime: no Qt plugins directory; ${target} may fail to start.")
    endif()

    # ----------------------------------------------------------------------
    # install 阶段
    # ----------------------------------------------------------------------
    if(_windeployqt)
        set(_qtpaths_install_arg "")
        if(_qtpaths)
            set(_qtpaths_install_arg "--qtpaths \"${_qtpaths}\"")
        endif()
        install(CODE
            "message(STATUS \"正在为 ${target} 部署 Qt 运行时...\")
             execute_process(
                 COMMAND \"${_windeployqt}\"
                         --no-translations
                         --no-system-d3d-compiler
                         --no-opengl-sw
                         --compiler-runtime
                         ${_qtpaths_install_arg}
                         --release
                         \"\${CMAKE_INSTALL_PREFIX}/bin/$<TARGET_FILE_NAME:${target}>\"
                 WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/bin\"
                 RESULT_VARIABLE _windeployqt_result
             )
             if(_windeployqt_result AND NOT _windeployqt_result EQUAL 0)
                 message(WARNING \"windeployqt 退出码: \${_windeployqt_result}\")
             endif()"
            COMPONENT Runtime
        )
        message(STATUS "deploy_qt_runtime: install-time windeployqt registered")
    elseif(_qt_plugins_dir)
        foreach(_plugin_dir IN ITEMS platforms imageformats styles tls iconengines generic networkinformation)
            if(EXISTS "${_qt_plugins_dir}/${_plugin_dir}")
                install(DIRECTORY "${_qt_plugins_dir}/${_plugin_dir}/"
                        DESTINATION "bin/${_plugin_dir}"
                        COMPONENT Runtime)
            endif()
        endforeach()
        message(STATUS "deploy_qt_runtime: install copies plugin dirs (no windeployqt)")
    endif()
endfunction()
