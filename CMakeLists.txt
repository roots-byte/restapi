cmake_minimum_required(VERSION 3.16)
project(restapi C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0600)
endif()

option(RESTAPI_BUILD_EXAMPLES "Build RESTAPI example executables" ON)

if(WIN32)
    set(RESTAPI_PLATFORM_NAME "Windows")
elseif(UNIX)
    set(RESTAPI_PLATFORM_NAME "Unix/Linux")
else()
    set(RESTAPI_PLATFORM_NAME "Unknown")
endif()

message(STATUS "RESTAPI target platform: ${RESTAPI_PLATFORM_NAME}")

set(RESTAPI_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(RESTAPI_OUT_BIN "${RESTAPI_OUTPUT_ROOT}/out_bin")
set(RESTAPI_OUT_LIB "${RESTAPI_OUTPUT_ROOT}/out_lib")

add_library(restapi_static STATIC
    restapi.c
)
target_include_directories(restapi_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(restapi_static PUBLIC ws2_32)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(restapi_static PUBLIC Threads::Threads)
endif()

set_target_properties(restapi_static PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${RESTAPI_OUT_LIB}"
)

if(RESTAPI_BUILD_EXAMPLES)
    add_executable(single_communication_example examples/single_communication/main.c)
    target_link_libraries(single_communication_example PRIVATE restapi_static)

    add_executable(multi_communication_example examples/multi_communication/main.c)
    target_link_libraries(multi_communication_example PRIVATE restapi_static)

    set_target_properties(single_communication_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${RESTAPI_OUT_BIN}"
    )

    set_target_properties(multi_communication_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${RESTAPI_OUT_BIN}"
    )
endif()