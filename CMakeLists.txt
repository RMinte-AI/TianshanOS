# TianShanOS - Main Project CMakeLists.txt
# ESP32 机架管理操作系统
#
# 作者: TianShanOS Team
# 版本: 从 version.txt 读取，每次构建自动生成新时间戳

cmake_minimum_required(VERSION 3.16)

# ============================================================================
# 版本管理 - 使用 Python 脚本生成版本号
# 格式: MAJOR.MINOR.PATCH+HASH.TIMESTAMP
# 例如: 0.3.1+7fdcca4d.01311234
# 
# 配置阶段会生成版本号，每次 fullClean 或手动 reconfigure 会更新时间戳
# ============================================================================

# 使用 Python 脚本生成版本号
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_version.py" "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE TIANSHAN_VERSION_DISPLAY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GEN_VERSION_RESULT
)

# 如果脚本失败，回退到简单版本
if(NOT GEN_VERSION_RESULT EQUAL 0 OR NOT TIANSHAN_VERSION_DISPLAY)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_CONTENT)
    string(STRIP "${VERSION_CONTENT}" TIANSHAN_VERSION_DISPLAY)
    message(WARNING "Version generation script failed, using: ${TIANSHAN_VERSION_DISPLAY}")
endif()

# 从完整版本字符串解析各组件
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" VERSION_MATCH "${TIANSHAN_VERSION_DISPLAY}")
if(NOT VERSION_MATCH)
    message(FATAL_ERROR "Invalid version format: ${TIANSHAN_VERSION_DISPLAY}")
endif()
set(TIANSHAN_VERSION_MAJOR ${CMAKE_MATCH_1})
set(TIANSHAN_VERSION_MINOR ${CMAKE_MATCH_2})
set(TIANSHAN_VERSION_PATCH ${CMAKE_MATCH_3})

# 解析预发布标识
string(REGEX MATCH "-([a-zA-Z0-9.]+)" PRERELEASE_MATCH "${TIANSHAN_VERSION_DISPLAY}")
if(PRERELEASE_MATCH)
    set(TIANSHAN_VERSION_PRERELEASE ${CMAKE_MATCH_1})
else()
    set(TIANSHAN_VERSION_PRERELEASE "")
endif()

# 解析构建元数据
string(REGEX MATCH "\\+([a-zA-Z0-9.]+)" BUILD_MATCH "${TIANSHAN_VERSION_DISPLAY}")
if(BUILD_MATCH)
    set(TIANSHAN_VERSION_BUILD ${CMAKE_MATCH_1})
else()
    set(TIANSHAN_VERSION_BUILD "")
endif()

if(ROOT_PASSWORD_RESET)
    if(ROOT_PASSWORD_RESET_VERSION)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" ROOT_RESET_VERSION_MATCH "${ROOT_PASSWORD_RESET_VERSION}")
        if(NOT ROOT_RESET_VERSION_MATCH)
            message(FATAL_ERROR "ROOT_PASSWORD_RESET_VERSION must use MAJOR.MINOR.PATCH format: ${ROOT_PASSWORD_RESET_VERSION}")
        endif()
        set(TIANSHAN_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(TIANSHAN_VERSION_MINOR ${CMAKE_MATCH_2})
        set(TIANSHAN_VERSION_PATCH ${CMAKE_MATCH_3})
        set(TIANSHAN_VERSION_PRERELEASE "")
        set(TIANSHAN_VERSION_BUILD "")
    endif()
    if(NOT ROOT_PASSWORD_RESET_RUN_ID)
        string(TIMESTAMP ROOT_PASSWORD_RESET_RUN_ID "%H%M%S")
    endif()
    string(REGEX REPLACE "[^a-zA-Z0-9.]" "." ROOT_PASSWORD_RESET_RUN_ID "${ROOT_PASSWORD_RESET_RUN_ID}")
    string(SUBSTRING "${ROOT_PASSWORD_RESET_RUN_ID}" 0 13 ROOT_PASSWORD_RESET_RUN_ID)

    set(TIANSHAN_VERSION_PRERELEASE "root-reset.${ROOT_PASSWORD_RESET_RUN_ID}")
    set(TIANSHAN_VERSION_BUILD "")
    set(TIANSHAN_VERSION_DISPLAY "${TIANSHAN_VERSION_MAJOR}.${TIANSHAN_VERSION_MINOR}.${TIANSHAN_VERSION_PATCH}-${TIANSHAN_VERSION_PRERELEASE}")
    string(LENGTH "${TIANSHAN_VERSION_DISPLAY}" TIANSHAN_VERSION_DISPLAY_LEN)
    if(TIANSHAN_VERSION_DISPLAY_LEN GREATER 31)
        message(FATAL_ERROR "Root reset PROJECT_VER is too long for esp_app_desc: ${TIANSHAN_VERSION_DISPLAY}")
    endif()
endif()

# 基础版本（不含构建元数据）
set(TIANSHAN_VERSION "${TIANSHAN_VERSION_MAJOR}.${TIANSHAN_VERSION_MINOR}.${TIANSHAN_VERSION_PATCH}")
if(TIANSHAN_VERSION_PRERELEASE)
    set(TIANSHAN_VERSION "${TIANSHAN_VERSION}-${TIANSHAN_VERSION_PRERELEASE}")
endif()

# 设置 ESP-IDF 项目版本（会嵌入到 esp_app_desc）
set(PROJECT_VER "${TIANSHAN_VERSION_DISPLAY}")

# 设置组件目录
set(EXTRA_COMPONENT_DIRS 
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ts_core"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ts_core/ts_config"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ts_core/ts_log"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ts_core/ts_event"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ts_core/ts_service"
)

# 使用项目自定义分区表（16MB Flash, 双 OTA 分区 + WebUI 分区）
set(PARTITION_TABLE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv" CACHE STRING "" FORCE)

# 包含 ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(ROOT_PASSWORD_RESET)
    idf_build_set_property(MINIMAL_BUILD ON)
    idf_build_set_property(ROOT_PASSWORD_RESET ON)
endif()

# 项目定义
project(TianShanOS VERSION ${TIANSHAN_VERSION_MAJOR}.${TIANSHAN_VERSION_MINOR}.${TIANSHAN_VERSION_PATCH})

if(NOT ROOT_PASSWORD_RESET)
    include(cmake/httpd_admission.cmake)
    include(cmake/dhcps_bindings.cmake)
endif()

# 添加编译定义 - 使用 idf_build_set_property 确保所有组件都能访问
idf_build_set_property(COMPILE_DEFINITIONS
    "TIANSHAN_OS_VERSION_MAJOR=${TIANSHAN_VERSION_MAJOR}"
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "TIANSHAN_OS_VERSION_MINOR=${TIANSHAN_VERSION_MINOR}"
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "TIANSHAN_OS_VERSION_PATCH=${TIANSHAN_VERSION_PATCH}"
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "TIANSHAN_OS_VERSION=\"${TIANSHAN_VERSION}\""
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "TIANSHAN_OS_VERSION_FULL=\"${TIANSHAN_VERSION_DISPLAY}\""
    APPEND
)
if(ROOT_PASSWORD_RESET)
    idf_build_set_property(COMPILE_DEFINITIONS
        "ROOT_PASSWORD_RESET_RUN_ID=\"${ROOT_PASSWORD_RESET_RUN_ID}\""
        APPEND
    )
endif()

# 打印项目信息
message(STATUS "")
message(STATUS "╔═══════════════════════════════════════════════════════════╗")
message(STATUS "║              TianShanOS v${TIANSHAN_VERSION_DISPLAY}                      ║")
message(STATUS "║           ESP32 Rack Management Operating System          ║")
message(STATUS "╚═══════════════════════════════════════════════════════════╝")
message(STATUS "")
message(STATUS "Version: ${TIANSHAN_VERSION_DISPLAY}")
message(STATUS "  Major: ${TIANSHAN_VERSION_MAJOR}")
message(STATUS "  Minor: ${TIANSHAN_VERSION_MINOR}")
message(STATUS "  Patch: ${TIANSHAN_VERSION_PATCH}")
if(TIANSHAN_VERSION_PRERELEASE)
    message(STATUS "  Prerelease: ${TIANSHAN_VERSION_PRERELEASE}")
endif()
if(TIANSHAN_VERSION_BUILD)
    message(STATUS "  Build: ${TIANSHAN_VERSION_BUILD}")
endif()
message(STATUS "")
message(STATUS "Target: ${IDF_TARGET}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
