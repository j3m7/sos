// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// What a manual page's topic may be, and the topic a file in /man holds. Pure C, host tested.

#ifndef SOS_MAN_TOPIC_H
#define SOS_MAN_TOPIC_H

#include <stdbool.h>
#include <stddef.h>

#define MAN_TOPIC_MAX   32              // characters in a topic (a command name)
#define MAN_EXT         ".txt"

// A topic is a command name: letters, digits, '-' and '_', starting with a letter or digit, at
// most MAN_TOPIC_MAX characters; lower-cased here. Nothing else, so it names a file in /man and
// nowhere else.
bool man_topic_ok(char *s);

// The topic of a file in /man, for Tab: "wifi.txt" (any case, as FAT matches it) gives "wifi"
// in `topic` (lower-cased, `cap` bytes). False for anything else: another extension (a page
// still downloading, ".~new"), or a name that isn't a topic.
bool man_topic_of(const char *file, char *topic, size_t cap);

#endif
