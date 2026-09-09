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
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(firmware-notifier C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(firmware-notifier firmware-notifier.c )

pico_set_program_name(firmware-notifier "firmware-notifier")
pico_set_program_version(firmware-notifier "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(firmware-notifier 0)
pico_enable_stdio_usb(firmware-notifier 1)
pico_generate_pio_header(
    firmware-notifier
    ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio
)

# Add the standard library to the build
target_link_libraries(firmware-notifier
        pico_stdlib  hardware_pio)

# Add the standard include files to the build
target_include_directories(firmware-notifier PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)
# change the VID PID MANUFACTURER and PRODUCT for customization, don't forget to update the notifier-host-executable with the new VID/PID
target_compile_definitions(
    firmware-notifier
    PRIVATE
    USBD_VID=0xCAFE
    USBD_PID=0x1337
    "USBD_MANUFACTURER=\"Test\""
    "USBD_PRODUCT=\"Notifier\""
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)
pico_add_extra_outputs(firmware-notifier)

