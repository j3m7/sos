#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

int main(void) {
    stdio_init_all();

    if (cyw43_arch_init()) {
        printf("sos: cyw43 init failed\n");
        return 1;
    }

    bool led = false;
    for (uint32_t tick = 0;; tick++) {
        led = !led;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        printf("sos: heartbeat %lu\n", (unsigned long)tick);
        sleep_ms(500);
    }
}
