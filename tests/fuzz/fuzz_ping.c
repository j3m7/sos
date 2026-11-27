// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/net/ping_msg.c: ICMP packets the network sends back (echo replies and errors quoting
// our request).

#include "fuzz.h"
#include "net/ping_msg.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint16_t id = (uint16_t)(fuzz_byte(&data, &size) << 8 | fuzz_byte(&data, &size));
    uint16_t seq;
    uint8_t ttl;
    ping_parse_reply(data, size, id, &seq, &ttl);
    ping_error_t e;
    if (ping_parse_error(data, size, id, &e)) {
        char text[40];
        ping_error_text(e.type, e.code, text);
    }
    return 0;
}
