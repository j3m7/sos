# Copied into SOS from https://github.com/tjko/pico-sshd (cmake/wolfssh_pico.cmake) at commit
# 697e33c65a3a36231e91b0d2d121611110978454; pico-sshd is GPL-3.0-or-later (see THIRD_PARTY.md).
# SOS changes: none.

# wolfssh_pico.cmake
#


#include_directories(${WOLFSSH_ROOT})

### wolfSSH library
    file(GLOB WOLFSSH_SRC
        "${WOLFSSH_ROOT}/src/*.c"
    )
    list(REMOVE_ITEM WOLFSSH_SRC
        "${WOLFSSH_ROOT}/src/misc.c"
    )

    add_library(wolfssh INTERFACE)
    target_include_directories(wolfssh INTERFACE ${WOLFSSH_ROOT})
    target_sources(wolfssh INTERFACE ${WOLFSSH_SRC})

    target_compile_definitions(wolfssh INTERFACE
        WOLFSSH_USER_IO
    )

    target_link_libraries(wolfssh INTERFACE
        pico_stdlib
        pico_rand
    )
### End of wolfSSH

