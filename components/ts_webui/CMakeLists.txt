idf_component_register(
    SRCS
        "src/ts_webui.c"
        "src/ts_webui_api.c"
        "src/ts_webui_ws.c"
        "src/ts_ws_subscriptions.c"
        "src/ts_ws_transport.c"
        "src/ts_ws_operation.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ts_core
        ts_net
        ts_api
        ts_security
        ts_storage
        ts_console
        ts_automation
        esp_http_server
    PRIV_REQUIRES
        json
)

# ===========================================================================
# Web 资源构建时优化：minify + gzip 预压缩
#
# 流程：源文件 -> 复制到 build/web -> minify(JS/CSS) -> gzip -> SPIFFS 打包
# 原始源文件不会被修改，所有处理在构建目录中进行
# ===========================================================================
set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/web_optimized")
set(MINIFY_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/minify_web.py")

if(EXISTS "${WEB_SRC_DIR}")

    # 收集所有源文件（用于依赖追踪）
    file(GLOB_RECURSE WEB_ALL_FILES "${WEB_SRC_DIR}/*")

    # 步骤 1：复制 web 目录到构建目录
    add_custom_command(
        OUTPUT "${WEB_BUILD_DIR}/.stamp_copy"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${WEB_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${WEB_SRC_DIR}" "${WEB_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${WEB_BUILD_DIR}/.stamp_copy"
        DEPENDS ${WEB_ALL_FILES}
        COMMENT "Copying web assets to build directory"
        VERBATIM
    )

    # 步骤 2+3：Minify JS/CSS + Gzip 预压缩（纯 Python，无外部依赖）
    find_program(PYTHON3 python3)
    if(PYTHON3 AND EXISTS "${MINIFY_SCRIPT}")
        add_custom_command(
            OUTPUT "${WEB_BUILD_DIR}/.stamp_optimize"
            COMMAND ${PYTHON3} "${MINIFY_SCRIPT}" "${WEB_BUILD_DIR}" --gzip
            COMMAND ${CMAKE_COMMAND} -E touch "${WEB_BUILD_DIR}/.stamp_optimize"
            DEPENDS "${WEB_BUILD_DIR}/.stamp_copy" "${MINIFY_SCRIPT}"
            COMMENT "Minifying + gzipping web resources"
            VERBATIM
        )
        set(FINAL_STAMP "${WEB_BUILD_DIR}/.stamp_optimize")
        message(STATUS "Web optimization enabled (minify + gzip)")
    else()
        set(FINAL_STAMP "${WEB_BUILD_DIR}/.stamp_copy")
        message(STATUS "Web optimization skipped (python3 or script not found)")
    endif()

    add_custom_target(optimize_web ALL DEPENDS ${FINAL_STAMP})

    # 从优化后的构建目录创建 SPIFFS 镜像
    spiffs_create_partition_image(www ${WEB_BUILD_DIR} FLASH_IN_PROJECT DEPENDS optimize_web)

endif()
