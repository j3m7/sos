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

# src/ssh/pico-sshd/ringbuffer.c: buffers for all SSH session I/O.
add_executable(test_ringbuffer test_ringbuffer.c ${SOS_ROOT}/src/ssh/pico-sshd/ringbuffer.c)
target_include_directories(test_ringbuffer PRIVATE ${SOS_ROOT}/src/ssh/pico-sshd/include)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

# src/shell/shell_core.c: shell line editing and output (commands are faked by the test).
add_executable(test_shell_line test_shell_line.c ${SOS_ROOT}/src/shell/shell_core.c ${SOS_ROOT}/src/shell/keys.c)
target_include_directories(test_shell_line PRIVATE ${SOS_ROOT}/src)
add_test(NAME shell_line COMMAND test_shell_line)

# src/shell/keys.c: terminal key decoding (escape sequences to key codes).
add_executable(test_keys test_keys.c ${SOS_ROOT}/src/shell/keys.c)
target_include_directories(test_keys PRIVATE ${SOS_ROOT}/src)
add_test(NAME keys COMMAND test_keys)

# src/edit/editor.c: the full-screen editor's text, keys and screen.
add_executable(test_editor test_editor.c ${SOS_ROOT}/src/edit/editor.c ${SOS_ROOT}/src/shell/termtext.c)
target_include_directories(test_editor PRIVATE ${SOS_ROOT}/src)
add_test(NAME editor COMMAND test_editor)

# src/files/path.c: path resolution for the file commands.
add_executable(test_path test_path.c ${SOS_ROOT}/src/files/path.c)
target_include_directories(test_path PRIVATE ${SOS_ROOT}/src)
add_test(NAME path COMMAND test_path)

# src/core/config.c: persistent configuration (A/B flash records, key parsing, validation).
add_executable(test_config test_config.c ${SOS_ROOT}/src/core/config.c)
target_include_directories(test_config PRIVATE ${SOS_ROOT}/src)
add_test(NAME config COMMAND test_config)

# src/core/log.c: log levels, console output with input-line redraw, RAM history.
add_executable(test_log test_log.c ${SOS_ROOT}/src/core/log.c)
target_include_directories(test_log PRIVATE ${SOS_ROOT}/src)
add_test(NAME log COMMAND test_log)

# src/time/timeconv.c: Unix time <-> calendar and PCF8523 RTC registers.
add_executable(test_timeconv test_timeconv.c ${SOS_ROOT}/src/time/timeconv.c)
target_include_directories(test_timeconv PRIVATE ${SOS_ROOT}/src)
add_test(NAME timeconv COMMAND test_timeconv)

# src/time/nts_ke.c: NTS-KE request encoding and response parsing (RFC 8915).
add_executable(test_nts_ke test_nts_ke.c ${SOS_ROOT}/src/time/nts_ke.c)
target_include_directories(test_nts_ke PRIVATE ${SOS_ROOT}/src)
add_test(NAME nts_ke COMMAND test_nts_ke)

# src/time/nts_ntp.c: NTS-protected NTP packets (RFC 8915 section 5), with a stand-in AEAD.
add_executable(test_nts_ntp test_nts_ntp.c ${SOS_ROOT}/src/time/nts_ntp.c)
target_include_directories(test_nts_ntp PRIVATE ${SOS_ROOT}/src)
add_test(NAME nts_ntp COMMAND test_nts_ntp)

# src/tls/certbundle.c: PEM bundles and DER certificates for /certs/roots.pem (base64 from config.c).
add_executable(test_certbundle test_certbundle.c ${SOS_ROOT}/src/tls/certbundle.c ${SOS_ROOT}/src/core/config.c)
target_include_directories(test_certbundle PRIVATE ${SOS_ROOT}/src)
add_test(NAME certbundle COMMAND test_certbundle)

# tools/host/mkconfig.c: the provisioning file and the config image written by tools/provision.sh.
add_executable(test_mkconfig test_mkconfig.c ${SOS_ROOT}/tools/host/mkconfig.c ${SOS_ROOT}/src/core/config.c)
target_compile_definitions(test_mkconfig PRIVATE MKCONFIG_NO_MAIN)
target_include_directories(test_mkconfig PRIVATE ${SOS_ROOT}/src ${SOS_ROOT}/tools/host)
add_test(NAME mkconfig COMMAND test_mkconfig)

# src/shell/termtext.c: outside text made safe for the terminal (C0/C1 controls, UTF-8).
add_executable(test_termtext test_termtext.c ${SOS_ROOT}/src/shell/termtext.c)
target_include_directories(test_termtext PRIVATE ${SOS_ROOT}/src)
add_test(NAME termtext COMMAND test_termtext)

# src/net/http_msg.c: HTTPS client messages (URLs, requests, incremental response parsing).
add_executable(test_http_msg test_http_msg.c ${SOS_ROOT}/src/net/http_msg.c)
target_include_directories(test_http_msg PRIVATE ${SOS_ROOT}/src)
add_test(NAME http_msg COMMAND test_http_msg)

# src/chat/chat.c: wall, chat and who between shells (with the real shell_core.c and termtext.c).
add_executable(test_chat test_chat.c ${SOS_ROOT}/src/chat/chat.c
    ${SOS_ROOT}/src/shell/shell_core.c ${SOS_ROOT}/src/shell/keys.c ${SOS_ROOT}/src/shell/termtext.c)
target_include_directories(test_chat PRIVATE ${SOS_ROOT}/src)
add_test(NAME chat COMMAND test_chat)
