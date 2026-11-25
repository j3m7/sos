// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Names the RP2350 revision from the chip's revision field and the boot ROM version. See
// chipinfo.h.

#include "core/chipinfo.h"

#include <stdio.h>

const char *chipinfo_revision_name(unsigned chip_revision, unsigned rom_version) {
    if (chip_revision == 2 && rom_version == 2)
        return "A2";
    if (chip_revision == 3 && rom_version == 3)
        return "A3";
    if (chip_revision == 3 && rom_version == 4)
        return "A4";
    return NULL;
}

void chipinfo_format(char *out, size_t out_size, unsigned chip_revision, unsigned rom_version) {
    const char *name = chipinfo_revision_name(chip_revision, rom_version);
    if (name)
        snprintf(out, out_size, "RP2350 %s (revision %u, ROM %u)", name, chip_revision, rom_version);
    else
        snprintf(out, out_size, "RP2350, unknown revision (revision %u, ROM %u)", chip_revision,
                 rom_version);
}
