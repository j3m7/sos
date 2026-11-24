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
   Modified for SOS: session_id field, ssh_server_read()/ssh_server_write(),
   ssh_server_service() (wolfSSH runs in thread context), timing statistics. */

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
	CS_NONE = 0,
	CS_ACCEPT,
	CS_CONNECT,
	CS_CLOSING,     /* SOS: exit status and channel close sent; waiting for the client to close */
} ssh_connection_state_t;

/* SSH Server Private Key */
typedef struct ssh_server_pkey_t {
	const uint8_t *key;
	uint32_t key_len;
	int key_type;
} ssh_server_pkey_t;

/* SSH Server Instance */
typedef struct ssh_server_t {
	struct tcp_pcb *listen;            /* lwIP listenig socket */
	struct tcp_pcb *client;            /* lwIP client connection socket */
	volatile ssh_connection_state_t cstate; /* Current client connection state */
	WOLFSSH_CTX *ssh_ctx;              /* SSH server context */
	WOLFSSH* ssh;                      /* SSH (client) connection context */
	ssh_ringbuffer_t rb_tcp_in;        /* TCP (encrypted) input buffer */
	ssh_ringbuffer_t rb_in;            /* Decrypted input buffer */
	ssh_ringbuffer_t rb_out;           /* Unencrypted output buffer */
	char login[MAX_LOGIN_LENGTH + 1];  /* Currently logged in user */
	uint8_t auth_tries;                /* Count of failed authentication attempts */
	uint32_t session_id;               /* SOS: incremented on every successful login */
	/* SOS: set by lwIP callbacks (interrupt context), consumed by ssh_server_service(). */
	volatile bool work_pending;        /* input arrived, data was sent, or poll tick */
	volatile bool peer_closed;         /* client closed or connection failed */
	uint64_t closing_deadline_us;      /* SOS: force close in CS_CLOSING after this time */
	/* SOS: timing statistics for wolfSSH work (accept/worker calls). */
	uint64_t accept_start_us;          /* when the current connection was accepted */
	uint32_t last_handshake_us;        /* accept to logged-in, for the last login */
	uint32_t work_us_max;              /* longest single wolfSSH call */
	uint64_t work_us_total;            /* sum of all wolfSSH call durations */
	uint32_t work_calls;               /* number of wolfSSH calls */

	/* Configuration options... set before calling ssg_server_start() */
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
} ssh_server_t;

/* Authentication entry used by default authentication callback routine. */
typedef struct ssh_user_auth_entry_t {
	int type;              /* Entry type:
				    WOLFSSH_USERAUTH_PASSWORD,
				    WOLFSSH_USERAUTH_PUBLICKEY
			       */
	const char *username;  /* User name (string) */
	const uint8_t *auth;   /* Password hash (string) or public key (binary) */
	uint32_t auth_len;     /* Needed when type is WOLFSSH_USERAUTH_PUBLICKEY */
} ssh_user_auth_entry_t;


/* ssh_server.c */
ssh_server_t* ssh_server_new(size_t rxbuf_size, size_t txbuf_size);
bool ssh_server_start(ssh_server_t *st, bool stdio);
void ssh_server_destroy(ssh_server_t *st);
bool ssh_server_client_connected(ssh_server_t *st);
err_t ssh_server_get_client_ip(const ssh_server_t *st, ip_addr_t *ip, uint16_t *port);
const char* ssh_server_connection_state_name(enum ssh_connection_state state);
err_t ssh_server_disconnect_client(ssh_server_t *st);
const char *ssh_server_get_pubkey_hash(const void *pkey, uint32_t pkey_len, char *str_buf, uint32_t str_buf_len);
void ssh_server_log_level(int priority);
int ssh_server_add_priv_key(ssh_server_t *st, int type, const uint8_t *key, uint32_t key_len);

/* SOS: direct I/O on the connected session, for use instead of the stdio driver
   (ssh_server_start(st, false)). Both are non-blocking and return -1 when no client is
   connected. Read returns the number of bytes copied (0 if none are waiting); write returns
   the number of bytes queued, dropping what does not fit in the output buffer. */
int ssh_server_read(ssh_server_t *st, char *buf, int length);
int ssh_server_write(ssh_server_t *st, const char *buf, int length);

/* SOS: run pending SSH protocol work (handshake, decryption, sending). All wolfSSH calls happen
   here, in thread context, never in lwIP callbacks. Call regularly from the main loop; returns
   quickly when there is nothing to do. ssh_server_read/write/disconnect_client must be called
   from the same context. */
void ssh_server_service(ssh_server_t *st);


#ifdef __cplusplus
}
#endif

#endif /* PICO_SSHD_H */
