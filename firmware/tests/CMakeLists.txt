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
