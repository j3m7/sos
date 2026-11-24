# Host-side unit tests. A separate project from the firmware: built with the host compiler,
# not the Pico SDK toolchain.
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

cmake_minimum_required(VERSION 3.13)
project(sos_host_tests C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(SOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

option(SOS_TEST_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
add_compile_options(-Wall -Wextra -g)
if (SOS_TEST_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# src/ssh/ringbuffer.c: buffers for all SSH session I/O.
add_executable(test_ringbuffer test_ringbuffer.c ${SOS_ROOT}/src/ssh/ringbuffer.c)
target_include_directories(test_ringbuffer PRIVATE ${SOS_ROOT}/src/ssh/include)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

# src/shell_core.c: shell line editing and output (commands are faked by the test).
add_executable(test_shell_line test_shell_line.c ${SOS_ROOT}/src/shell_core.c)
target_include_directories(test_shell_line PRIVATE ${SOS_ROOT}/src)
add_test(NAME shell_line COMMAND test_shell_line)
