# Replace exactly two SDK inputs for this build; never mutate the shared SDK.
set(bindings_dir "${CMAKE_BINARY_DIR}/dhcps_bindings")
set(bindings_tool "${CMAKE_SOURCE_DIR}/tools/prepare_dhcps_bindings.py")
set(bindings_headers "${CMAKE_SOURCE_DIR}/components/ts_net/private_include")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${bindings_tool}"
            --idf-dir "$ENV{IDF_PATH}" --output "${bindings_dir}"
    RESULT_VARIABLE bindings_result
    ERROR_VARIABLE bindings_error
)
if(NOT bindings_result EQUAL 0)
    message(FATAL_ERROR "DHCP address ownership unavailable: ${bindings_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${bindings_tool}" "${bindings_headers}/ts_dhcps_binding_table.h"
    "${bindings_headers}/ts_dhcps_bindings.h"
    "$ENV{IDF_PATH}/components/lwip/apps/dhcpserver/dhcpserver.c"
    "$ENV{IDF_PATH}/components/esp_netif/lwip/esp_netif_lwip.c")
foreach(component lwip esp_netif)
    idf_component_get_property(bindings_lib ${component} COMPONENT_LIB)
    idf_component_get_property(bindings_component_dir ${component} COMPONENT_DIR)
    if(component STREQUAL "lwip")
        set(bindings_source "apps/dhcpserver/dhcpserver.c")
        set(bindings_name "dhcpserver.c")
    else()
        set(bindings_source "lwip/esp_netif_lwip.c")
        set(bindings_name "esp_netif_lwip.c")
    endif()
    get_target_property(bindings_sources ${bindings_lib} SOURCES)
    set(bindings_updated_sources "")
    set(bindings_found FALSE)
    foreach(source IN LISTS bindings_sources)
        if(source STREQUAL "${bindings_component_dir}/${bindings_source}" OR source STREQUAL "${bindings_source}")
            list(APPEND bindings_updated_sources "${bindings_dir}/${bindings_name}")
            set(bindings_found TRUE)
        else()
            list(APPEND bindings_updated_sources "${source}")
        endif()
    endforeach()
    if(NOT bindings_found)
        message(FATAL_ERROR "Cannot replace DHCP ${bindings_source}; SDK component sources changed")
    endif()
    set_property(TARGET ${bindings_lib} PROPERTY SOURCES ${bindings_updated_sources})
    target_include_directories(${bindings_lib} PRIVATE "${bindings_headers}")
endforeach()
# The generated esp_netif input retains its original same-directory includes.
target_include_directories(${bindings_lib} PRIVATE "${bindings_component_dir}/lwip")
