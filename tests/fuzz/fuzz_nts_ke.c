// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/time/nts_ke.c: the NTS-KE server's reply (records inside the TLS stream), parsed as it
// accumulates, so every prefix is parsed too.

#include "fuzz.h"
#include "time/nts_ke.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    for (size_t len = 0; len <= size; len += (len < 64 ? 1 : 61)) {
        nts_ke_result_t res;
        const char *why = NULL;
        if (nts_ke_parse(data, len, &res, &why) != NTS_KE_NEED_MORE)
            break;
    }
    nts_ke_result_t res;
    const char *why = NULL;
    nts_ke_parse(data, size, &res, &why);
    return 0;
}
