cmake_minimum_required(VERSION 3.13)

# Board/platform must be set before the SDK is imported.
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Target platform")

# Fall back to the shared SDK checkout if PICO_SDK_PATH isn't set in the environment.
if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/slop/pico-sdk")
endif()

include(pico_sdk_import.cmake)

project(sos C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.1.0")
    message(FATAL_ERROR "Pico SDK 2.1.0 or later required (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

# --- Per-machine settings (Wi-Fi, SSH user/key) -----------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL RESULT_VARIABLE SOS_LOCAL_CMAKE)
if (NOT SOS_LOCAL_CMAKE)
    message(FATAL_ERROR "local.cmake not found: copy local.cmake.example to local.cmake and fill it in")
endif()
foreach(var SOS_WIFI_SSID SOS_WIFI_PASSWORD SOS_HOSTNAME SOS_SSH_USER SOS_SSH_AUTHORIZED_KEY)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "${var} must be set in local.cmake")
    endif()
endforeach()
configure_file(config/sos_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/sos_config.h @ONLY)

# --- External libraries (kept outside the repo, under ~/slop by default) ----
set(SOS_DEPS_DIR "$ENV{HOME}/slop" CACHE PATH "Directory holding wolfssl, wolfssh and pico-sshd checkouts")
set(WOLFSSL_ROOT   "${SOS_DEPS_DIR}/wolfssl"   CACHE PATH "wolfSSL source")
set(WOLFSSH_ROOT   "${SOS_DEPS_DIR}/wolfssh"   CACHE PATH "wolfSSH source")
set(PICO_SSHD_ROOT "${SOS_DEPS_DIR}/pico-sshd" CACHE PATH "pico-sshd source")
foreach(dir WOLFSSL_ROOT WOLFSSH_ROOT PICO_SSHD_ROOT)
    if (NOT EXISTS "${${dir}}")
        message(FATAL_ERROR "${dir} not found at ${${dir}}")
    endif()
endforeach()

include(${PICO_SSHD_ROOT}/cmake/wolfssl_pico.cmake)
include(${PICO_SSHD_ROOT}/cmake/wolfssh_pico.cmake)

# pico-sshd's own CMakeLists adds its sources to the wrong target (pico-telnetd-lib),
# so define the library here instead of using add_subdirectory().
add_library(pico-sshd-lib INTERFACE)
target_include_directories(pico-sshd-lib INTERFACE ${PICO_SSHD_ROOT}/include)
target_sources(pico-sshd-lib INTERFACE
    ${PICO_SSHD_ROOT}/src/server.c
    ${PICO_SSHD_ROOT}/src/ringbuffer.c
    ${PICO_SSHD_ROOT}/src/sha512crypt.c
)
target_link_libraries(pico-sshd-lib INTERFACE pico_rand pico_aon_timer wolfssl wolfssh)

# --- SOS firmware ------------------------------------------------------------
set(SOS_SOURCES
    src/main.c
    src/hostkey.c
    src/sshd.c
    src/shell.c
    src/stackmon.c
)
add_executable(sos ${SOS_SOURCES})

# Warnings for our own code only (third-party sources are compiled into this target too).
set_source_files_properties(${SOS_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

target_include_directories(sos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config          # lwipopts.h, user_settings.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated       # sos_config.h
)

# Core 0 stack: lives in its own region at the top of main RAM (config/ld/*.incl) rather than
# the 4K SCRATCH_Y bank, leaving both scratch banks to their SDK roles (SCRATCH_X = core 1
# stack). Measured peak is ~3.2K during an SSH login (`mem` shows it); the rest is headroom
# for TLS and future work. Interrupt handlers, including lwIP/cyw43 processing, use this stack.
set(SOS_CORE0_STACK_SIZE 0x2000 CACHE STRING "Core 0 stack size in bytes")
target_compile_definitions(sos PRIVATE
    PICO_STACK_SIZE=${SOS_CORE0_STACK_SIZE}
    PICO_CORE1_STACK_SIZE=0x1000    # all of SCRATCH_X; the SDK would otherwise reuse PICO_STACK_SIZE
    PICO_USE_STACK_GUARDS=1         # MSPLIM: stack overflow faults instead of corrupting the heap
)
pico_set_linker_script_var(sos SOS_CORE0_STACK_SIZE ${SOS_CORE0_STACK_SIZE})
pico_add_linker_script_override_path(sos ${CMAKE_CURRENT_SOURCE_DIR}/config/ld
    FILES memory_ram.incl sections_stack.incl section_end.incl)

target_link_libraries(sos
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_flash
    hardware_flash
    pico-sshd-lib
)

pico_set_program_name(sos "SOS")
pico_set_program_description(sos "Small/Secure OS")

# Console on USB CDC; UART0 (GP0/GP1) kept as a fallback for use with a debug probe.
# USB stdio also provides the reset interface that lets `picotool load -f` reboot into BOOTSEL.
pico_enable_stdio_usb(sos 1)
pico_enable_stdio_uart(sos 1)

pico_add_extra_outputs(sos)   # .uf2, .hex, .bin, .dis, .map

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
