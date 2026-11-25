// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Build a board's saved configuration on the host (see tools/provision.sh): the same record
// src/core/config.c writes on the device, made with that code, from a provisioning file.

#ifndef SOS_MKCONFIG_H
#define SOS_MKCONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "core/config.h"

#define MKCONFIG_IMAGE_SIZE (2 * CONFIG_SLOT_SIZE)     // both config slots, as they sit in flash

// Parse a provisioning file (`text`, NUL-terminated; it is modified) into *cfg, starting from
// the defaults. Lines are "name = value"; '#' starts a comment line. Names: wifi_ssid,
// wifi_password, wifi_powersave (on|off), hostname, ssh_user (only older firmware uses it), ssh_key (an authorized_keys line;
// may repeat), nts_server, nts_sync_hours. Returns NULL, or a message (in errbuf) naming the line.
const char *mkconfig_parse(char *text, sos_config_t *cfg, char *errbuf, size_t errlen);

// The flash image for the two config slots: cfg saved in the first, the second erased (0xff), so
// whatever the board had saved before is gone. False if the record doesn't read back.
bool mkconfig_image(const sos_config_t *cfg, uint8_t image[MKCONFIG_IMAGE_SIZE]);

#endif
