// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// wall, chat and who (see chat.h). Depends only on the shell, termtext and the C library, so it
// builds on the host for unit tests.

#include "chat/chat.h"

#include <stdio.h>
#include <string.h>

#include "shell/termtext.h"

enum { MSG_WALL, MSG_CHAT, MSG_JOIN, MSG_LEAVE };

typedef struct {
    uint32_t seq;                       // 0 = empty
    uint32_t from;                      // sender's peer id (never delivered back to it)
    uint8_t kind;
    uint64_t posted_us;
    char stamp[CHAT_STAMP_MAX + 1];
    char name[CHAT_NAME_MAX + 1];       // copied: the sender may be gone when it's delivered
    char text[SHELL_LINE_MAX + 1];
} msg_t;

typedef struct {
    shell_t *sh;                        // NULL = free entry
    uint32_t id;                        // unique per registration
    unsigned slot;
    char name[CHAT_NAME_MAX + 1];
    char from[CHAT_FROM_MAX + 1];
    uint64_t since_us;
    bool in_room;
    uint32_t room_since;                // first message seq of this stay in the room
    uint32_t next;                      // seq of the next message to look at
    uint32_t missed;                    // messages for it the ring overwrote before delivery
    // A wall or chat line waiting for room in the ring (see wait_for_room()).
    uint8_t wait_kind;
    char wait_text[SHELL_LINE_MAX + 1];
} peer_t;

static const chat_hooks_t *hooks;
static peer_t peers[CHAT_MAX_PEERS];
static msg_t ring[CHAT_RING];
static uint32_t head = 1;               // seq of the next message posted
static uint32_t next_id = 1;

void chat_init(const chat_hooks_t *h) {
    hooks = h;
    memset(peers, 0, sizeof(peers));
    memset(ring, 0, sizeof(ring));
    head = 1;
    next_id = 1;
}

static uint64_t now_us(void) {
    return hooks && hooks->now_us ? hooks->now_us() : 0;
}

static peer_t *find(const shell_t *sh) {
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++)
        if (sh && peers[i].sh == sh)
            return &peers[i];
    return NULL;
}

const char *chat_name(const shell_t *sh) {
    peer_t *p = find(sh);
    return p ? p->name : NULL;
}

// Outside text (a key comment, typed text) made safe for every terminal it's shown on, on one
// line, cut to at most `max` bytes without splitting a UTF-8 character.
static void clean(char *out, size_t max, const char *in) {
    char buf[SHELL_LINE_MAX + 8];       // termtext_filter() + termtext_flush() add up to 6
    size_t len = strlen(in);
    if (len > SHELL_LINE_MAX)
        len = SHELL_LINE_MAX;
    termtext_t t = TERMTEXT_INIT;
    size_t n = termtext_filter(&t, (const uint8_t *)in, len, buf);
    n += termtext_flush(&t, buf + n);
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == '\t')
            buf[i] = ' ';
    if (n > max) {
        n = max;
        while (n > 0 && ((unsigned char)buf[n] & 0xc0) == 0x80)    // inside a character
            n--;
    }
    memcpy(out, buf, n);
    out[n] = '\0';
}

// --- the ring ------------------------------------------------------------------------------

static bool for_peer(const msg_t *m, const peer_t *p) {
    if (m->from == p->id)
        return false;
    if (m->kind == MSG_WALL)
        return true;
    return p->in_room && m->seq >= p->room_since;
}

static void post(peer_t *from, int kind, const char *text) {
    msg_t *m = &ring[head % CHAT_RING];
    if (m->seq) {               // overwriting: whoever hadn't got it yet has missed it
        for (unsigned i = 0; i < CHAT_MAX_PEERS; i++) {
            peer_t *p = &peers[i];
            if (p->sh && p->next <= m->seq) {
                if (for_peer(m, p))
                    p->missed++;
                p->next = m->seq + 1;
            }
        }
    }
    m->seq = head++;
    m->from = from->id;
    m->kind = (uint8_t)kind;
    m->posted_us = now_us();
    m->stamp[0] = '\0';
    if (hooks && hooks->stamp)
        hooks->stamp(m->stamp, sizeof(m->stamp));
    strcpy(m->name, from->name);
    clean(m->text, SHELL_LINE_MAX, text ? text : "");
}

// Posting now would overwrite a message that someone hasn't had yet, and that message is recent.
// Then wall and chat wait (as a job, so the sender's further input waits too): a pasted burst
// reaches everyone who keeps up, and a stalled client holds the sender back for at most
// CHAT_WAIT_US.
static bool ring_full(void) {
    const msg_t *m = &ring[head % CHAT_RING];
    if (!hooks || !hooks->now_us)       // no clock to time out with: never wait
        return false;
    if (!m->seq || now_us() - m->posted_us >= CHAT_WAIT_US)
        return false;
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++) {
        const peer_t *p = &peers[i];
        if (p->sh && p->next <= m->seq && for_peer(m, p))
            return true;
    }
    return false;
}

static int format(const msg_t *m, char *buf, size_t len) {
    switch (m->kind) {
    case MSG_WALL:
        return snprintf(buf, len, "[wall %s %s] %s\n", m->stamp, m->name, m->text);
    case MSG_CHAT:
        return snprintf(buf, len, "[%s] %s: %s\n", m->stamp, m->name, m->text);
    case MSG_JOIN:
        return snprintf(buf, len, "-- %s %s joined the chat\n", m->stamp, m->name);
    default:
        return snprintf(buf, len, "-- %s %s left the chat\n", m->stamp, m->name);
    }
}

// Output room needed besides the message: clearing the line, then redrawing the prompt and
// what's typed, with some slack for newline translation.
static int redraw_cost(const shell_t *sh) {
    const char *prompt = sh->answer ? sh->ask_prompt : SHELL_PROMPT;
    return 4 + (int)strlen(prompt ? prompt : "") + (int)sh->line_len + 8;
}

void chat_deliver(shell_t *sh) {
    peer_t *p = find(sh);
    if (!p || (!p->missed && p->next == head))
        return;
    if (shell_raw_mode(sh))     // a full-screen program owns the screen: messages wait
        return;
    char buf[CHAT_STAMP_MAX + CHAT_NAME_MAX + SHELL_LINE_MAX + 32];
    int space = shell_write_space(sh) - redraw_cost(sh);
    bool hidden = false;

    if (p->missed) {
        int n = snprintf(buf, sizeof(buf), "(%lu message%s missed)\n",
                         (unsigned long)p->missed, p->missed == 1 ? "" : "s");
        if (n + 1 > space)
            return;
        shell_hide_input(sh);
        hidden = true;
        shell_write(sh, buf, (size_t)n);
        space -= n + 1;
        p->missed = 0;
    }
    while (p->next != head) {
        const msg_t *m = &ring[p->next % CHAT_RING];
        if (!for_peer(m, p)) {
            p->next++;
            continue;
        }
        int n = format(m, buf, sizeof(buf));
        if (n < 0)
            n = 0;
        if ((size_t)n >= sizeof(buf))
            n = sizeof(buf) - 1;
        if (n + 1 > space)
            break;              // the rest on a later pass
        if (!hidden) {
            shell_hide_input(sh);
            hidden = true;
        }
        shell_write(sh, buf, (size_t)n);
        space -= n + 1;
        p->next++;
    }
    if (hidden)
        shell_show_input(sh);
}

// --- peers ---------------------------------------------------------------------------------

static bool name_taken(const char *name, const peer_t *except) {
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++)
        if (peers[i].sh && &peers[i] != except && !strcmp(peers[i].name, name))
            return true;
    return false;
}

bool chat_register(shell_t *sh, unsigned slot, const char *name, const char *from) {
    peer_t *p = find(sh);
    if (p)
        chat_unregister(sh);
    for (unsigned i = 0; i < CHAT_MAX_PEERS && !p; i++)
        if (!peers[i].sh)
            p = &peers[i];
    if (!p)
        return false;
    memset(p, 0, sizeof(*p));
    p->id = next_id++;
    p->slot = slot;
    char suffix[16];
    int slen = snprintf(suffix, sizeof(suffix), "/%u", slot);
    clean(p->name, CHAT_NAME_MAX - (size_t)slen, name && *name ? name : "?");
    if (name_taken(p->name, NULL))
        strcat(p->name, suffix);
    clean(p->from, CHAT_FROM_MAX, from ? from : "");
    p->since_us = now_us();
    p->next = head;
    p->sh = sh;
    return true;
}

static void leave_room(peer_t *p) {
    if (!p->in_room)
        return;
    p->in_room = false;
    post(p, MSG_LEAVE, NULL);
}

void chat_unregister(shell_t *sh) {
    peer_t *p = find(sh);
    if (!p)
        return;
    leave_room(p);
    p->sh = NULL;
}

// --- commands ------------------------------------------------------------------------------

static void cmd_who_room(shell_t *sh, const peer_t *self) {
    shell_printf(sh, "in the chat: ");
    const char *sep = "";
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++) {
        const peer_t *p = &peers[i];
        if (p->sh && p->in_room) {
            shell_printf(sh, "%s%s%s", sep, p->name, p == self ? " (you)" : "");
            sep = ", ";
        }
    }
    shell_printf(sh, "\n");
}

static void send(shell_t *sh, peer_t *self, int kind, const char *text);

void chat_cmd_wall(shell_t *sh, char *args) {
    peer_t *self = find(sh);
    if (!args) {
        shell_printf(sh, "usage: wall <message>\n");
        return;
    }
    if (!self) {
        shell_printf(sh, "wall: this shell can't send messages\n");
        return;
    }
    unsigned others = 0;
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++)
        if (peers[i].sh && &peers[i] != self)
            others++;
    if (!others) {
        shell_printf(sh, "wall: nobody else is logged in\n");
        return;
    }
    send(sh, self, MSG_WALL, args);
}

static void chat_line(shell_t *sh, char *line, void *arg);

// Tab at the chat prompt, on the last word: the room's commands, and the names of the others in
// the room. Names are already filtered for the terminal (clean()); one with a space can't be
// completed as a word and isn't offered (shell/complete.h).
static bool chat_complete(shell_t *sh, shell_value_fn fn, void *arg) {
    fn("/who", "list the room", arg);
    fn("/quit", "leave the chat", arg);
    for (unsigned i = 0; i < CHAT_MAX_PEERS; i++)
        if (peers[i].sh && peers[i].sh != sh && peers[i].in_room)
            fn(peers[i].name, NULL, arg);
    return false;
}

static void ask_again(shell_t *sh) {
    shell_ask_eof(sh, "chat> ", chat_line, NULL);
    shell_ask_complete_words(sh, chat_complete);
}

static void sent(shell_t *sh, peer_t *self, int kind, const char *text) {
    post(self, kind, text);
    if (kind == MSG_WALL) {
        unsigned others = 0;
        for (unsigned i = 0; i < CHAT_MAX_PEERS; i++)
            if (peers[i].sh && &peers[i] != self)
                others++;
        shell_printf(sh, "sent to %u shell%s\n", others, others == 1 ? "" : "s");
    } else {
        ask_again(sh);
    }
}

// Job: post the waiting message once the ring has room. Ctrl-C (or the session ending) drops it,
// and in the chat also leaves the room.
static bool wait_for_room(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    peer_t *self = find(sh);
    if (!self)
        return false;
    if (cancel) {
        if (self->wait_kind == MSG_WALL) {
            shell_printf(sh, "wall: not sent\n");
        } else if (self->in_room) {
            leave_room(self);
            shell_printf(sh, "(left the chat)\n");
        }
        return false;
    }
    if (ring_full())
        return true;
    sent(sh, self, self->wait_kind, self->wait_text);
    return false;
}

static void send(shell_t *sh, peer_t *self, int kind, const char *text) {
    if (!ring_full()) {
        sent(sh, self, kind, text);
        return;
    }
    self->wait_kind = (uint8_t)kind;
    snprintf(self->wait_text, sizeof(self->wait_text), "%s", text);
    shell_run_job(sh, wait_for_room, NULL);
}

static void chat_line(shell_t *sh, char *line, void *arg) {
    (void)arg;
    peer_t *self = find(sh);
    if (!self || !self->in_room)
        return;
    // Trailing spaces go first, so "/quit " (as Tab completes it) is /quit.
    size_t len = line && line != SHELL_EOF ? strlen(line) : 0;
    while (len > 0 && line[len - 1] == ' ')
        line[--len] = '\0';
    if (!line || line == SHELL_EOF || !strcmp(line, "/quit")) {    // Ctrl-C, Ctrl-D, /quit
        leave_room(self);
        shell_printf(sh, "(left the chat)\n");
        return;
    }
    if (!strcmp(line, "/who")) {
        cmd_who_room(sh, self);
    } else if (line[0] == '/' && line[1] != '/') {
        shell_printf(sh, "chat: /who lists the room; /quit, Ctrl-C or Ctrl-D leaves; "
                         "start a line with // to send a /\n");
    } else if (strspn(line, " ") < len) {
        send(sh, self, MSG_CHAT, line[0] == '/' ? line + 1 : line);
        return;
    }
    ask_again(sh);
}

void chat_cmd_chat(shell_t *sh, char *args) {
    peer_t *self = find(sh);
    if (args) {
        shell_printf(sh, "usage: chat\n");
        return;
    }
    if (!self) {
        shell_printf(sh, "chat: this shell can't join the chat\n");
        return;
    }
    // The last few chat lines still in the ring.
    uint32_t first = head > CHAT_RING ? head - CHAT_RING : 1;
    unsigned shown = 0;
    uint32_t from_seq = head;
    while (from_seq > first && shown < CHAT_HISTORY) {
        from_seq--;
        if (ring[from_seq % CHAT_RING].kind == MSG_CHAT)
            shown++;
    }
    char buf[CHAT_STAMP_MAX + CHAT_NAME_MAX + SHELL_LINE_MAX + 32];
    for (uint32_t s = from_seq; s < head; s++) {
        const msg_t *m = &ring[s % CHAT_RING];
        if (m->seq == s && m->kind == MSG_CHAT) {
            int n = format(m, buf, sizeof(buf));
            if (n > 0)
                shell_write(sh, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
        }
    }
    self->in_room = true;
    self->room_since = head;
    post(self, MSG_JOIN, NULL);
    cmd_who_room(sh, self);
    shell_printf(sh, "(/who lists the room; /quit, Ctrl-C or Ctrl-D leaves)\n");
    ask_again(sh);
}

static void duration(char *buf, size_t len, uint64_t us) {
    uint64_t s = us / 1000000;
    if (s >= 86400)
        snprintf(buf, len, "%llud%02lluh", (unsigned long long)(s / 86400),
                 (unsigned long long)(s / 3600 % 24));
    else if (s >= 3600)
        snprintf(buf, len, "%lluh%02llum", (unsigned long long)(s / 3600),
                 (unsigned long long)(s / 60 % 60));
    else
        snprintf(buf, len, "%llum%02llus", (unsigned long long)(s / 60),
                 (unsigned long long)(s % 60));
}

void chat_cmd_who(shell_t *sh, char *args) {
    (void)args;
    uint64_t now = now_us();
    for (unsigned slot = 0; slot <= 255; slot++) {     // in slot order
        for (unsigned i = 0; i < CHAT_MAX_PEERS; i++) {
            const peer_t *p = &peers[i];
            if (!p->sh || p->slot != slot)
                continue;
            char t[16];
            duration(t, sizeof(t), now - p->since_us);
            shell_printf(sh, "%c %u  %-24s %-24s %-4s %s\n", p->sh == sh ? '*' : ' ', p->slot,
                         p->name, p->from, p->in_room ? "chat" : "", t);
        }
    }
}
