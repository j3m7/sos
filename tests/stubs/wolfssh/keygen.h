// Host-test stand-in for wolfSSH's header: the one function src/ssh/hostkey.c calls, which
// the test provides.
#pragma once
#include <stdint.h>
#define WOLFSSH_ED25519KEY 256
int wolfSSH_MakeEd25519Key(uint8_t *out, uint32_t out_size, uint32_t size);
