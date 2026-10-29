# Host-Tests der hardwareunabhängigen Firmware-Teile:
#   cmake -S firmware/tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.13)
project(ekg_firmware_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(test_firmware
    test_firmware.c
    ../src/protocol.c
    ../src/decimate.c
    ../src/testsignal.c
)
target_include_directories(test_firmware PRIVATE ../src)
target_compile_options(test_firmware PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_firmware PRIVATE m)
add_test(NAME test_firmware COMMAND test_firmware)

# Bedienung und Auswertung (ohne Hardware)
file(GLOB ANALYSIS_SRC ../src/analysis/*.c)
add_executable(test_ui
    test_ui.c
    ../src/ui/app.c
    ../src/ui/assets.c
    ../src/ui/gesture.c
    ../src/ui/gfx.c
    ../src/ui/monitor.c
    ../src/ui/theme.c
    ../src/ui/touchcal.c
    ../src/ecg/hr.c
    ../src/ecg/qrs.c
    ../src/ecg/sosf.c
    ${ANALYSIS_SRC}
)
target_include_directories(test_ui PRIVATE ../src ../src/ui ../src/ecg ../src/analysis ../src/display)
target_compile_options(test_ui PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_ui PRIVATE m)
add_test(NAME test_ui COMMAND test_ui)
