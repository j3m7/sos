// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Manual page topics (see man_topic.h).

#include "man/man_topic.h"

#include <ctype.h>
#include <string.h>

bool man_topic_ok(char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > MAN_TOPIC_MAX || !isalnum((unsigned char)s[0]))
        return false;
    for (char *p = s; *p; p++) {
        if (isupper((unsigned char)*p))
            *p = (char)tolower((unsigned char)*p);
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return false;
    }
    return true;
}

bool man_topic_of(const char *file, char *topic, size_t cap) {
    size_t n = strlen(file), ext = strlen(MAN_EXT);
    if (n <= ext || n - ext > MAN_TOPIC_MAX || n - ext >= cap)
        return false;
    for (size_t i = 0; i < ext; i++)
        if (tolower((unsigned char)file[n - ext + i]) != MAN_EXT[i])
            return false;
    memcpy(topic, file, n - ext);
    topic[n - ext] = '\0';
    return man_topic_ok(topic);
}
