// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The libraries' functions whose reports phase 6's counters count, by name (docs/design/health.md
// §9.9): the one list both countcheck.c's classifiers and CMakeLists.txt's configure-time guard
// read. The guard fails the build if a name below is no longer a function in the library's
// source, or if sd_card_spi.c's count of DBG_PRINTF( calls changes, so a library update is looked
// at before its reports are counted. Keep one entry per line, in this form: CMake reads the quoted
// names with a regular expression.

#ifndef SOS_COUNTCHECK_NAMES_H
#define SOS_COUNTCHECK_NAMES_H

// The SD card library's sd_driver/SPI/sd_card_spi.c, compiled with USE_DBG_PRINTF=1 alone:
// SD_FN(function, kind, counted during an initialisation, from its error lines (EMSG_PRINTF) rather
// than its debug lines (DBG_PRINTF)). Every other function's lines count nothing (the card's type
// at initialisation, and the callers' repeats of an event counted here); so do send_block's debug
// lines, which repeat sd_wait_ready's timeout.
#define COUNTCHECK_SD_FUNCS(SD_FN) \
    SD_FN("chk_crc16", COUNTCHECK_SD_CRC, true, false) \
    SD_FN("sd_wait_token", COUNTCHECK_SD_TIMEOUT, true, false) \
    SD_FN("sd_wait_ready", COUNTCHECK_SD_TIMEOUT, true, false) \
    SD_FN("in_sd_spi_sectors", COUNTCHECK_SD_CMD, true, false) \
    SD_FN("sd_cmd", COUNTCHECK_SD_CMD, false, false) \
    SD_FN("send_block", COUNTCHECK_SD_WRITE, true, true)

// DBG_PRINTF( calls in sd_card_spi.c (version 3.6.2): the guard compares.
#define COUNTCHECK_SD_DBG_PRINTF_CALLS 51

// The CYW43 driver's lib/cyw43-driver/src/cyw43_ll.c and cyw43_lwip.c: the functions whose
// CYW43_WARN lines are counted, WIFI_FN(function, kind). COUNTCHECK_WIFI_NONE: logged only, the
// event counted by its error return (CYW43_FAIL_FAST_CHECK). Any other function is "other".
#define COUNTCHECK_WIFI_FUNCS(WIFI_FN) \
    WIFI_FN("cyw43_ll_sdpcm_poll_device", COUNTCHECK_WIFI_BUS) \
    WIFI_FN("cyw43_kso_set", COUNTCHECK_WIFI_BUS) \
    WIFI_FN("cyw43_ll_bus_sleep_helper", COUNTCHECK_WIFI_BUS) \
    WIFI_FN("cyw43_netif_output", COUNTCHECK_WIFI_TX) \
    WIFI_FN("cyw43_do_ioctl", COUNTCHECK_WIFI_NONE) \
    WIFI_FN("cyw43_sdpcm_send_common", COUNTCHECK_WIFI_NONE)

#endif
