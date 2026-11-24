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

	for (unsigned i = 0; i < max_sessions; i++) {
		ssh_session_t *ss = &st->sessions[i];
		ss->server = st;
		ss->index = i;
		ss->cstate = CS_NONE;
		if (ssh_ringbuffer_init(&ss->rb_tcp_in, NULL, rxbuf_size) ||
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
		/* unknown error... */
		LOG_MSG(LOG_WARNING, "ssh_server_recv: error received: %d", err);
		pbuf_free(p);
		return err;
	}

	/* Take the whole pbuf chain or none of it: if it doesn't fit, ERR_MEM makes lwIP keep it
	   and redeliver once ssh_server_service() has drained the buffer. */
	len = p->tot_len;
	if (len > ssh_ringbuffer_free(&ss->rb_tcp_in)) {
		ss->work_pending = true;
		return ERR_MEM;
	}
	for (buf = p; buf; buf = buf->next)
		ssh_ringbuffer_add(&ss->rb_tcp_in, buf->payload, buf->len, false);
	tcp_recved(pcb, len);
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


/* Tear down a session and free its slot. Takes the lwIP lock (recursive), so it may be called
   from thread context or from lwIP callbacks. */
static err_t ssh_close_session(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	err_t err = ERR_OK;

	cyw43_arch_lwip_begin();
	ss->cstate = CS_NONE;
	ss->login[0] = 0;
	ss->work_pending = false;
	ss->peer_closed = false;
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
	   has already sent its exit status and channel close, and thread context no longer
	   touches it, so it is safe to finish closing it here. */
	for (unsigned i = 0; i < st->max_sessions; i++) {
		if (st->sessions[i].cstate == CS_CLOSING) {
			ssh_close_session(&st->sessions[i]);
			return &st->sessions[i];
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
	WOLFSSH *ssh = NULL;

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

	if (!(ssh = wolfSSH_new(st->ssh_ctx))) {
		LOG_MSG(LOG_ERR, "ssh_server_accept: failed to create SSH session");
		return ERR_MEM;
	}
	wolfSSH_SetUserAuthCtx(ssh, ss);
	wolfSSH_SetUserAuthResultCtx(ssh, ss);
	wolfSSH_SetIOReadCtx(ssh, ss);
	wolfSSH_SetIOWriteCtx(ssh, ss);
	wolfSSH_SetChannelReqCtx(ssh, ss);

	ssh_ringbuffer_flush(&ss->rb_tcp_in);
	ssh_ringbuffer_flush(&ss->rb_in);
	ssh_ringbuffer_flush(&ss->rb_out);

	ss->ssh = ssh;
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
		if (len > 0) {
			int res = wolfSSH_ChannelIdSend(ss->ssh, channel_id, rbuf, len);
			if (res != (int)len)
				break;
			ssh_ringbuffer_read(&ss->rb_out, NULL, len);
			wcount++;
		} else {
			break;
		}
	}

	return wcount;
}


static void ssh_session_process_input(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, rc;
	word32 channel_id = 0;


	res = SSH_TIMED(st, wolfSSH_worker(ss->ssh, &channel_id));
	if (res < 0) {
		rc = wolfSSH_get_error(ss->ssh);
		if (rc == WS_CHAN_RXD) {
			if (channel_id != 0)
				LOG_MSG(LOG_DEBUG,"Data waiting for channel: %u", channel_id);
		}
		else if (rc == WS_CHANNEL_CLOSED) {
			LOG_MSG(LOG_DEBUG, "Channel closed: %d", channel_id);
			return;
		}
		else if (rc == WS_REKEYING) {
			LOG_MSG(LOG_INFO, "SSH connection re-keying");
			return;
		}
		else if (rc == WS_WANT_READ || rc == WS_WANT_WRITE) {
			return;
		} else {
			LOG_MSG(LOG_NOTICE, "SSH unkown error: %d", rc);
			ssh_close_session(ss);
			return;
		}
	}
	else  {
		WOLFSSH_CHANNEL *cur = wolfSSH_ChannelFind(ss->ssh, channel_id,	WS_CHANNEL_ID_SELF);
		if (cur) {
			if (wolfSSH_ChannelGetEof(cur)) {
				LOG_MSG(LOG_NOTICE, "SSH client closed connection");
				ssh_close_session(ss);
			}
		}
		return;
	}


	do {
		uint8_t tmp[64];

		if (channel_id != 0 || ssh_ringbuffer_free(&ss->rb_in) >= sizeof(tmp)) {
			res = wolfSSH_ChannelIdRead(ss->ssh, channel_id, tmp, sizeof(tmp));
			if (res > 0) {
				if (channel_id == 0)
					ssh_ringbuffer_add(&ss->rb_in, tmp, res, false);
			}
		} else {
			res = 0;
		}
	} while (res > 0);
}


static void ssh_session_service(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, err;

	if (ss->cstate == CS_NONE)
		return;

	if (ss->peer_closed) {
		ssh_close_session(ss);
		return;
	}
	if (ss->cstate == CS_CLOSING) {
		/* Waiting for the client to close after our channel close: discard its input and
		   give up after the timeout. */
		if (time_us_64() > ss->closing_deadline_us) {
			ssh_close_session(ss);
		} else if (ss->work_pending) {
			ss->work_pending = false;
			cyw43_arch_lwip_begin();
			ssh_ringbuffer_flush(&ss->rb_tcp_in);
			cyw43_arch_lwip_end();
		}
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
		if (st->sessions[i].cstate != CS_NONE)
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
