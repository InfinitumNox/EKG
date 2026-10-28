cmake_minimum_required(VERSION 3.13)

# Pico SDK: entweder PICO_SDK_PATH setzen oder automatisch holen lassen.
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_FETCH_FROM_GIT ON)
    set(PICO_SDK_FETCH_FROM_GIT_TAG 2.2.0)
endif()
set(PICO_BOARD pico2 CACHE STRING "Board")

include(pico_sdk_import.cmake)

project(ekg_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(ekg
    src/main.c
    src/capture.c
    src/calsignal.c
    src/decimate.c
    src/protocol.c
    src/testsignal.c
    src/usb_descriptors.c
)
target_include_directories(ekg PRIVATE src)
target_compile_options(ekg PRIVATE -Wall -Wextra)
target_link_libraries(ekg PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_adc
    hardware_dma
    hardware_pwm
    hardware_watchdog
    tinyusb_device
    tinyusb_board
)
pico_enable_stdio_usb(ekg 0)
pico_enable_stdio_uart(ekg 0)
pico_set_program_name(ekg "Pico EKG")
pico_set_program_version(ekg "0.1.0")
pico_add_extra_outputs(ekg)
