// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// See board.h.

#include "core/board.h"

#include "pico/stdlib.h"
#if SOS_HAVE_NET
#include "pico/cyw43_arch.h"
#endif

void board_led_init(void) {
#if SOS_HAVE_NET && defined(CYW43_WL_GPIO_LED_PIN)
    // The Wi-Fi chip's pin: nothing to set up beyond the chip itself.
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
}

void board_led_set(bool on) {
#if SOS_HAVE_NET && defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;                           // a Pico 2 W built without the Wi-Fi chip: no LED
#endif
}
