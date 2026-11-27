// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The names the SD card library's SPI side refers to in the parts of it that SOS doesn't build
// (CMakeLists.txt lists the sources): its SDIO driver and its crash handler. The library's
// headers are included so the compiler checks these against its declarations.

#include "pico/stdlib.h"

#include "SDIO/SdioCard.h"
#include "SDIO/rp2040_sdio.h"
#include "crash.h"
#include "sd_card.h"

// The library's assertions end here (its my_assert_func() calls this after printing). Its own
// version saved the details for a crash report of its own and reset. SOS has one way to stop:
// panic(), which src/core/fault.c records, so the next boot's log says where it came from.
void __attribute__((noreturn)) capture_assert(const char *file, int line, const char *func,
                                              const char *pred) {
    (void)file;
    (void)line;
    (void)func;
    (void)pred;
    panic("SD card library assertion failed");
}

// Called for a card whose interface type is SD_IF_SDIO. SOS's slot is SD_IF_SPI
// (src/files/sdcard.c), so this is never reached; if a card is ever declared as SDIO, say so
// instead of running without a driver.
void sd_sdio_ctor(sd_card_t *sd_card_p) {
    (void)sd_card_p;
    panic("SDIO cards are not built into this firmware");
}

// The library's sd_allocation_unit() calls this for an SDIO card (it returns before the call
// for an SPI one, which can't read the full SD Status). Nothing in SOS calls
// sd_allocation_unit() today, so the linker drops it and never asks for this; the stub is here
// so that the first caller links instead of failing on a name from a file that isn't built.
bool rp2040_sdio_get_sd_status(sd_card_t *sd_card_p, uint8_t response[64]) {
    (void)sd_card_p;
    (void)response;
    return false;
}
