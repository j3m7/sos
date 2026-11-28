cmake_minimum_required(VERSION 3.13)

# Board/platform must be set before the SDK is imported.
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Target platform")

# The Pico SDK is located by pico_sdk_import.cmake: PICO_SDK_PATH (environment or -D), or
# PICO_SDK_FETCH_FROM_GIT=ON to download it.
include(pico_sdk_import.cmake)

project(sos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.1.0")
    message(FATAL_ERROR "Pico SDK 2.1.0 or later required (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

# --- Per-machine settings (optional) --------------------------------------------
# local.cmake: dependency paths and build options for this machine. The firmware image holds no
# device configuration: a board is set up on its console ('setup') or with the `provision`
# target below.
#
# The signing key (SOS_SIGN_KEY, see "Signed builds" below) given on the command line wins over
# local.cmake: -DSOS_SIGN_KEY=<key> for this build directory, or -DSOS_SIGN_KEY=none for an
# unsigned image. local.cmake's set() would otherwise hide the command line's value, and a
# build made "with the other key" would silently be signed with the usual one.
if (DEFINED CACHE{SOS_SIGN_KEY})
    set(SOS_SIGN_KEY_FROM_COMMAND_LINE "$CACHE{SOS_SIGN_KEY}")
    set(SOS_SIGN_KEY_ON_COMMAND_LINE TRUE)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL)
if (SOS_SIGN_KEY_ON_COMMAND_LINE)
    set(SOS_SIGN_KEY "${SOS_SIGN_KEY_FROM_COMMAND_LINE}")
endif()

# --- wolfSSL / wolfSSH ---------------------------------------------------------
# Git submodules under lib/ by default. To use checkouts elsewhere, set WOLFSSL_ROOT and
# WOLFSSH_ROOT in local.cmake (or with -D / CMakeUserPresets.json).
# Only define the cache defaults when not already set: with this cmake_minimum_required
# (CMP0126 OLD), creating the cache entry would discard a value set in local.cmake.
if (NOT DEFINED WOLFSSL_ROOT)
    set(WOLFSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/wolfssl" CACHE PATH "wolfSSL source")
endif()
if (NOT DEFINED WOLFSSH_ROOT)
    set(WOLFSSH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/wolfssh" CACHE PATH "wolfSSH source")
endif()
foreach(check "WOLFSSL_ROOT;wolfssl/ssl.h" "WOLFSSH_ROOT;wolfssh/ssh.h")
    list(GET check 0 dir)
    list(GET check 1 header)
    if (NOT EXISTS "${${dir}}/${header}")
        message(FATAL_ERROR "${dir} (${${dir}}) is empty or missing. "
            "Run 'git submodule update --init', or point ${dir} at an existing checkout in local.cmake.")
    endif()
endforeach()

# microSD driver + FatFs (carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico), lib/sdcard by default.
if (NOT DEFINED SDCARD_ROOT)
    set(SDCARD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/sdcard" CACHE PATH "SD card library source")
endif()
if (NOT EXISTS "${SDCARD_ROOT}/src/CMakeLists.txt")
    message(FATAL_ERROR "SDCARD_ROOT (${SDCARD_ROOT}) is empty or missing. "
        "Run 'git submodule update --init', or point SDCARD_ROOT at a checkout in local.cmake.")
endif()
# The library's own CMakeLists.txt builds everything it has. SOS's slot is on SPI (src/files/
# sdcard.c), so only what that needs is listed here: not the SDIO driver and its PIO program
# (about 8 KB, with interrupt handlers that were never used), and not the library's crash
# handler (src/core/fault.c takes the fault vector itself). The few names the SPI side still
# refers to are in src/files/sd_stubs.c. When the library is updated, compare this list with
# its src/CMakeLists.txt.
add_library(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE)
target_compile_definitions(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE
    PICO_MAX_SHARED_IRQ_HANDLERS=8u
)
target_sources(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE
    ${SDCARD_ROOT}/src/ff15/source/ff.c
    ${SDCARD_ROOT}/src/ff15/source/ffsystem.c
    ${SDCARD_ROOT}/src/ff15/source/ffunicode.c
    ${SDCARD_ROOT}/src/sd_driver/dma_interrupts.c
    ${SDCARD_ROOT}/src/sd_driver/sd_card.c
    ${SDCARD_ROOT}/src/sd_driver/sd_timeouts.c
    ${SDCARD_ROOT}/src/sd_driver/SPI/my_spi.c
    ${SDCARD_ROOT}/src/sd_driver/SPI/sd_card_spi.c
    ${SDCARD_ROOT}/src/sd_driver/SPI/sd_spi.c
    ${SDCARD_ROOT}/src/src/crc.c
    ${SDCARD_ROOT}/src/src/f_util.c
    ${SDCARD_ROOT}/src/src/ff_stdio.c
    ${SDCARD_ROOT}/src/src/file_stream.c
    ${SDCARD_ROOT}/src/src/glue.c
    ${SDCARD_ROOT}/src/src/my_debug.c
    ${SDCARD_ROOT}/src/src/my_rtc.c
    ${SDCARD_ROOT}/src/src/util.c
)
# SYSTEM: third-party headers, kept out of SOS's warnings (cmake/warnings.cmake).
target_include_directories(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico SYSTEM INTERFACE
    ${SDCARD_ROOT}/src/ff15/source
    ${SDCARD_ROOT}/src/sd_driver
    ${SDCARD_ROOT}/src/include
)
target_link_libraries(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE
    hardware_dma
    hardware_pio
    hardware_spi
    pico_aon_timer
    pico_stdlib
    cmsis_core
)

# Lua 5.4 (lib/lua by default), run on core 1 by src/lua/lua_core1.c.
if (NOT DEFINED LUA_ROOT)
    set(LUA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/lua" CACHE PATH "Lua source")
endif()
if (NOT EXISTS "${LUA_ROOT}/lua.h")
    message(FATAL_ERROR "LUA_ROOT (${LUA_ROOT}) is empty or missing. "
        "Run 'git submodule update --init', or point LUA_ROOT at a checkout in local.cmake.")
endif()
# The core and the safe libraries only: no io, os, package (loadlib) or debug, and no linit.c
# (src/lua/lua_core1.c opens the libraries itself).
set(LUA_SOURCES)
foreach(f lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser
          lstate lstring ltable ltm lundump lvm lzio
          lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib loadlib)
    list(APPEND LUA_SOURCES ${LUA_ROOT}/${f}.c)
endforeach()
# Lua formats numbers with snprintf("%.14g"); the SDK's lightweight printf (which SOS's own code
# uses) prints doubles with ~10 digits and trailing zeros, so Lua uses the C library's.
set_source_files_properties(${LUA_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/lua/lua_port.h;-Dsnprintf=__real_snprintf")
# sos.json.encode formats numbers itself (src/lua/lua_json.c) and must write what tostring does,
# so it uses the C library's snprintf too.
set_property(SOURCE src/lua/lua_json.c APPEND PROPERTY COMPILE_DEFINITIONS snprintf=__real_snprintf)

include(cmake/wolfssl_pico.cmake)
include(cmake/wolfssh_pico.cmake)
# AES: wolfSSL's Thumb-2 assembly, the inline-assembly version (thumb2-aes-asm_c.c; issue #59),
# about 60% faster than the C code for encrypted SD cards, and its two 1 KB tables stay in flash
# (the XIP cache holds them). Only aes.c and the assembly get WOLFSSL_ARMASM: globally it would
# switch SHA-2, ChaCha, Curve25519 etc. to assembly too. WOLFSSL_ARMASM turns on
# WOLFSSL_USE_ALIGN, which changes struct Aes, so config/user_settings.h defines that for every
# file: with it on aes.c only, the first SSH login corrupted the heap.
set(SOS_AES_ASM_SRC ${WOLFSSL_ROOT}/wolfcrypt/src/port/arm/thumb2-aes-asm_c.c)
set_source_files_properties(${WOLFSSL_ROOT}/wolfcrypt/src/aes.c ${SOS_AES_ASM_SRC} PROPERTIES
    COMPILE_DEFINITIONS "WOLFSSL_ARMASM;WOLFSSL_ARMASM_THUMB2;WOLFSSL_ARMASM_INLINE;WOLFSSL_ARMASM_NO_HW_CRYPTO;WOLFSSL_ARMASM_NO_NEON")
# SHA-256's constants in SRAM (FLASH_QUALIFIER in config/user_settings.h).
set_source_files_properties(${WOLFSSL_ROOT}/wolfcrypt/src/sha256.c
    PROPERTIES COMPILE_DEFINITIONS SOS_WOLFCRYPT_TABLES_IN_RAM)

# --- SSH server (src/ssh/pico-sshd: copied from pico-sshd, see THIRD_PARTY.md) ---
add_library(sos-ssh INTERFACE)
target_include_directories(sos-ssh INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)       # core/heaptag.h
target_sources(sos-ssh INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/ringbuffer.c
)
target_link_libraries(sos-ssh INTERFACE pico_rand pico_aon_timer wolfssl wolfssh)
# The server's log formats go to the log module's vsnprintf as they are (#105): have the
# compiler check each LOG_MSG() against its arguments. (The copied code isn't built with -Wall.)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/server.c
    PROPERTIES COMPILE_OPTIONS "-Wformat")

# --- SOS firmware ------------------------------------------------------------
# One directory per subsystem; see src/README.md.
set(SOS_SOURCES
    src/main.c
    src/core/log.c
    src/core/config.c
    src/core/settings.c
    src/core/keysector.c
    src/core/rootsecret.c
    src/core/rootflash.c
    src/core/roototp.c
    src/core/rootsource.c
    src/core/failcount.c
    src/core/failflash.c
    src/core/flashop.c
    src/core/keyprov.c
    src/core/seal.c
    src/core/pwverify.c
    src/core/random.c
    src/core/wdog.c
    src/core/fault.c
    src/core/stackmon.c
    src/core/heap.c
    src/core/heaptag.c
    src/core/glitchdet.c
    src/core/resetwhy.c
    src/core/resetwhy_board.c
    src/core/history.c
    src/core/history_board.c
    src/core/chipinfo.c
    src/core/sensors.c
    src/core/sensors_board.c
    src/core/health.c
    src/core/health_board.c
    src/core/hwtest.c
    src/core/statecheck.c
    src/core/statecheck_board.c
    src/core/bgcheck.c
    src/core/bgcheck_board.c
    src/core/fullcheck.c
    src/core/fullcheck_board.c
    src/core/memtest.c
    src/core/heapwalk.c
    src/core/selftest.c
    src/core/logfile.c
    src/core/logfile_board.c
    src/core/bootkey.c
    src/core/otpread.c
    src/core/consoleonly.c
    src/core/consoleonly_board.c
    src/shell/shell.c
    src/shell/keys.c
    src/shell/shell_core.c
    src/shell/pipe.c
    src/shell/words.c
    src/shell/cmdcheck.c
    src/shell/complete.c
    src/shell/login.c
    src/shell/termtext.c
    src/shell/charwidth.c
    src/edit/edit.c
    src/edit/editor.c
    src/edit/fsterm.c
    src/edit/pager.c
    src/edit/screen.c
    src/edit/view.c
    src/man/man.c
    src/man/man_topic.c
    src/chat/chat.c
    src/net/wifi.c
    src/net/wifi_pick.c
    src/net/wifi_names.c
    src/net/http.c
    src/net/http_msg.c
    src/net/ping.c
    src/net/ping_msg.c
    src/ssh/sshd.c
    src/ssh/hostkey.c
    src/ssh/hostkey_record.c
    src/ssh/sftp_fs.c
    src/files/sd_stubs.c
    src/files/sdcard.c
    src/files/fsck.c
    src/files/fsck_exfat.c
    src/files/fsck_plan.c
    src/files/fsck_repair.c
    src/files/files.c
    src/files/path.c
    src/files/sums.c
    src/files/sums_parse.c
    src/files/sdcrypt.c
    src/files/cardkey.c
    src/time/rtc.c
    src/time/timeconv.c
    src/time/utc_libc.c
    src/time/timesync.c
    src/time/tzrule.c
    src/time/tzbundle.c
    src/time/tzzone.c
    src/time/nts.c
    src/time/nts_ke.c
    src/time/nts_ntp.c
    src/tls/tls.c
    src/tls/tls_admit.c
    src/tls/tls_roots.c
    src/tls/certstore.c
    src/tls/certbundle.c
    src/lua/lua_host.c
    src/lua/lua_core1.c
    src/lua/lua_codec.c
    src/lua/lua_complete.c
    src/lua/lua_date.c
    src/lua/lua_hash.c
    src/lua/lua_health.c
    src/lua/lua_info.c
    src/lua/lua_net.c
    src/lua/lua_json.c
)
# The commit being built (src/core/version.h), shown in the shell greeting and by `version`.
# Regenerated at every build rather than at configure time, so it follows checkouts and commits.
set(SOS_VERSION_C ${CMAKE_CURRENT_BINARY_DIR}/generated/sos_version.c)
add_custom_target(sos_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${SOS_VERSION_C}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${SOS_VERSION_C}
    COMMENT "Recording the commit hash"
)
add_executable(sos ${SOS_SOURCES}
    ${SOS_VERSION_C}
    src/ssh/sftp_wolf.c         # wolfSSH's wolfsftp.c with SOS file functions (third-party warnings)
    ${LUA_SOURCES}
)
add_dependencies(sos sos_version)
# wolfSSL's Thumb-2 AES (see above), linked last. Where its code lands in flash matters: listed
# before the Lua sources, encrypted SD card reads ran 16% slower (841 vs 1000 KB/s), from code
# competing for the same XIP cache lines. Re-measure with `sd test` after moving it.
target_sources(sos PRIVATE ${SOS_AES_ASM_SRC})

# Warnings for our own code only (third-party sources are compiled into this target too).
# -Wstack-usage: any one function's frame over 1.5K, or unbounded (alloca), against an 8K core 0
# stack shared with interrupt handlers and a 4K core 1 stack. The largest today are ~1K: two
# 512-byte path copies (path_is_protected), a FatFs FIL with its sector buffer (sd test).
include(cmake/warnings.cmake)
set(SOS_FIRMWARE_WARNINGS ${SOS_WARNINGS} -Wstack-usage=1536)
set_source_files_properties(${SOS_SOURCES} PROPERTIES COMPILE_OPTIONS "${SOS_FIRMWARE_WARNINGS}")
# The SDK's stdatomic.h (via wolfSSL) tests #if PICO_RP2040, which isn't defined on RP2350:
# -Wundef flags it in every file that includes it. The SDK only tests it with #if, so 0 means
# what undefined did.
if (PICO_PLATFORM MATCHES "^rp2350")
    target_compile_definitions(sos PRIVATE PICO_RP2040=0)
endif()

target_include_directories(sos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src             # "subsystem/header.h"
    ${LUA_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/config          # lwipopts.h, user_settings.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated       # sos_config.h
)

# Core 0 stack: lives in its own region at the top of main RAM (config/ld/*.incl) rather than
# the 4K SCRATCH_Y bank, leaving both scratch banks to their SDK roles (SCRATCH_X = core 1
# stack). Measured peak is ~3.2K during an SSH login (`mem` shows it); the rest is headroom
# for TLS and future work. Interrupt handlers, including lwIP/cyw43 processing, use this stack.
set(SOS_CORE0_STACK_SIZE 0x2000 CACHE STRING "Core 0 stack size in bytes")
# Concurrent SSH sessions. Each slot preallocates ~23K of buffers (TCP input sized to TCP_WND,
# decrypted input, output); logins need ~25K transiently.
set(SOS_SSH_MAX_SESSIONS 3 CACHE STRING "Maximum concurrent SSH sessions")
# Hardware watchdog: the main loop must come round within this time or the chip resets.
# 0 disables it. RP2350 maximum is 16777 ms. The `ssh` command shows the longest SSH work step.
set(SOS_WATCHDOG_MS 8000 CACHE STRING "Watchdog timeout in ms (0 = off)")
set(SOS_CONSOLE_IDLE_LOCK_MIN 10 CACHE STRING
    "Minutes without a key typed after which an open console locks itself (0 = never)")
# Default NTS server (authenticated time), until one is set on the device with `time server`.
# Its certificate chain must end at a root in src/tls/tls_roots.c.
set(SOS_NTS_SERVER "time.cloudflare.com" CACHE STRING "NTS-KE server host name")
# Plain SNTP server, used only on `time sync sntp` (unauthenticated).
set(SOS_NTP_SERVER "pool.ntp.org" CACHE STRING "SNTP server host name")
# Lower bound for any accepted time. Taken at configure time; SOURCE_DATE_EPOCH overrides it for
# reproducible builds.
string(TIMESTAMP SOS_BUILD_EPOCH "%s" UTC)
target_compile_definitions(sos PRIVATE
    SOS_SSH_MAX_SESSIONS=${SOS_SSH_MAX_SESSIONS}
    SOS_WATCHDOG_MS=${SOS_WATCHDOG_MS}
    SOS_CONSOLE_IDLE_LOCK_MIN=${SOS_CONSOLE_IDLE_LOCK_MIN}
    SOS_NTS_SERVER="${SOS_NTS_SERVER}"
    SOS_NTP_SERVER="${SOS_NTP_SERVER}"
    SOS_BUILD_EPOCH=${SOS_BUILD_EPOCH}LL
    PICO_STACK_SIZE=${SOS_CORE0_STACK_SIZE}
    PICO_CORE1_STACK_SIZE=0x1000    # all of SCRATCH_X; the SDK would otherwise reuse PICO_STACK_SIZE
    PICO_USE_STACK_GUARDS=1         # MSPLIM: stack overflow faults instead of corrupting the heap
    PICO_PANIC_FUNCTION=sos_panic   # src/core/fault.c: record why, reset at once
    PICO_MALLOC_PANIC=0             # a failed malloc returns NULL (the code handles it) instead of
                                    # the SDK default: panic, i.e. halt until the watchdog resets
)
pico_set_linker_script_var(sos SOS_CORE0_STACK_SIZE ${SOS_CORE0_STACK_SIZE})
# The board keeps UTC: localtime_r() and mktime() go to src/time/utc_libc.c, so the C library's
# time zone code and the scanf it parses TZ with stay out of the image (about 10 KB).
target_link_options(sos PRIVATE -Wl,--wrap=localtime_r -Wl,--wrap=mktime)
pico_add_linker_script_override_path(sos ${CMAKE_CURRENT_SOURCE_DIR}/config/ld
    FILES memory_ram.incl sections_stack.incl section_end.incl)

target_link_libraries(sos
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_flash
    pico_unique_id          # board serial, shown by `version`
    pico_lwip_sntp
    hardware_flash
    hardware_i2c
    hardware_watchdog
    hardware_adc            # chip temperature and supply voltage (core/sensors.h)
    sos-ssh
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    pico_multicore          # Lua on core 1 (also makes malloc mutex-protected)
)

pico_set_program_name(sos "SOS")
pico_set_program_description(sos "Small/Secure OS")

# Console on USB CDC; UART0 (GP0/GP1) kept as a fallback for use with a debug probe.
# USB stdio also provides the reset interface that lets `picotool load -f` reboot into BOOTSEL.
pico_enable_stdio_usb(sos 1)
pico_enable_stdio_uart(sos 1)

# Signed builds (issue #64). With SOS_SIGN_KEY set (local.cmake, -D, or the environment) to a
# secp256k1 private key in PEM form, picotool hashes and signs the image after linking, so
# sos.elf, sos.uf2 and `ninja flash` are the signed image. A board without secure boot runs a
# signed image like any other; a board with secure boot runs only images signed with the key
# whose hash is in its OTP memory. Without SOS_SIGN_KEY the image is unsigned, as before.
#
# Every image is hashed, signed or not (pico_hash_binary below; docs/design/health.md §9.7):
# picotool adds a HASH_VALUE to the image's block. The boot ROM checks it at every reset and
# doesn't start an image whose flash no longer matches it, and the `image` health check compares
# the running flash with it.
#
# The key is never in the repository.
#
# No OTP file is produced. The SDK would have picotool write sos.otp.json next to the image:
# the settings `picotool otp load` takes to make a board accept only this key, which are the
# key's hash, the "key valid" flag AND the bit that enables secure boot. Loading that file once
# turns secure boot on for good. A file like that doesn't belong among everyday build outputs,
# so it's switched off below; making it is part of the written procedure for securing a board.
#
# Where the key comes from, first match: the command line (-DSOS_SIGN_KEY=..., kept in this
# build directory's cache until -USOS_SIGN_KEY), local.cmake, the environment. "none" or an
# empty value means: this image is not signed, whatever the others say.
if (NOT DEFINED SOS_SIGN_KEY AND DEFINED ENV{SOS_SIGN_KEY})
    set(SOS_SIGN_KEY "$ENV{SOS_SIGN_KEY}")
endif()
if ("${SOS_SIGN_KEY}" STREQUAL "none" OR "${SOS_SIGN_KEY}" STREQUAL "NONE")
    set(SOS_SIGN_KEY "")
endif()
if (SOS_SIGN_KEY)
    if (NOT EXISTS "${SOS_SIGN_KEY}")
        message(FATAL_ERROR "SOS_SIGN_KEY (${SOS_SIGN_KEY}) doesn't exist. Unset it to build "
            "unsigned, or see docs/development.md for making a key.")
    endif()
    get_filename_component(SOS_SIGN_KEY "${SOS_SIGN_KEY}" ABSOLUTE)
    pico_sign_binary(sos "${SOS_SIGN_KEY}")
    # pico_sign_binary() asks for the OTP file; an empty name makes the SDK leave it out.
    set_target_properties(sos PROPERTIES PICOTOOL_OTP_FILE "")
    message(STATUS "SOS: the image is signed with ${SOS_SIGN_KEY}")
else()
    message(STATUS "SOS: the image is not signed (SOS_SIGN_KEY is not set)")
endif()
pico_hash_binary(sos)

# The burn image (docs/security/design.md, "Writing it"): the one build that can write a
# board's root secret into OTP memory, with `rootsecret burn` on the console. It is flashed for that
# job and replaced by the everyday firmware afterwards. Everyday firmware has no code that
# programs OTP: the two files below are compiled only here.
option(SOS_BURN_IMAGE "Build the image that writes the root secret into OTP (permanent)" OFF)
if (SOS_BURN_IMAGE)
    target_sources(sos PRIVATE src/core/rootburn.c src/core/rootburn_board.c)
    set_source_files_properties(src/core/rootburn.c src/core/rootburn_board.c
        PROPERTIES COMPILE_OPTIONS "${SOS_FIRMWARE_WARNINGS}")
    target_compile_definitions(sos PRIVATE SOS_BURN_IMAGE=1)
    message(WARNING "SOS: this is the BURN IMAGE. It can write one-time-programmable memory. "
                    "Do not leave it on a board.")
endif()

# For trying the console-only behaviour (src/core/consoleonly.h) on a development board without
# writing its OTP marker: -DSOS_FORCE_CONSOLE_ONLY=ON makes this image treat every board as
# console-only. It can only add the restriction; an image without it reads the board's marker.
# The option's old name: stop only if it is ON. CMake's option() left it cached as OFF in every
# build directory configured before #102, and that must not stop a build.
if (SOS_FORCE_SEALED_ONLY)
    message(FATAL_ERROR "SOS_FORCE_SEALED_ONLY was renamed to SOS_FORCE_CONSOLE_ONLY (#91)")
endif()
option(SOS_FORCE_CONSOLE_ONLY "Treat every board as console-only (testing)" OFF)
if (SOS_FORCE_CONSOLE_ONLY)
    target_compile_definitions(sos PRIVATE SOS_FORCE_CONSOLE_ONLY=1)
    message(STATUS "SOS: this image treats every board as console-only (SOS_FORCE_CONSOLE_ONLY)")
endif()

# Test commands (issue #188): -DSOS_TEST_COMMANDS=ON adds `mem hold` and `mem release` on the
# console, which take heap memory and keep it so the out-of-memory paths can be made to happen
# on a board, and `sd fault` (#213), which fails a card write or resets the board before one.
# Off by default; not for a board in use.
option(SOS_TEST_COMMANDS "Add console commands for testing (mem hold, mem release, sd fault)" OFF)
if (SOS_TEST_COMMANDS)
    target_sources(sos PRIVATE src/core/memhold.c src/files/sdfault.c)
    target_compile_definitions(sos PRIVATE SOS_TEST_COMMANDS=1)
    message(STATUS "SOS: this image has the test commands (SOS_TEST_COMMANDS)")
endif()

pico_add_extra_outputs(sos)   # .uf2, .hex, .bin, .dis, .map

# `ninja -C build provision` : write the board's configuration from provision.conf (or
# $SOS_PROVISION_FILE) with tools/provision.sh. `flash` leaves the saved configuration alone.
set(SOS_FLASH_BYTES 4194304)
if (EXISTS "${PICO_SDK_PATH}/src/boards/include/boards/${PICO_BOARD}.h")
    file(STRINGS "${PICO_SDK_PATH}/src/boards/include/boards/${PICO_BOARD}.h" flash_line
         REGEX "define PICO_FLASH_SIZE_BYTES \\(([0-9]+) \\* 1024 \\* 1024\\)")
    if (flash_line MATCHES "\\(([0-9]+) \\* 1024 \\* 1024\\)")
        math(EXPR SOS_FLASH_BYTES "${CMAKE_MATCH_1} * 1024 * 1024")
    endif()
endif()
add_custom_target(provision
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/provision.sh -f ${SOS_FLASH_BYTES}
    USES_TERMINAL
    COMMENT "Writing the board's configuration (tools/provision.sh)"
)

# `ninja -C build flash` : reboot into BOOTSEL if needed, load, verify, run. It goes through
# tools/flash.sh, which first checks the image's signing key against this machine's list of
# accepted keys, if it has one (the flash guard; "Flashing" in docs/development.md).
# `flash-check` does the check and flashes nothing.
find_program(PICOTOOL_EXE picotool)
if (PICOTOOL_EXE)
    add_custom_target(flash
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/flash.sh ${CMAKE_CURRENT_BINARY_DIR}/sos.bin
        DEPENDS sos
        USES_TERMINAL
        COMMENT "Flashing sos via picotool (tools/flash.sh)"
    )
endif()
add_custom_target(flash-check
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/flash.sh -n ${CMAKE_CURRENT_BINARY_DIR}/sos.bin
    DEPENDS sos
    USES_TERMINAL
    COMMENT "Checking the image's signing key (tools/flash.sh -n)"
)
