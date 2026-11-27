// Host-test stand-in for the Pico SDK header (src/ssh/hostkey.c uses nothing from it;
// src/files/files.c uses time_us_64(), which tests/test_files.c implements).
#pragma once

#include <stdint.h>

uint64_t time_us_64(void);
