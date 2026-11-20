# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)


project(qmi_uart C CXX ASM)

pico_sdk_init()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Protothread table generator
set(GEN_TABLE_PY
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_table.py"
)

# Protothread source files
set(GEN_TABLE_SRC_FILES
    pt_qmi_common.c
)

set(GENERATED_TABLE_SRCS)
set(GENERATED_TABLE_HDRS)

foreach(SRC IN LISTS GEN_TABLE_SRC_FILES)
    get_filename_component(BASE "${SRC}" NAME_WE)

    set(SOURCE_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
    )
    set(GENERATED_H
        "${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.h"
    )
    set(GENERATED_C
        "${CMAKE_CURRENT_BINARY_DIR}/${BASE}_table.c"
    )

    if(NOT EXISTS "${SOURCE_FILE}")
        message(FATAL_ERROR
            "Protothread source not found: ${SOURCE_FILE}"
        )
    endif()

    add_custom_command(
        OUTPUT
            "${GENERATED_H}"
            "${GENERATED_C}"
        COMMAND
            "${Python3_EXECUTABLE}"
            "${GEN_TABLE_PY}"
            "${SOURCE_FILE}"
            "${GENERATED_H}"
            "${GENERATED_C}"
        DEPENDS
            "${SOURCE_FILE}"
            "${GEN_TABLE_PY}"
        COMMENT "Generating ${BASE}_table.[ch]"
        VERBATIM
    )

    list(APPEND GENERATED_TABLE_SRCS
        "${GENERATED_C}"
    )
    list(APPEND GENERATED_TABLE_HDRS
        "${GENERATED_H}"
    )
endforeach()

#
# 生成専用ターゲット
#

add_custom_target(generate_pt_tables
    DEPENDS
        ${GENERATED_TABLE_SRCS}
        ${GENERATED_TABLE_HDRS}
)

# Add executable. Default name is the project name, version 0.1

add_executable(qmi_uart qmi_uart.c cmd.c memprt.c support.c
                qmi_floppy.c pt_qmi_common.c
                qmi_flash.c qmi_flash_sync.c qmi_flash_async.c
                ${GENERATED_TABLE_SRCS})

pico_set_program_name(qmi_uart "qmi_uart")
pico_set_program_version(qmi_uart "0.1")

# no_flash means the target is to run from RAM
pico_set_binary_type(qmi_uart copy_to_ram)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(qmi_uart 1)
pico_enable_stdio_usb(qmi_uart 0)

# Add the standard include files to the build
target_include_directories(qmi_uart PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_LIST_DIR}
)

#
# 先に生成
#

add_dependencies(qmi_uart generate_pt_tables)

# Add the standard library to the build
target_link_libraries(qmi_uart
        pico_stdlib
        hardware_uart
        hardware_clocks
        hardware_flash
        pico_multicore
)

# link option
target_link_options(qmi_uart PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/qmi_uart.map"
)

pico_add_extra_outputs(qmi_uart)

