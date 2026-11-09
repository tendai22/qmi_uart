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

project(qmi_usrt C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(qmi_usrt qmi_usrt.c cmd.c memprt.c support.c
                qmi_floppy.c
                qmi_flash.c qmi_flash_sync.c qmi_flash_async.c)

pico_set_program_name(qmi_usrt "qmi_usrt")
pico_set_program_version(qmi_usrt "0.1")

# no_flash means the target is to run from RAM
pico_set_binary_type(qmi_usrt copy_to_ram)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(qmi_usrt 1)
pico_enable_stdio_usb(qmi_usrt 0)

# Add the standard library to the build
target_link_libraries(qmi_usrt
        pico_stdlib
        hardware_uart
        hardware_clocks
        hardware_flash
)

# Add the standard include files to the build
target_include_directories(qmi_usrt PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)
# link option
target_link_options(qmi_usrt PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/qmi_usrt.map"
)

pico_add_extra_outputs(qmi_usrt)

