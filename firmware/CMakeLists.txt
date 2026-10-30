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
    src/feed.c
    src/protocol.c
    src/testsignal.c
    src/usb_descriptors.c
    src/display/display_task.c
    src/display/hmi.c
    src/display/ili9341.c
    src/display/settings.c
    src/display/private_store.c
    src/display/settings_codec.c
    src/ecg/hr.c
    src/ecg/qrs.c
    src/ecg/sosf.c
    src/analysis/ana.c
    src/analysis/ana_breath.c
    src/analysis/ana_math.c
    src/analysis/ana_rr.c
    src/analysis/ana_wave.c
    src/ui/app.c
    src/ui/assets.c
    src/ui/avatar.c
    src/ui/breath.c
    src/ui/coach.c
    src/ui/check.c
    src/ui/gesture.c
    src/ui/gfx.c
    src/ui/keyboard.c
    src/ui/monitor.c
    src/ui/private_data.c
    src/ui/privat.c
    src/ui/profile.c
    src/ui/solo.c
    src/ui/splash.c
    src/ui/theme.c
    src/ui/touchcal.c
)
target_include_directories(ekg PRIVATE src src/analysis src/display src/ecg src/ui)
target_compile_options(ekg PRIVATE -Wall -Wextra)
target_link_libraries(ekg PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_adc
    hardware_dma
    hardware_flash
    hardware_pwm
    hardware_spi
    hardware_watchdog
    pico_flash
    pico_multicore
    pico_rand
    tinyusb_device
    tinyusb_board
)
pico_enable_stdio_usb(ekg 0)
pico_enable_stdio_uart(ekg 0)
pico_set_program_name(ekg "Pico EKG")
pico_set_program_version(ekg "0.8.0")
pico_add_extra_outputs(ekg)
