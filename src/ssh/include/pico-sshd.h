/* pico-sshd.h
   Copyright (C) 2025 Timo Kokkonen <tjko@iki.fi>

   SPDX-License-Identifier: GPL-3.0-or-later

   This file is part of pico-sshd

   pico-sshd is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   pico-sshd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with pico-sshd. If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef PICO_SSHD_H
#define PICO_SSHD_H 1

/* Copied into SOS from https://github.com/tjko/pico-sshd at commit
   697e33c65a3a36231e91b0d2d121611110978454 (see THIRD_PARTY.md).

   Modified for SOS (Copyright (C) 2026 Julio Martinez, GPL-3.0-or-later):
   - Multiple concurrent sessions: per-connection state moved from ssh_server_t into
     ssh_session_t; the server owns an array of sessions.
   - Session API (ssh_session_read/write/disconnect/get_client_ip) replaces the stdio driver.
   - ssh_server_service(): all wolfSSH work runs in thread context.
   - CS_CLOSING state for graceful disconnect; timing statistics.
   - Refused connections get an SSH disconnect message ("too many connections"). */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <lwip/tcp.h>
#include <wolfssh/ssh.h>

#include "pico-sshd/ringbuffer.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_LOGIN_LENGTH 32
#define MAX_SERVER_PKEYS 3


/* Client connection state */
typedef enum ssh_connection_state {
	CS_NONE = 0,    /* slot free */
	CS_ACCEPT,      /* TCP connected, SSH handshake / authentication in progress */
	CS_CONNECT,     /* logged in */
	CS_CLOSING,     /* disconnecting: sending remaining output, then exit status and channel
	                   close (exit_sent), then waiting for the client to close */
} ssh_connection_state_t;

/* SSH Server Private Key */
typedef struct ssh_server_pkey_t {
	const uint8_t *key;
	uint32_t key_len;
	int key_type;
} ssh_server_pkey_t;

typedef struct ssh_server_t ssh_server_t;

/* One client connection. Fields marked (irq) are also written by lwIP callbacks in interrupt
   context; everything else is only touched from thread context (ssh_server_service and the
   ssh_session_* functions). */
typedef struct ssh_session_t {
	ssh_server_t *server;              /* owning server */
	unsigned index;                    /* position in server->sessions */
	struct tcp_pcb *client;            /* lwIP connection (irq: cleared on error) */
	volatile ssh_connection_state_t cstate;
	WOLFSSH *ssh;                      /* wolfSSH session */
	ssh_ringbuffer_t rb_tcp_in;        /* TCP (encrypted) input buffer (irq: filled) */
	ssh_ringbuffer_t rb_in;            /* Decrypted input buffer */
	ssh_ringbuffer_t rb_out;           /* Unencrypted output buffer */
	char login[MAX_LOGIN_LENGTH + 1];  /* Logged in user */
	uint8_t auth_tries;                /* Count of failed authentication attempts */
	uint32_t session_id;               /* unique per login (server->logins at login time) */
	volatile bool work_pending;        /* (irq) input arrived, data sent, or poll tick */
	volatile bool peer_closed;         /* (irq) client closed or connection failed */
	volatile bool setup_pending;       /* (irq) accepted; wolfSSH session not created yet */
	bool eof_received;                 /* client sent channel EOF: end once input is consumed */
	volatile bool exit_sent;           /* CS_CLOSING: exit status and channel close sent (the
	                                      slot may then be reclaimed by a new connection) */
	uint64_t accept_start_us;          /* when the connection was accepted */
	uint64_t closing_deadline_us;      /* force close in CS_CLOSING after this time */
	uint32_t handshake_us;             /* accept to logged-in */
} ssh_session_t;

/* SSH Server Instance */
struct ssh_server_t {
	struct tcp_pcb *listen;            /* lwIP listening socket */
	WOLFSSH_CTX *ssh_ctx;              /* SSH server context */
	ssh_session_t *sessions;           /* session slots */
	unsigned max_sessions;             /* number of slots */

	/* Statistics */
	uint32_t logins;                   /* successful logins */
	uint32_t rejected_busy;            /* connections refused because all slots were in use */
	uint8_t busy_pending;              /* refused connections still being closed (irq only) */
	uint32_t last_handshake_us;        /* accept to logged-in, for the last login */
	uint32_t work_us_max;              /* longest single wolfSSH call */
	uint64_t work_us_total;            /* sum of all wolfSSH call durations */
	uint32_t work_calls;               /* number of wolfSSH calls */

	/* Configuration options... set before calling ssh_server_start() */
	uint16_t port;             /* Listen port (default tcp/22) */
	const char *banner;        /* Login banner string to display when connection starts. */
	ssh_server_pkey_t pkeys[MAX_SERVER_PKEYS];  /* server private key(s) */
	bool auth_enabled;         /* Set false to disable authentication. */
	uint8_t max_auth_tries;    /* Maximum authentication attempts allowed (MaxAuthTries) */

	/* Callback functions (optional) */
	int (*auth_cb)(void* ctx, const byte *login, word32 login_len,
		const byte *auth, word32 auth_len, int auth_type);
	void *auth_cb_ctx;
	void (*log_cb)(int priority, const char *format, ...);
	/* Call back to determine if incoming connection should be allowed */
	int (*allow_connect_cb)(ip_addr_t *src_ip);
};

/* Authentication entry used by default authentication callback routine. */
typedef struct ssh_user_auth_entry_t {
	int type;              /* Entry type: WOLFSSH_USERAUTH_PUBLICKEY */
	const char *username;  /* User name (string) */
	const uint8_t *auth;   /* Public key (binary, SSH wire format) */
	uint32_t auth_len;
} ssh_user_auth_entry_t;


/* Server */
ssh_server_t* ssh_server_new(unsigned max_sessions, size_t rxbuf_size, size_t txbuf_size);
bool ssh_server_start(ssh_server_t *st);
void ssh_server_destroy(ssh_server_t *st);
int ssh_server_add_priv_key(ssh_server_t *st, int type, const uint8_t *key, uint32_t key_len);
const char* ssh_server_connection_state_name(enum ssh_connection_state state);
const char *ssh_server_get_pubkey_hash(const void *pkey, uint32_t pkey_len, char *str_buf, uint32_t str_buf_len);
void ssh_server_log_level(int priority);

/* Run pending SSH protocol work (handshakes, decryption, sending) for all sessions. All wolfSSH
   calls happen here, in thread context, never in lwIP callbacks. Call regularly from the main
   loop; returns quickly when there is nothing to do. The ssh_session_* functions must be
   called from the same context. */
void ssh_server_service(ssh_server_t *st);

/* Session slot by index (0 .. max_sessions-1), or NULL. */
ssh_session_t *ssh_server_session(ssh_server_t *st, unsigned index);

/* Session I/O. Non-blocking; both return -1 unless the session is logged in (CS_CONNECT).
   Read returns the number of bytes copied (0 if none are waiting); write returns the number of
   bytes queued, dropping what does not fit in the output buffer. */
int ssh_session_read(ssh_session_t *ss, char *buf, int length);
int ssh_session_write(ssh_session_t *ss, const char *buf, int length);

/* End a session gracefully (exit status 0, channel close, then wait for the client). */
err_t ssh_session_disconnect(ssh_session_t *ss);

err_t ssh_session_get_client_ip(const ssh_session_t *ss, ip_addr_t *ip, uint16_t *port);


#ifdef __cplusplus
}
#endif

#endif /* PICO_SSHD_H */
