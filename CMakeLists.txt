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
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL)

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
add_subdirectory(${SDCARD_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR}/sdcard)

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

include(cmake/wolfssl_pico.cmake)
include(cmake/wolfssh_pico.cmake)

# --- SSH server (src/ssh/pico-sshd: copied from pico-sshd, see THIRD_PARTY.md) ---
add_library(sos-ssh INTERFACE)
target_include_directories(sos-ssh INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/include)
target_sources(sos-ssh INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/pico-sshd/ringbuffer.c
)
target_link_libraries(sos-ssh INTERFACE pico_rand pico_aon_timer wolfssl wolfssh)

# --- SOS firmware ------------------------------------------------------------
# One directory per subsystem; see src/README.md.
set(SOS_SOURCES
    src/main.c
    src/core/log.c
    src/core/config.c
    src/core/settings.c
    src/core/wdog.c
    src/core/fault.c
    src/core/stackmon.c
    src/shell/shell.c
    src/shell/shell_core.c
    src/shell/termtext.c
    src/net/wifi.c
    src/net/http.c
    src/net/http_msg.c
    src/ssh/sshd.c
    src/ssh/hostkey.c
    src/ssh/sftp_fs.c
    src/files/sdcard.c
    src/files/files.c
    src/files/path.c
    src/time/rtc.c
    src/time/timeconv.c
    src/time/timesync.c
    src/time/nts.c
    src/time/nts_ke.c
    src/time/nts_ntp.c
    src/tls/tls.c
    src/tls/tls_roots.c
    src/tls/certstore.c
    src/tls/certbundle.c
    src/lua/lua_host.c
    src/lua/lua_core1.c
)
add_executable(sos ${SOS_SOURCES}
    src/ssh/sftp_wolf.c         # wolfSSH's wolfsftp.c with SOS file functions (third-party warnings)
    ${LUA_SOURCES}
)

# Warnings for our own code only (third-party sources are compiled into this target too).
set_source_files_properties(${SOS_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

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
pico_add_linker_script_override_path(sos ${CMAKE_CURRENT_SOURCE_DIR}/config/ld
    FILES memory_ram.incl sections_stack.incl section_end.incl)

target_link_libraries(sos
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_flash
    pico_lwip_sntp
    hardware_flash
    hardware_i2c
    hardware_watchdog
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

# `ninja -C build flash` : reboot into BOOTSEL if needed, load, verify, run.
find_program(PICOTOOL_EXE picotool)
if (PICOTOOL_EXE)
    add_custom_target(flash
        COMMAND ${PICOTOOL_EXE} load -f -v -x $<TARGET_FILE:sos>
        DEPENDS sos
        USES_TERMINAL
        COMMENT "Flashing sos via picotool"
    )
endif()
