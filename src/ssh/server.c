/* ssh_server.c
   Copyright (C) 2025-2026 Timo Kokkonen <tjko@iki.fi>

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


/* Copied into SOS from https://github.com/tjko/pico-sshd at commit
   697e33c65a3a36231e91b0d2d121611110978454 (see THIRD_PARTY.md).

   Modified for SOS (Copyright (C) 2026 Julio Martinez, GPL-3.0-or-later):
   - Password authentication removed (sha512_crypt dependency dropped); the default
     authentication callback and the advertised methods are public key only.
   - Default public key check compares key lengths before contents.
   - Multiple concurrent sessions: per-connection state lives in ssh_session_t; lwIP and
     wolfSSH callbacks get the session as context. The stdio driver is removed in favour of
     ssh_session_read()/ssh_session_write().
   - All wolfSSH processing moved out of lwIP callbacks (interrupt context with the
     threadsafe_background arch) into ssh_server_service(), called from the main loop. The
     callbacks only buffer input and flag work; connections are torn down in thread context.
     Logins no longer wait for the 500 ms TCP poll between handshake steps.
   - ssh_server_recv() no longer drops/leaks input when the buffer is full (returns ERR_MEM so
     lwIP redelivers it).
   - Graceful disconnect: ssh_session_disconnect() sends exit status 0 and channel close, then
     waits (CS_CLOSING, at most SSH_CLOSING_TIMEOUT_US) for the client to close the TCP
     connection. A new connection arriving when no slot is free reclaims a closing session.
   - Timing statistics for wolfSSH calls.
   - When every slot is busy, the client gets an SSH_MSG_DISCONNECT with reason "too many
     connections" and a readable description instead of a TCP reset.
   - wolfSSH sessions are created and freed only in thread context (the accept callback no
     longer allocates), and closing sessions are examined under the lwIP lock so a reclaim by
     a new connection cannot race with them.
   - Output flushing handles partial channel sends (one packet per call, bounded by the max
     packet size) instead of resending the same data.
   - Login grace timeout and TCP keepalive, so stalled or vanished clients release their slot.
   - TCP receive flow control: the window reopens as wolfSSH consumes input, and the TCP input
     buffer holds a full window.
   - Channel input is drained on every pass, not only on WS_CHAN_RXD. Client EOF ends the
     session cleanly once its input is consumed; stderr-type extended data is discarded
     instead of ending the session.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssh/ssh.h>

#include "pico-sshd.h"
#include "pico-sshd/log.h"


#define SSH_DEFAULT_PORT 22
#define SSH_CLIENT_POLL_TIME 1

#define LOG_MAX_MSG_LEN 256
#define SSH_CLOSING_TIMEOUT_US (2 * 1000 * 1000)
/* Connections that have not logged in within this time are dropped (like sshd LoginGraceTime),
   so idle or stalled clients cannot hold every session slot. */
#define SSH_LOGIN_GRACE_US (30 * 1000 * 1000)
/* TCP keepalive: probe after this much idle time, give up after COUNT unanswered probes. */
#define SSH_KEEPALIVE_IDLE_MS  60000
#define SSH_KEEPALIVE_INTVL_MS 10000
#define SSH_KEEPALIVE_COUNT    6

#define SSH_PROTO_ID_STR "SSH-2.0-picoSSH wolfSSHv" LIBWOLFSSH_VERSION_STRING "\r\n"

/* Refused ("server busy") connections: at most this many are kept open at once to deliver the
   disconnect message (beyond that they are reset), each for at most SSH_BUSY_POLL_TIME
   tcp_poll intervals (500 ms each). */
#define SSH_BUSY_MAX_PENDING 2
#define SSH_BUSY_POLL_TIME 4
#define SSH_MSG_DISCONNECT_ID 1                     /* RFC 4253 section 11.1 */
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS_CODE 4

/* Time a wolfSSH call and accumulate statistics in the server. */
#define SSH_TIMED(st, call) ({ \
	uint64_t _t0 = time_us_64(); \
	int _r = (call); \
	uint32_t _d = (uint32_t)(time_us_64() - _t0); \
	(st)->work_us_total += _d; \
	if (_d > (st)->work_us_max) (st)->work_us_max = _d; \
	(st)->work_calls++; \
	_r; })

static int global_log_level = LOG_ERR;

static const char *ssh_default_banner = "\r\npico-sshd\r\n\r\n";
static const char* connected_text = "Connected.\r\n";

/* Expects a variable `st` (ssh_server_t *) in scope. */
#ifndef LOG_MSG
#define LOG_MSG(...) { if (st) if (st->log_cb) st->log_cb(__VA_ARGS__); }
#endif



static void ssh_server_log_msg(int priority, const char *format, ...)
{
	va_list ap;
	char *buf;
	char tstamp[32];
	int len;
	uint core = get_core_num();

	if (priority > global_log_level)
		return;

	if (!(buf = malloc(LOG_MAX_MSG_LEN)))
		return;

	va_start(ap, format);
	vsnprintf(buf, LOG_MAX_MSG_LEN, format, ap);
	va_end(ap);

	if ((len = strnlen(buf, LOG_MAX_MSG_LEN - 1)) > 0) {
		/* If string ends with \n, remove it. */
		if (buf[len - 1] == '\n')
			buf[len - 1] = 0;
	}


	uint64_t t = to_us_since_boot(get_absolute_time());
	snprintf(tstamp, sizeof(tstamp), "[%6llu.%06llu][%u]",
		(t / 1000000), (t % 1000000), core);
	printf("%s %s %s\n", tstamp, "pico-sshd:", buf);

	free(buf);
}


static int ssh_server_default_auth_cb(void *ctx, const byte *login, word32 login_len,
				const byte *auth, word32 auth_len, int auth_type)
{
	ssh_user_auth_entry_t *users = (ssh_user_auth_entry_t*)ctx;
	int i = 0;
	int res = WOLFSSH_USERAUTH_INVALID_USER;

	(void)login_len;

	if (!ctx || !login || !auth)
		return WOLFSSH_USERAUTH_FAILURE;

	/* Go through list of authentication entries */
	while (users[i].username) {
		ssh_user_auth_entry_t *u = &users[i++];

		if (!u->auth || auth_type != u->type)
			continue;
		if (strlen(u->username) < 1)
			continue;
		if (strcasecmp((const char*)login, u->username))
			continue;

		if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY) {
			if (auth_len == u->auth_len && !memcmp(auth, u->auth, auth_len)) {
				/* public key matches */
				res = WOLFSSH_USERAUTH_SUCCESS;
				break;
			} else {
				/* public key does not match */
				res = WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
			}
		}
	}

	return res;
}


ssh_server_t* ssh_server_new(unsigned max_sessions, size_t rxbuf_size, size_t txbuf_size)
{
	ssh_server_t *st;

	if (max_sessions < 1)
		return NULL;
	if (!(st = calloc(1, sizeof(ssh_server_t))))
		return NULL;
	if (!(st->sessions = calloc(max_sessions, sizeof(ssh_session_t)))) {
		free(st);
		return NULL;
	}

	st->max_sessions = max_sessions;
	st->log_cb = ssh_server_log_msg;
	st->auth_enabled = true;
	st->auth_cb = ssh_server_default_auth_cb;
	st->port = SSH_DEFAULT_PORT;
	st->banner = ssh_default_banner;
	st->allow_connect_cb = NULL;
	st->max_auth_tries = 6;

	/* rb_tcp_in must hold a full TCP window: lwIP may deliver everything the window allows in
	   one pbuf chain (e.g. after reassembling out-of-order segments), and a chain that can
	   never fit would stall the connection. */
	size_t tcp_in_size = rxbuf_size > TCP_WND ? rxbuf_size : TCP_WND;

	for (unsigned i = 0; i < max_sessions; i++) {
		ssh_session_t *ss = &st->sessions[i];
		ss->server = st;
		ss->index = i;
		ss->cstate = CS_NONE;
		if (ssh_ringbuffer_init(&ss->rb_tcp_in, NULL, tcp_in_size) ||
		    ssh_ringbuffer_init(&ss->rb_in, NULL, rxbuf_size) ||
		    ssh_ringbuffer_init(&ss->rb_out, NULL, txbuf_size)) {
			LOG_MSG(LOG_ERR, "ssh_server_new: out of memory for session %u", i);
			ssh_server_destroy(st);
			return NULL;
		}
	}

	LOG_MSG(LOG_DEBUG, "ssh_server_new(%u,%u,%u): %p",
		max_sessions, rxbuf_size, txbuf_size, st);
	return st;
}


ssh_session_t *ssh_server_session(ssh_server_t *st, unsigned index)
{
	if (!st || index >= st->max_sessions)
		return NULL;
	return &st->sessions[index];
}


/* --- lwIP side (interrupt context): buffer input and flag work, nothing else ------------ */

static err_t ssh_server_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	ssh_session_t *ss = (ssh_session_t*)arg;

	(void)pcb;
	(void)len;
	if (ss)
		ss->work_pending = true;	/* send buffer space freed: retry pending output */
	return ERR_OK;
}


static err_t ssh_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	ssh_session_t *ss = (ssh_session_t*)arg;
	ssh_server_t *st = ss ? ss->server : NULL;
	struct pbuf *buf;
	size_t len;

	if (!ss) {
		if (p)
			pbuf_free(p);
		return ERR_OK;
	}

	if (!p) {
		/* Connection closed by client: tear down in ssh_server_service(). */
		LOG_MSG(LOG_NOTICE, "SSH client closed connection from %s:%u",
			ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port);
		ss->peer_closed = true;
		return ERR_OK;
	}
	if (err != ERR_OK) {
		/* Not expected (lwIP always passes ERR_OK). Returning an error would make lwIP keep
		   the pbuf, so consume it here and treat the connection as failed. */
		LOG_MSG(LOG_WARNING, "ssh_server_recv: error received: %d", err);
		pbuf_free(p);
		ss->peer_closed = true;
		return ERR_OK;
	}

	/* The window is only reopened (tcp_recved) as receive_cb() hands bytes to wolfSSH, and
	   rb_tcp_in holds at least TCP_WND, so the data always fits. Should it not, ERR_MEM makes
	   lwIP keep the pbuf chain and redeliver it later. */
	len = p->tot_len;
	if (len > ssh_ringbuffer_free(&ss->rb_tcp_in)) {
		ss->work_pending = true;
		return ERR_MEM;
	}
	for (buf = p; buf; buf = buf->next)
		ssh_ringbuffer_add(&ss->rb_tcp_in, buf->payload, buf->len, false);
	pbuf_free(p);

	ss->work_pending = true;
	return ERR_OK;
}


static err_t ssh_server_poll(void *arg, struct tcp_pcb *pcb)
{
	ssh_session_t *ss = (ssh_session_t*)arg;

	(void)pcb;
	if (ss)
		ss->work_pending = true;	/* periodic retry (e.g. after WS_WANT_WRITE) */
	return ERR_OK;
}


static void ssh_server_err(void *arg, err_t err)
{
	ssh_session_t *ss = (ssh_session_t*)arg;
	ssh_server_t *st = ss ? ss->server : NULL;

	if (!ss)
		return;

	/* lwIP has already freed the pcb. Forget it here and let ssh_server_service() free the
	   wolfSSH session, which may be in use by thread context right now. */
	ss->client = NULL;
	ss->peer_closed = true;

	if (err == ERR_RST) {
		LOG_MSG(LOG_ERR,"SSH client disconnected: connection reset");
	} else if (err != ERR_ABRT) {
		LOG_MSG(LOG_ERR,"ssh_server_err: client connection error: %d", err);
	}
}


static err_t close_client_connection(struct tcp_pcb *pcb)
{
	err_t err = ERR_OK;

	tcp_arg(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);

	if ((err = tcp_close(pcb)) != ERR_OK) {
		tcp_abort(pcb);
		err = ERR_ABRT;
	}

	return err;
}


/* Tear down a session and free its slot. Thread context only (wolfSSH_free uses the heap);
   takes the lwIP lock (recursive), so callers may already hold it. */
static err_t ssh_close_session(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	err_t err = ERR_OK;

	cyw43_arch_lwip_begin();
	ss->cstate = CS_NONE;
	ss->login[0] = 0;
	ss->work_pending = false;
	ss->peer_closed = false;
	ss->setup_pending = false;
	ss->eof_received = false;
	if (ss->client) {
		err = close_client_connection(ss->client);
		ss->client = NULL;
	}
	if (ss->ssh) {
		wolfSSH_free(ss->ssh);
		ss->ssh = NULL;
	}
	ssh_ringbuffer_flush(&ss->rb_tcp_in);
	ssh_ringbuffer_flush(&ss->rb_in);
	ssh_ringbuffer_flush(&ss->rb_out);
	cyw43_arch_lwip_end();

	LOG_MSG(LOG_DEBUG, "ssh_close_session(%u): %d", ss->index, err);
	return err;
}


static ssh_session_t *find_free_session(ssh_server_t *st)
{
	for (unsigned i = 0; i < st->max_sessions; i++)
		if (st->sessions[i].cstate == CS_NONE)
			return &st->sessions[i];

	/* No free slot: reclaim one that is only waiting for its client to hang up. Such a session
	   has already sent its exit status and channel close, and thread context only touches it
	   under the lwIP lock (ssh_session_service), so the TCP side can be closed here. Its
	   wolfSSH session is left for ssh_session_setup() to free: this runs in interrupt
	   context, where the (unlocked) heap must not be used. */
	for (unsigned i = 0; i < st->max_sessions; i++) {
		ssh_session_t *ss = &st->sessions[i];
		if (ss->cstate == CS_CLOSING) {
			if (ss->client) {
				close_client_connection(ss->client);
				ss->client = NULL;
			}
			ss->cstate = CS_NONE;
			LOG_MSG(LOG_DEBUG, "reclaimed closing session %u", ss->index);
			return ss;
		}
	}
	return NULL;
}


/* --- refusing connections when all slots are busy (interrupt context) ------------------ */

static void put_u32be(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* Build an unencrypted SSH binary packet (RFC 4253 section 6, before key exchange: no MAC,
   block size 8) carrying SSH_MSG_DISCONNECT. Returns its length, or 0 if buf is too small. */
static size_t build_disconnect_packet(uint8_t *buf, size_t size, uint32_t reason,
				const char *desc)
{
	size_t dlen = strlen(desc);
	size_t payload = 1 + 4 + 4 + dlen + 4;          /* msg, reason, description, language */
	size_t pad = 8 - ((4 + 1 + payload) % 8);
	if (pad < 4)
		pad += 8;
	size_t plen = 1 + payload + pad;                 /* padding_length byte + payload + pad */
	uint8_t *p = buf;

	if (4 + plen > size)
		return 0;
	put_u32be(p, plen); p += 4;
	*p++ = pad;
	*p++ = SSH_MSG_DISCONNECT_ID;
	put_u32be(p, reason); p += 4;
	put_u32be(p, dlen); p += 4;
	memcpy(p, desc, dlen); p += dlen;
	put_u32be(p, 0); p += 4;                          /* language tag: empty */
	memset(p, 0, pad); p += pad;
	return p - buf;
}

static err_t busy_release(ssh_server_t *st, struct tcp_pcb *pcb)
{
	err_t res = ERR_OK;

	tcp_arg(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);
	if (tcp_close(pcb) != ERR_OK) {
		tcp_abort(pcb);
		res = ERR_ABRT;
	}
	if (st && st->busy_pending)
		st->busy_pending--;
	return res;
}

static err_t busy_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	(void)err;
	if (!p)		/* client hung up after reading the message */
		return busy_release((ssh_server_t*)arg, pcb);
	/* Discard the client's version string / KEXINIT; closing while data is still arriving
	   would make lwIP send a reset, which can destroy the unread message at the client. */
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static err_t busy_poll(void *arg, struct tcp_pcb *pcb)
{
	return busy_release((ssh_server_t*)arg, pcb);	/* client didn't hang up in time */
}

static void busy_err(void *arg, err_t err)
{
	ssh_server_t *st = (ssh_server_t*)arg;

	(void)err;
	if (st && st->busy_pending)		/* pcb already freed by lwIP */
		st->busy_pending--;
}

/* Tell the client all sessions are in use: send our version string and an SSH disconnect
   message, then close once the client hangs up. Returns ERR_OK if handled; anything else makes
   lwIP reset the connection. */
static err_t ssh_server_reject_busy(ssh_server_t *st, struct tcp_pcb *pcb)
{
	char desc[96];
	uint8_t pkt[160];
	size_t len;

	if (st->busy_pending >= SSH_BUSY_MAX_PENDING)
		return ERR_MEM;

	snprintf(desc, sizeof(desc),
		"Too many connections: all %u SSH sessions are in use, try again later",
		st->max_sessions);
	if (!(len = build_disconnect_packet(pkt, sizeof(pkt),
				SSH_DISCONNECT_TOO_MANY_CONNECTIONS_CODE, desc)))
		return ERR_MEM;

	if (tcp_write(pcb, SSH_PROTO_ID_STR, strlen(SSH_PROTO_ID_STR), TCP_WRITE_FLAG_COPY) != ERR_OK ||
	    tcp_write(pcb, pkt, len, TCP_WRITE_FLAG_COPY) != ERR_OK)
		return ERR_MEM;

	tcp_arg(pcb, st);
	tcp_recv(pcb, busy_recv);
	tcp_err(pcb, busy_err);
	tcp_poll(pcb, busy_poll, SSH_BUSY_POLL_TIME);
	tcp_output(pcb);
	st->busy_pending++;
	return ERR_OK;
}


static err_t ssh_server_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	ssh_server_t *st = (ssh_server_t*)arg;
	ssh_session_t *ss;

	if (!pcb || err != ERR_OK) {
		LOG_MSG(LOG_ERR, "ssh_server_accept: failure: %d", err);
		return ERR_VAL;
	}

	if (st->allow_connect_cb) {
		if (st->allow_connect_cb(&pcb->remote_ip) <= 0) {
			LOG_MSG(LOG_ERR, "SSH connection rejected from %s",
				ip4addr_ntoa(&pcb->remote_ip));
			return ERR_MEM;
		}
	}

	if (!(ss = find_free_session(st))) {
		st->rejected_busy++;
		LOG_MSG(LOG_ERR, "SSH connection from %s:%u rejected: all %u sessions in use",
			ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port, st->max_sessions);
		return ssh_server_reject_busy(st, pcb);
	}

	LOG_MSG(LOG_NOTICE, "SSH accepted connection from %s:%u (session %u)",
		ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port, ss->index + 1);

	/* The wolfSSH session is created by ssh_session_setup() in thread context: wolfSSH_new()
	   allocates, and this callback runs in interrupt context. rb_in and rb_out are reset there
	   too; rb_tcp_in is reset here because input may arrive before the thread gets to it. */
	ssh_ringbuffer_flush(&ss->rb_tcp_in);

	/* Notice clients that vanish without closing (e.g. dropped off Wi-Fi); an idle shell sends
	   nothing, so otherwise the slot would be held forever. */
	ip_set_option(pcb, SOF_KEEPALIVE);
	pcb->keep_idle = SSH_KEEPALIVE_IDLE_MS;
	pcb->keep_intvl = SSH_KEEPALIVE_INTVL_MS;
	pcb->keep_cnt = SSH_KEEPALIVE_COUNT;

	ss->setup_pending = true;
	ss->eof_received = false;
	ss->client = pcb;
	ss->auth_tries = 0;
	ss->login[0] = 0;
	ss->session_id = 0;
	ss->handshake_us = 0;
	ss->accept_start_us = time_us_64();
	ss->peer_closed = false;
	ss->work_pending = true;
	ss->cstate = CS_ACCEPT;
	tcp_arg(pcb, ss);
	tcp_sent(pcb, ssh_server_sent);
	tcp_recv(pcb, ssh_server_recv);
	tcp_poll(pcb, ssh_server_poll, SSH_CLIENT_POLL_TIME);
	tcp_err(pcb, ssh_server_err);

	return ERR_OK;
}


/* --- wolfSSH side (thread context) ----------------------------------------------------- */

static int receive_cb(WOLFSSH* ssh, void* buf, word32 buf_len, void *ctx)
{
	ssh_session_t *ss = ctx;
	size_t size;
	int res;

	(void)ssh;
	if (!ss)
		return WS_CBIO_ERR_GENERAL;

	/* rb_tcp_in is filled by ssh_server_recv() in interrupt context. */
	cyw43_arch_lwip_begin();
	if ((size = ssh_ringbuffer_used(&ss->rb_tcp_in)) < 1) {
		cyw43_arch_lwip_end();
		return ss->peer_closed ? WS_CBIO_ERR_CONN_CLOSE : WS_WANT_READ;
	}
	if (size > buf_len)
		size = buf_len;
	res = ssh_ringbuffer_read(&ss->rb_tcp_in, buf, size);
	if (res == 0 && ss->client)
		tcp_recved(ss->client, (u16_t)size);	/* size <= rb_tcp_in size < 64K */
	cyw43_arch_lwip_end();

	return (res == 0 ? (int)size : 0);
}


static int send_cb(WOLFSSH* ssh, void* buf, word32 buf_len, void *ctx)
{
	ssh_session_t *ss = ctx;
	err_t err;

	(void)ssh;
	if (!ss)
		return WS_CBIO_ERR_GENERAL;

	if (buf_len < 1)
		return 0;

	/* Called from thread context (ssh_server_service): lwIP needs the lock. */
	cyw43_arch_lwip_begin();
	if (!ss->client) {
		cyw43_arch_lwip_end();
		return WS_CBIO_ERR_CONN_CLOSE;
	}
	err = tcp_write(ss->client, buf, buf_len, TCP_WRITE_FLAG_COPY);
	if (err == ERR_OK)
		tcp_output(ss->client);
	cyw43_arch_lwip_end();

	return err == ERR_OK ? (int)buf_len : WS_WANT_WRITE;
}


static int user_auth_types_cb(WOLFSSH *ssh, void *ctx)
{
	ssh_session_t *ss = ctx;
	ssh_server_t *st = ss ? ss->server : NULL;
	int ret = 0;

	if (!ssh || !st)
		return WS_BAD_ARGUMENT;

	if (!st->auth_enabled)
		return WOLFSSH_USERAUTH_NONE;
	if (st->auth_cb)
		ret |= WOLFSSH_USERAUTH_PUBLICKEY;

	return ret;
}


static int user_auth_cb(byte auth_type, WS_UserAuthData* auth_data, void *ctx)
{
	ssh_session_t *ss = ctx;
	ssh_server_t *st = ss ? ss->server : NULL;
	int ret = WOLFSSH_USERAUTH_FAILURE;

	if (!auth_data || !st)
		return ret;
	if (!st->auth_cb)
		return ret;


	if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY) {
		char hash[64];
		ret = st->auth_cb(st->auth_cb_ctx,
				auth_data->username,
				auth_data->usernameSz,
				auth_data->sf.publicKey.publicKey,
				auth_data->sf.publicKey.publicKeySz,
				auth_type);
		ssh_server_get_pubkey_hash(auth_data->sf.publicKey.publicKey,
					auth_data->sf.publicKey.publicKeySz,
					hash, sizeof(hash));
		if (ret == WOLFSSH_USERAUTH_SUCCESS) {
			if (auth_data->sf.publicKey.hasSignature)
				LOG_MSG(LOG_NOTICE, "SSH accepted publickey for %s (SHA256:%s) (session %u)",
					auth_data->username, hash, ss->index + 1);
		} else if (ret == WOLFSSH_USERAUTH_INVALID_USER) {
			LOG_MSG(LOG_INFO, "SSH failed publickey authentication for invalid user"
				" %s (SHA256:%s)", auth_data->username, hash);
		} else {
			LOG_MSG(LOG_INFO, "SSH failed publickey authentication for user"
				" %s (SHA256:%s)", auth_data->username, hash);
		}
	}
	else if (auth_type == WOLFSSH_USERAUTH_NONE && !st->auth_enabled) {
		ret = WOLFSSH_USERAUTH_SUCCESS;
		LOG_MSG(LOG_NOTICE, "SSH unauthenticated connection accepted for user %s",
			auth_data->username);
	}
	else {
		ret = WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
	}


	/* Check for too many failed authentication attempts... */
	if (ret != WOLFSSH_USERAUTH_SUCCESS && ret != WOLFSSH_USERAUTH_INVALID_AUTHTYPE) {
		ss->auth_tries++;
		if (st->max_auth_tries > 0 && ss->auth_tries >= st->max_auth_tries) {
			LOG_MSG(LOG_NOTICE, "SSH too many failed authentication attempts for %s",
				auth_data->username);
			ret = WOLFSSH_USERAUTH_REJECTED;
		}
	}

	return ret;
}


static int channel_req_shell_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
	ssh_session_t *ss = ctx;
	ssh_server_t *st = ss ? ss->server : NULL;
	word32 id = 0;

	wolfSSH_ChannelGetId(channel, &id, WS_CHANNEL_ID_SELF);
	LOG_MSG(LOG_INFO, "SSH opening shell on channel: %u", id);

	return WS_SUCCESS;
}


static int channel_req_subsys_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
	ssh_session_t *ss = ctx;
	ssh_server_t *st = ss ? ss->server : NULL;

	LOG_MSG(LOG_INFO, "SSH subsystem not implemented: %s",
		wolfSSH_ChannelGetSessionCommand(channel));

	return WS_ERROR;
}


static int channel_req_exec_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
	ssh_session_t *ss = ctx;
	ssh_server_t *st = ss ? ss->server : NULL;

	LOG_MSG(LOG_INFO, "SSH exec not implemented: %s",
		wolfSSH_ChannelGetSessionCommand(channel));

	return WS_ERROR;
}


static int ssh_session_flush(ssh_session_t *ss)
{
	void *rbuf;
	int waiting;
	int wcount = 0;
	word32 channel_id = 0;

	if (ss->cstate != CS_CONNECT) {
		ssh_ringbuffer_flush(&ss->rb_out);
		return 0;
	}

	while ((waiting = ssh_ringbuffer_used(&ss->rb_out)) > 0) {
		size_t len = ssh_ringbuffer_peek(&ss->rb_out, &rbuf, waiting);
		if (len < 1)
			break;
		/* Sends at most one channel packet (bounded by the max packet size and the peer's
		   window) and returns how much it took, which may be less than len. */
		int res = wolfSSH_ChannelIdSend(ss->ssh, channel_id, rbuf, len);
		if (res <= 0)
			break;
		ssh_ringbuffer_read(&ss->rb_out, NULL, (size_t)res);
		wcount++;
	}

	return wcount;
}


/* Move channel data from wolfSSH into rb_in (channel 0, the shell) or discard it (any other
   channel, and stderr-type extended data, which a shell has no use for). Reading also lets
   wolfSSH reopen the client's window. */
static void ssh_session_drain_channels(ssh_session_t *ss, word32 channel_id)
{
	uint8_t tmp[64];
	int res;

	if (channel_id != 0) {
		while (wolfSSH_ChannelIdRead(ss->ssh, channel_id, tmp, sizeof(tmp)) > 0)
			;
	}
	while (wolfSSH_ChannelIdReadExt(ss->ssh, 0, tmp, sizeof(tmp)) > 0)
		;

	for (;;) {
		size_t room = ssh_ringbuffer_free(&ss->rb_in);
		if (room < 1) {
			/* Full: the shell drains rb_in every poll, so come back on the next pass rather
			   than waiting for the client to send more (it may be out of window). */
			if (wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) > 0)
				ss->work_pending = true;
			break;
		}
		res = wolfSSH_ChannelIdRead(ss->ssh, 0, tmp, room < sizeof(tmp) ? room : sizeof(tmp));
		if (res <= 0)
			break;
		ssh_ringbuffer_add(&ss->rb_in, tmp, res, false);
	}
}


static void ssh_session_process_input(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, rc;
	word32 channel_id = 0;

	res = SSH_TIMED(st, wolfSSH_worker(ss->ssh, &channel_id));
	if (res < 0) {
		rc = wolfSSH_get_error(ss->ssh);
		switch (rc) {
		case WS_CHAN_RXD:
		case WS_EXTDATA:
		case WS_WANT_READ:
		case WS_WANT_WRITE:
			break;
		case WS_REKEYING:
			LOG_MSG(LOG_INFO, "SSH connection re-keying");
			break;
		case WS_EOF:
			/* The client won't send more (e.g. `echo cmd | ssh host`). Let the shell run
			   what has arrived; ssh_session_service() then ends the session cleanly. */
			LOG_MSG(LOG_DEBUG, "SSH channel EOF from client (session %u)", ss->index + 1);
			if (channel_id == 0)
				ss->eof_received = true;
			break;
		case WS_CHANNEL_CLOSED:
			LOG_MSG(LOG_DEBUG, "Channel closed: %u", channel_id);
			return;
		default:
			LOG_MSG(LOG_NOTICE, "SSH unknown error: %d", rc);
			ssh_close_session(ss);
			return;
		}
	}

	/* Always drain, not only on WS_CHAN_RXD: data left behind when rb_in was full would
	   otherwise wait for the client to send more. */
	ssh_session_drain_channels(ss, channel_id);
}


/* Create the wolfSSH session for a newly accepted connection (deferred from ssh_server_accept,
   which runs in interrupt context). Also frees the session left behind by a reclaimed slot. */
static bool ssh_session_setup(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	WOLFSSH *ssh;

	if (ss->ssh) {
		wolfSSH_free(ss->ssh);
		ss->ssh = NULL;
	}
	ssh_ringbuffer_flush(&ss->rb_in);
	ssh_ringbuffer_flush(&ss->rb_out);

	if (!(ssh = wolfSSH_new(st->ssh_ctx))) {
		LOG_MSG(LOG_ERR, "ssh_session_setup: failed to create SSH session");
		return false;
	}
	wolfSSH_SetUserAuthCtx(ssh, ss);
	wolfSSH_SetUserAuthResultCtx(ssh, ss);
	wolfSSH_SetIOReadCtx(ssh, ss);
	wolfSSH_SetIOWriteCtx(ssh, ss);
	wolfSSH_SetChannelReqCtx(ssh, ss);
	ss->ssh = ssh;
	ss->setup_pending = false;
	return true;
}


static void ssh_session_service(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, err;

	/* Interrupt context may reclaim a CS_CLOSING slot for a new connection at any time, so
	   decide what to do with closed and closing sessions under the lock. Past this block the
	   session is in CS_ACCEPT or CS_CONNECT, which interrupt context never takes over. */
	cyw43_arch_lwip_begin();
	if (ss->cstate == CS_NONE) {
		cyw43_arch_lwip_end();
		return;
	}
	if (ss->peer_closed) {
		ssh_close_session(ss);
		cyw43_arch_lwip_end();
		return;
	}
	if (ss->cstate == CS_CLOSING) {
		/* Waiting for the client to close after our channel close: discard its input and
		   give up after the timeout. */
		if (time_us_64() > ss->closing_deadline_us) {
			ssh_close_session(ss);
		} else if (ss->work_pending) {
			size_t discarded = ssh_ringbuffer_used(&ss->rb_tcp_in);
			ss->work_pending = false;
			ssh_ringbuffer_flush(&ss->rb_tcp_in);
			if (discarded && ss->client)	/* keep the window open for the client's FIN */
				tcp_recved(ss->client, (u16_t)discarded);
		}
		cyw43_arch_lwip_end();
		return;
	}
	cyw43_arch_lwip_end();

	if (ss->cstate == CS_ACCEPT) {
		if (time_us_64() - ss->accept_start_us > SSH_LOGIN_GRACE_US) {
			ip_addr_t ip;
			uint16_t port;
			if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK)
				LOG_MSG(LOG_NOTICE, "SSH login timed out for %s:%u (session %u)",
					ip4addr_ntoa(&ip), port, ss->index + 1);
			ssh_close_session(ss);
			return;
		}
		if (ss->setup_pending && !ssh_session_setup(ss)) {
			ssh_close_session(ss);
			return;
		}
	}

	/* After the client's EOF, end the session once the shell has consumed all input (it reads
	   rb_in after every service pass), sending any output it produced first. */
	if (ss->cstate == CS_CONNECT && ss->eof_received && ssh_ringbuffer_used(&ss->rb_in) == 0 &&
	    wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) <= 0) {
		ssh_session_disconnect(ss);
		return;
	}

	if (!ss->work_pending)
		return;
	ss->work_pending = false;

	if (ss->cstate == CS_ACCEPT) {
		if ((res = SSH_TIMED(st, wolfSSH_accept(ss->ssh))) == WS_SUCCESS) {
			LOG_MSG(LOG_DEBUG, "wolfSSH_accept(): ok");
			ss->cstate = CS_CONNECT;
			ss->session_id = ++st->logins;
			ss->handshake_us = (uint32_t)(time_us_64() - ss->accept_start_us);
			st->last_handshake_us = ss->handshake_us;
			strncpy(ss->login, wolfSSH_GetUsername(ss->ssh), sizeof(ss->login) - 1);
			ss->login[sizeof(ss->login) - 1] = 0;
			ssh_ringbuffer_add(&ss->rb_out, connected_text, strlen(connected_text), true);
		} else {
			err = wolfSSH_get_error(ss->ssh);
			if (err == WS_WANT_READ || err == WS_WANT_WRITE)
				return;
			if (err == WS_USER_AUTH_E) {
				ip_addr_t ip;
				uint16_t port;
				if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK)
					LOG_MSG(LOG_NOTICE, "SSH connection rejected from %s:%u",
						ip4addr_ntoa(&ip), port);
			} else {
				LOG_MSG(LOG_INFO, "wolfSSH_accept(): failed: %d (%d)", err, res);
			}
			ssh_close_session(ss);
			return;
		}
	}
	if (ss->cstate == CS_CONNECT) {
		ssh_session_process_input(ss);
		if (ss->cstate == CS_CONNECT)
			ssh_session_flush(ss);
	}

	/* wolfSSH may process only part of the buffered input per call: come back for the rest. */
	if (ss->cstate != CS_NONE && ssh_ringbuffer_used(&ss->rb_tcp_in) > 0)
		ss->work_pending = true;
}


void ssh_server_service(ssh_server_t *st)
{
	if (!st)
		return;
	for (unsigned i = 0; i < st->max_sessions; i++)
		ssh_session_service(&st->sessions[i]);
}


/* --- server lifecycle ------------------------------------------------------------------ */

static bool ssh_server_open(ssh_server_t *st) {
	struct tcp_pcb *pcb = NULL;
	WOLFSSH_CTX *ctx = NULL;
	int pkey_count = 0;
	err_t err;

	if (!st)
		return false;

	if (!(ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL))) {
		LOG_MSG(LOG_ERR, "ssh_server_open: failed to create SSH context");
		goto panic;
	}

	wolfSSH_CTX_SetBanner(ctx, st->banner);
	wolfSSH_CTX_SetSshProtoIdStr(ctx, SSH_PROTO_ID_STR);
	wolfSSH_SetUserAuth(ctx, user_auth_cb);
	wolfSSH_SetUserAuthTypes(ctx, user_auth_types_cb);
	wolfSSH_SetIORecv(ctx, receive_cb);
	wolfSSH_SetIOSend(ctx, send_cb);
	wolfSSH_CTX_SetChannelReqShellCb(ctx, channel_req_shell_cb);
	wolfSSH_CTX_SetChannelReqSubsysCb(ctx, channel_req_subsys_cb);
	wolfSSH_CTX_SetChannelReqExecCb(ctx, channel_req_exec_cb);

	for (int i = 0; i < MAX_SERVER_PKEYS && st->pkeys[i].key; i++) {
		const ssh_server_pkey_t *pk = &st->pkeys[i];

		if (wolfSSH_CTX_UsePrivateKey_buffer(ctx, pk->key, pk->key_len, pk->key_type) < 0) {
			LOG_MSG(LOG_ERR, "ssh_server_open: failed to load server private key");
			goto panic;
		}
		pkey_count++;
	}
	if (pkey_count < 1) {
		LOG_MSG(LOG_ERR, "ssh_server_open: no private keys defined");
		goto panic;
	}

	if (!(pcb = tcp_new_ip_type(IPADDR_TYPE_ANY))) {
		LOG_MSG(LOG_ERR, "ssh_server_open: failed to create pcb");
		goto panic;
	}

	if ((err = tcp_bind(pcb, NULL, st->port)) != ERR_OK) {
		LOG_MSG(LOG_ERR, "ssh_server_open: cannot bind to port %d: %d", st->port, err);
		goto panic;
	}

	if (!(st->listen = tcp_listen_with_backlog(pcb, st->max_sessions))) {
		LOG_MSG(LOG_ERR, "ssh_server_open: failed to listen on port %d", st->port);
		goto panic;
	}

	st->ssh_ctx = ctx;
	tcp_arg(st->listen, st);
	tcp_accept(st->listen, ssh_server_accept);

	return true;

panic:
	if (pcb)
		tcp_abort(pcb);
	if (ctx)
		wolfSSH_CTX_free(ctx);

	return false;
}


static err_t ssh_server_close(ssh_server_t *st) {
	err_t err = ERR_OK;

	if (!st)
		return ERR_VAL;

	for (unsigned i = 0; i < st->max_sessions; i++)
		if (st->sessions[i].cstate != CS_NONE || st->sessions[i].ssh)
			ssh_close_session(&st->sessions[i]);

	if (st->listen) {
		tcp_arg(st->listen, NULL);
		tcp_accept(st->listen, NULL);
		if ((err = tcp_close(st->listen)) != ERR_OK) {
			LOG_MSG(LOG_NOTICE, "ssh_server_close: failed to close listen pcb: %d",
				err);
			tcp_abort(st->listen);
			err = ERR_ABRT;
		}
		st->listen = NULL;
	}

	if (st->ssh_ctx) {
		wolfSSH_CTX_free(st->ssh_ctx);
		st->ssh_ctx = NULL;
	}

	return err;
}


bool ssh_server_start(ssh_server_t *st)
{
	cyw43_arch_lwip_begin();
	bool res = ssh_server_open(st);
	if (!res)
		ssh_server_close(st);
	cyw43_arch_lwip_end();

	return res;
}


void ssh_server_destroy(ssh_server_t *st)
{
	if (!st)
		return;
	cyw43_arch_lwip_begin();
	ssh_server_close(st);
	cyw43_arch_lwip_end();
	for (unsigned i = 0; i < st->max_sessions; i++) {
		ssh_ringbuffer_destroy(&st->sessions[i].rb_tcp_in);
		ssh_ringbuffer_destroy(&st->sessions[i].rb_in);
		ssh_ringbuffer_destroy(&st->sessions[i].rb_out);
	}
	free(st->sessions);
	free(st);
}


int ssh_server_add_priv_key(ssh_server_t *st, int type, const uint8_t *key, uint32_t key_len)
{
	if (!st || !key || key_len < 1)
		return -1;

	for (int i = 0; i < MAX_SERVER_PKEYS; i++) {
		if (st->pkeys[i].key == NULL) {
			st->pkeys[i].key = key;
			st->pkeys[i].key_len = key_len;
			st->pkeys[i].key_type = type;
			return 0;
		}
	}

	return 1;
}


/* --- session API (thread context) ------------------------------------------------------ */

int ssh_session_write(ssh_session_t *ss, const char *buf, int length)
{
	int written = 0;

	if (!ss || ss->cstate != CS_CONNECT)
		return -1;

	/* rb_out is only used in thread context; send_cb takes the lwIP lock itself. */
	while (written < length && ss->cstate == CS_CONNECT) {
		size_t len = ssh_ringbuffer_free(&ss->rb_out);
		if (len < 1)
			break;	/* output buffer full and not draining: drop the rest */
		if (len > (size_t)(length - written))
			len = length - written;
		ssh_ringbuffer_add(&ss->rb_out, buf + written, len, false);
		written += len;
		ssh_session_flush(ss);
	}

	return written;
}


int ssh_session_read(ssh_session_t *ss, char *buf, int length)
{
	size_t len;

	if (!ss || ss->cstate != CS_CONNECT)
		return -1;
	if (length < 1 || (len = ssh_ringbuffer_used(&ss->rb_in)) < 1)
		return 0;

	if (len > (size_t)length)
		len = length;

	/* rb_in is filled by ssh_session_process_input(), also in thread context. */
	ssh_ringbuffer_read(&ss->rb_in, (uint8_t*)buf, len);

	return len;
}


err_t ssh_session_get_client_ip(const ssh_session_t *ss, ip_addr_t *ip, uint16_t *port)
{
	err_t res = ERR_CONN;

	if (!ss)
		return ERR_ARG;

	/* ss->client can be cleared by ssh_server_err() in interrupt context: check under lock. */
	cyw43_arch_lwip_begin();
	if (ss->cstate != CS_NONE && ss->client) {
		if (ip)
			ip_addr_set(ip, &ss->client->remote_ip);
		if (port)
			*port = ss->client->remote_port;
		res = ERR_OK;
	}
	cyw43_arch_lwip_end();

	return res;
}


err_t ssh_session_disconnect(ssh_session_t *ss)
{
	ssh_server_t *st = ss ? ss->server : NULL;
	ip_addr_t ip;
	uint16_t port;

	if (!ss || ss->cstate == CS_NONE || ss->cstate == CS_CLOSING)
		return ERR_OK;

	if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK)
		LOG_MSG(LOG_NOTICE, "SSH user (%s) disconnected from %s:%u (session %u)",
			ss->login[0] ? ss->login : "<unknown>", ip4addr_ntoa(&ip), port,
			ss->index + 1);

	if (ss->cstate == CS_CONNECT && ss->ssh && !ss->peer_closed) {
		/* Send what the session still has queued, then exit status 0, EOF and channel
		   close. The client answers by closing; ssh_server_service() finishes the job. */
		ssh_session_flush(ss);
		if (wolfSSH_stream_exit(ss->ssh, 0) == WS_SUCCESS) {
			ss->closing_deadline_us = time_us_64() + SSH_CLOSING_TIMEOUT_US;
			ss->cstate = CS_CLOSING;
			return ERR_OK;
		}
	}

	return ssh_close_session(ss);
}


/* --- helpers --------------------------------------------------------------------------- */

const char* ssh_server_connection_state_name(enum ssh_connection_state state)
{
	switch (state) {
	case CS_NONE:
		return "No Connection";
	case CS_ACCEPT:
		return "Accepted";
	case CS_CONNECT:
		return "Connected";
	case CS_CLOSING:
		return "Closing";
	}
	return "Unknown";
}


const char *ssh_server_get_pubkey_hash(const void *pkey, uint32_t pkey_len,
				char *str_buf, uint32_t str_buf_len)
{
	uint8_t hash[WC_SHA256_DIGEST_SIZE];
	word32 out_len = str_buf_len;

	if (!pkey || pkey_len < 1 || !str_buf || str_buf_len < 1)
		return NULL;

	if (wc_Sha256Hash(pkey, pkey_len, hash))
		return NULL;

	if (Base64_Encode_NoNl(hash, sizeof(hash), (byte*)str_buf, &out_len))
		return NULL;

	if (out_len > str_buf_len)
		out_len = str_buf_len;
	str_buf[out_len - 1] = 0;

	return str_buf;
}


void ssh_server_log_level(int priority)
{
	global_log_level = priority;
}

/* eof :-) */
