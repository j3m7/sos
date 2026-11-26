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
add_executable(test_shell_line test_shell_line.c stub_version.c ${SOS_ROOT}/src/shell/shell_core.c ${SOS_ROOT}/src/shell/charwidth.c ${SOS_ROOT}/src/shell/keys.c)
target_include_directories(test_shell_line PRIVATE ${SOS_ROOT}/src)
add_test(NAME shell_line COMMAND test_shell_line)

# src/shell/login.c: the console's lock (password prompt, delays, the gate), with the real
# shell_core.c.
add_executable(test_login test_login.c stub_version.c ${SOS_ROOT}/src/shell/login.c
    ${SOS_ROOT}/src/shell/shell_core.c ${SOS_ROOT}/src/shell/charwidth.c ${SOS_ROOT}/src/shell/keys.c)
target_include_directories(test_login PRIVATE ${SOS_ROOT}/src)
add_test(NAME login COMMAND test_login)

# src/net/wifi_pick.c: which saved Wi-Fi network to try.
add_executable(test_wifi_pick test_wifi_pick.c ${SOS_ROOT}/src/net/wifi_pick.c)
target_include_directories(test_wifi_pick PRIVATE ${SOS_ROOT}/src)
add_test(NAME wifi_pick COMMAND test_wifi_pick)

# src/core/failcount.c: the count of wrong console passwords kept in flash across reboots.
add_executable(test_failcount test_failcount.c ${SOS_ROOT}/src/core/failcount.c)
target_include_directories(test_failcount PRIVATE ${SOS_ROOT}/src)
add_test(NAME failcount COMMAND test_failcount)

# src/shell/keys.c: terminal key decoding (escape sequences to key codes).
add_executable(test_keys test_keys.c ${SOS_ROOT}/src/shell/keys.c)
target_include_directories(test_keys PRIVATE ${SOS_ROOT}/src)
add_test(NAME keys COMMAND test_keys)

# src/shell/charwidth.c: terminal column widths (generated tables).
add_executable(test_charwidth test_charwidth.c ${SOS_ROOT}/src/shell/charwidth.c)
target_include_directories(test_charwidth PRIVATE ${SOS_ROOT}/src)
add_test(NAME charwidth COMMAND test_charwidth)

# src/edit/editor.c: the full-screen editor's text, keys and screen.
add_executable(test_editor test_editor.c ${SOS_ROOT}/src/edit/editor.c ${SOS_ROOT}/src/shell/termtext.c ${SOS_ROOT}/src/shell/charwidth.c)
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

# src/core/chipinfo.c: the RP2350 revision's name, shown by `version`.
add_executable(test_chipinfo test_chipinfo.c ${SOS_ROOT}/src/core/chipinfo.c)
target_include_directories(test_chipinfo PRIVATE ${SOS_ROOT}/src)
add_test(NAME chipinfo COMMAND test_chipinfo)

# src/core/bootkey.c: the key an image is signed with, and its comparison with the boot keys
# in OTP (the `bootkey` command).
add_executable(test_bootkey test_bootkey.c ${SOS_ROOT}/src/core/bootkey.c)
target_include_directories(test_bootkey PRIVATE ${SOS_ROOT}/src)
add_test(NAME bootkey COMMAND test_bootkey)

# src/core/sealedonly.c: whether the board refuses unsealed records, from its marker's OTP rows.
add_executable(test_sealedonly test_sealedonly.c ${SOS_ROOT}/src/core/sealedonly.c
    ${SOS_ROOT}/src/core/bootkey.c)
target_include_directories(test_sealedonly PRIVATE ${SOS_ROOT}/src)
add_test(NAME sealedonly COMMAND test_sealedonly)

# tools/flash.sh: the guard that refuses to flash an image not signed with an accepted key.
find_program(BASH_EXE bash)
if (BASH_EXE)
    add_test(NAME flash_guard COMMAND ${BASH_EXE} ${SOS_ROOT}/tools/test/flash-guard-test.sh)
endif()

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
add_executable(test_mkconfig test_mkconfig.c ${SOS_ROOT}/tools/host/mkconfig.c ${SOS_ROOT}/src/core/config.c
    ${SOS_ROOT}/src/time/timeconv.c)
target_compile_definitions(test_mkconfig PRIVATE MKCONFIG_NO_MAIN)
target_include_directories(test_mkconfig PRIVATE ${SOS_ROOT}/src ${SOS_ROOT}/tools/host)
add_test(NAME mkconfig COMMAND test_mkconfig)

# src/shell/termtext.c: outside text made safe for the terminal (C0/C1 controls, UTF-8).
add_executable(test_termtext test_termtext.c ${SOS_ROOT}/src/shell/termtext.c)
target_include_directories(test_termtext PRIVATE ${SOS_ROOT}/src)
add_test(NAME termtext COMMAND test_termtext)

# src/tls/tls_admit.c: which new TLS connections may open (slots kept for the system, heap).
add_executable(test_tls_admit test_tls_admit.c ${SOS_ROOT}/src/tls/tls_admit.c)
target_include_directories(test_tls_admit PRIVATE ${SOS_ROOT}/src)
add_test(NAME tls_admit COMMAND test_tls_admit)

# src/net/http_msg.c: HTTPS client messages (URLs, requests, incremental response parsing).
add_executable(test_http_msg test_http_msg.c ${SOS_ROOT}/src/net/http_msg.c)
target_include_directories(test_http_msg PRIVATE ${SOS_ROOT}/src)
add_test(NAME http_msg COMMAND test_http_msg)

# src/chat/chat.c: wall, chat and who between shells (with the real shell_core.c and termtext.c).
add_executable(test_chat test_chat.c stub_version.c ${SOS_ROOT}/src/chat/chat.c
    ${SOS_ROOT}/src/shell/shell_core.c ${SOS_ROOT}/src/shell/charwidth.c ${SOS_ROOT}/src/shell/keys.c ${SOS_ROOT}/src/shell/termtext.c)
target_include_directories(test_chat PRIVATE ${SOS_ROOT}/src)
add_test(NAME chat COMMAND test_chat)

# src/files/sdcrypt.c: SD card encryption (header, XTS-AES sectors), with wolfCrypt built for the
# host (tests/wolfcrypt_host/user_settings.h), cross-checked against tools/host/sdcrypt.c, which
# uses OpenSSL's libcrypto. WOLFSSL_ROOT: as for the firmware (lib/wolfssl, or local.cmake).
if (NOT DEFINED WOLFSSL_ROOT)
    include(${SOS_ROOT}/local.cmake OPTIONAL)
endif()
if (NOT DEFINED WOLFSSL_ROOT)
    set(WOLFSSL_ROOT ${SOS_ROOT}/lib/wolfssl)
endif()
find_package(OpenSSL COMPONENTS Crypto)
if (EXISTS ${WOLFSSL_ROOT}/wolfcrypt/src/aes.c)
    add_library(wolfcrypt_host STATIC
        ${WOLFSSL_ROOT}/wolfcrypt/src/aes.c ${WOLFSSL_ROOT}/wolfcrypt/src/memory.c
        ${WOLFSSL_ROOT}/wolfcrypt/src/wc_port.c ${WOLFSSL_ROOT}/wolfcrypt/src/error.c
        ${WOLFSSL_ROOT}/wolfcrypt/src/logging.c ${WOLFSSL_ROOT}/wolfcrypt/src/hmac.c
        ${WOLFSSL_ROOT}/wolfcrypt/src/sha256.c ${WOLFSSL_ROOT}/wolfcrypt/src/hash.c)
    target_compile_definitions(wolfcrypt_host PUBLIC WOLFSSL_USER_SETTINGS)
    target_include_directories(wolfcrypt_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/wolfcrypt_host
        ${WOLFSSL_ROOT})
    target_compile_options(wolfcrypt_host PRIVATE -w)     # third-party code: its warnings aren't ours

    # src/core/keyprov.c and rootsecret.c: keys derived from the root secret (HKDF-SHA256 from
    # wolfCrypt), the provider's lock, and the root secret's flash record. Needs only wolfCrypt.
    add_executable(test_keyprov test_keyprov.c ${SOS_ROOT}/src/core/keyprov.c
        ${SOS_ROOT}/src/core/rootsecret.c)
    target_compile_definitions(test_keyprov PRIVATE KEYPROV_TEST)
    target_include_directories(test_keyprov PRIVATE ${SOS_ROOT}/src)
    target_link_libraries(test_keyprov PRIVATE wolfcrypt_host)
    add_test(NAME keyprov COMMAND test_keyprov)

    # src/core/roototp.c: the root secret's OTP pages, and which source a board uses.
    add_executable(test_roototp test_roototp.c ${SOS_ROOT}/src/core/roototp.c)
    target_include_directories(test_roototp PRIVATE ${SOS_ROOT}/src)
    target_link_libraries(test_roototp PRIVATE wolfcrypt_host)
    add_test(NAME roototp COMMAND test_roototp)

    # src/core/pwverify.c: the console password's verifier (PBKDF2-HMAC-SHA256, then keyed).
    add_executable(test_pwverify test_pwverify.c ${SOS_ROOT}/src/core/pwverify.c)
    target_include_directories(test_pwverify PRIVATE ${SOS_ROOT}/src)
    target_link_libraries(test_pwverify PRIVATE wolfcrypt_host)
    add_test(NAME pwverify COMMAND test_pwverify)
else()
    message(WARNING "sdcrypt and keyprov tests skipped: they need wolfSSL (WOLFSSL_ROOT)")
endif()

if (TARGET wolfcrypt_host AND OpenSSL_FOUND)
    add_executable(test_sdcrypt test_sdcrypt.c ${SOS_ROOT}/src/files/sdcrypt.c
        ${SOS_ROOT}/tools/host/sdcrypt.c)
    target_compile_definitions(test_sdcrypt PRIVATE SDCRYPT_TOOL_NO_MAIN
        SDCRYPT_TOOL_PATH="$<TARGET_FILE:sdcrypt>")
    target_include_directories(test_sdcrypt PRIVATE ${SOS_ROOT}/src)
    target_link_libraries(test_sdcrypt PRIVATE wolfcrypt_host OpenSSL::Crypto)
    add_test(NAME sdcrypt COMMAND test_sdcrypt)
    add_dependencies(test_sdcrypt sdcrypt)

    # src/core/seal.c: sealing stored records (AES-256-GCM, cross-checked against OpenSSL), with
    # the two record formats that seal through it: src/core/config.c, src/ssh/hostkey_record.c.
    add_executable(test_seal test_seal.c ${SOS_ROOT}/src/core/seal.c ${SOS_ROOT}/src/core/config.c
        ${SOS_ROOT}/src/ssh/hostkey_record.c)
    target_include_directories(test_seal PRIVATE ${SOS_ROOT}/src)
    target_link_libraries(test_seal PRIVATE wolfcrypt_host OpenSSL::Crypto)
    add_test(NAME seal COMMAND test_seal)

    # src/ssh/hostkey.c: the host key and its owner, with the real record and seal code and
    # stand-ins for flash, the key provider, the settings and key generation (tests/stubs).
    add_executable(test_hostkey test_hostkey.c ${SOS_ROOT}/src/ssh/hostkey.c
        ${SOS_ROOT}/src/ssh/hostkey_record.c ${SOS_ROOT}/src/core/seal.c)
    target_include_directories(test_hostkey PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${SOS_ROOT}/src)
    target_link_libraries(test_hostkey PRIVATE wolfcrypt_host)
    add_test(NAME hostkey COMMAND test_hostkey)

    add_executable(sdcrypt ${SOS_ROOT}/tools/host/sdcrypt.c)
    target_link_libraries(sdcrypt PRIVATE OpenSSL::Crypto)
elseif (TARGET wolfcrypt_host)
    message(WARNING "sdcrypt and seal tests skipped: they also need OpenSSL's libcrypto")
endif()
