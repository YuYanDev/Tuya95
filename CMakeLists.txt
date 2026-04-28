# @file CMakeLists.txt
# @brief BiosSimulator - Phoenix BIOS + Win95 Desktop Simulator

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRC)

set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/src
)

set(APP_OPTIONS "-w")
list(APPEND APP_OPTIONS "-Wall")

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PUBLIC
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
    )
