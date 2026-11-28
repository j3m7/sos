// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// What differs between the boards SOS runs on (docs/design/build-features.md §4.8): the Pico 2 W,
// whose LED hangs off the Wi-Fi chip, and the plain Pico 2, whose LED is GPIO 25. The sensor
// reads that differ are in sensors_board.c.

#ifndef SOS_BOARD_H
#define SOS_BOARD_H

#include <stdbool.h>

// Once, after the Wi-Fi chip is up (when the image has it).
void board_led_init(void);

// The board's LED: the Wi-Fi chip's GPIO on the Pico 2 W (only with SOS_FEATURE_NET, which
// drives the chip), PICO_DEFAULT_LED_PIN where the board has one, nothing otherwise.
void board_led_set(bool on);

#endif
