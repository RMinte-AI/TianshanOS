# Change only this project's HTTPD objects, never the shared ESP-IDF checkout.
idf_component_get_property(httpd_lib esp_http_server COMPONENT_LIB)
idf_component_get_property(httpd_dir esp_http_server COMPONENT_DIR)
set(admission_dir "${CMAKE_BINARY_DIR}/httpd_admission")
set(admission_tool "${CMAKE_SOURCE_DIR}/tools/prepare_httpd_admission.py")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${admission_tool}"
            --idf-dir "$ENV{IDF_PATH}" --output "${admission_dir}"
    RESULT_VARIABLE admission_result
    ERROR_VARIABLE admission_error
)
if(NOT admission_result EQUAL 0)
    message(FATAL_ERROR "HTTPD connection protection unavailable: ${admission_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${admission_tool}" "${httpd_dir}/src/httpd_sess.c" "${httpd_dir}/src/httpd_main.c")
get_target_property(httpd_sources ${httpd_lib} SOURCES)
foreach(name httpd_sess.c httpd_main.c)
    set(found FALSE)
    set(updated_sources "")
    foreach(source IN LISTS httpd_sources)
        if(source STREQUAL "${httpd_dir}/src/${name}" OR source STREQUAL "src/${name}")
            list(APPEND updated_sources "${admission_dir}/${name}")
            set(found TRUE)
        else()
            list(APPEND updated_sources "${source}")
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "Cannot replace HTTPD ${name}; SDK component sources changed")
    endif()
    set(httpd_sources ${updated_sources})
endforeach()
set_property(TARGET ${httpd_lib} PROPERTY SOURCES ${httpd_sources})
target_include_directories(${httpd_lib} PRIVATE "${httpd_dir}/src")
