// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/tls/certbundle.c: /certs/roots.pem, a file anyone with the card can write. Output
// buffer the size of the file, as certstore.c gives it.

#include <stdlib.h>

#include "fuzz.h"
#include "tls/certbundle.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *out = malloc(size ? size : 1);
    if (!out)
        return 0;
    certbundle_cert_t certs[CERTBUNDLE_MAX_CERTS];
    const char *why = NULL;
    size_t n = certbundle_parse(data, size, out, size, certs, CERTBUNDLE_MAX_CERTS, &why);
    for (size_t i = 0; i < n; i++)
        if (certs[i].off > size || certs[i].len > size - certs[i].off)
            abort();                                // a certificate outside the output
    free(out);
    return 0;
}
