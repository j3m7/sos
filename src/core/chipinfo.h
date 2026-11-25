#pragma once

#include <stddef.h>

// Which RP2350 this is, for `version`. The revision matters for security: the A2 revision has
// published fault-injection attacks on secure boot and the debug lock, which Raspberry Pi says
// A4 fixes (issue #64).
//
// Two numbers identify it (Pico SDK, pico/platform.h): the revision field of the chip's ID
// register (2 on A2; 3 on both A3 and A4) and the boot ROM's version byte (2 on A2, 3 on A3,
// 4 on A4). Pure C, so it's unit tested on the host; the shell reads the two numbers.

// The revision's name ("A2", "A3", "A4"), or NULL when the two numbers aren't a pair this
// firmware knows.
const char *chipinfo_revision_name(unsigned chip_revision, unsigned rom_version);

// Write the text `version` shows after "chip: " into out, e.g. "RP2350 A4 (revision 3, ROM 4)",
// or "RP2350, unknown revision (revision 5, ROM 7)". Always terminated; cut if out is too small.
void chipinfo_format(char *out, size_t out_size, unsigned chip_revision, unsigned rom_version);
