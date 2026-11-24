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

add_executable(sos
    src/main.c
)

target_compile_options(sos PRIVATE -Wall -Wextra)

target_link_libraries(sos
    pico_stdlib
    pico_cyw43_arch_none    # needed for the on-board LED, which is wired to the CYW43439
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
