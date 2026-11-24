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
   - Direct session I/O (ssh_server_read/ssh_server_write) and a per-login session_id, so a
     session can be served without the global stdio driver.
   - All wolfSSH processing moved out of lwIP callbacks (interrupt context with the
     threadsafe_background arch) into ssh_server_service(), called from the main loop. The
     callbacks only buffer input and flag work; the connection is torn down in thread context.
     Logins no longer wait for the 500 ms TCP poll between handshake steps.
   - ssh_server_recv() no longer drops/leaks input when the buffer is full (returns ERR_MEM so
     lwIP redelivers it).
   - Timing statistics for wolfSSH calls.
   - Graceful disconnect: ssh_server_disconnect_client() sends exit status 0 and channel close,
     then waits (CS_CLOSING, at most SSH_CLOSING_TIMEOUT_US) for the client to close the TCP
     connection, instead of an immediate tcp_close() that could reset the connection and lose
     the session's last output. A new connection arriving while the previous one is closing
     finishes that close instead of being refused.
*/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/cyw43_arch.h"
#include "pico/aon_timer.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssh/ssh.h>

#include "pico-sshd.h"
#include "pico-sshd/log.h"


#define SSH_DEFAULT_PORT 22
#define SSH_SERVER_MAX_CONN 1
#define SSH_CLIENT_POLL_TIME 1

#define LOG_MAX_MSG_LEN 256
#define SSH_CLOSING_TIMEOUT_US (2 * 1000 * 1000)

/* SOS: time a wolfSSH call and accumulate statistics in st. */
#define SSH_TIMED(st, call) ({ \
	uint64_t _t0 = time_us_64(); \
	int _r = (call); \
	uint32_t _d = (uint32_t)(time_us_64() - _t0); \
	(st)->work_us_total += _d; \
	if (_d > (st)->work_us_max) (st)->work_us_max = _d; \
	(st)->work_calls++; \
	_r; })

static int global_log_level = LOG_ERR;

static ssh_server_t *stdio_sshserv = NULL;
static void (*chars_available_callback)(void*) = NULL;
static void *chars_available_param = NULL;

static const char *ssh_default_banner = "\r\npico-sshd\r\n\r\n";
static const char* connected_text = "Connected.\r\n";

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


ssh_server_t* ssh_server_new(size_t rxbuf_size, size_t txbuf_size)
{
	ssh_server_t *st = calloc(1, sizeof(ssh_server_t));

	if (!st)
		return NULL;

	st->cstate = CS_NONE;
	st->log_cb = ssh_server_log_msg;
	st->auth_enabled = true;
	st->auth_cb = ssh_server_default_auth_cb;
	st->port = SSH_DEFAULT_PORT;
	st->banner = ssh_default_banner;
	st->allow_connect_cb = NULL;
	st->max_auth_tries = 6;

	ssh_ringbuffer_init(&st->rb_tcp_in, NULL, rxbuf_size);
	ssh_ringbuffer_init(&st->rb_in, NULL, rxbuf_size);
	ssh_ringbuffer_init(&st->rb_out, NULL, txbuf_size);

	LOG_MSG(LOG_DEBUG, "ssh_server_new(%u,%u): %p",
		rxbuf_size, txbuf_size, st);
	return st;
}


static err_t ssh_server_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	ssh_server_t *st = (ssh_server_t*)arg;

	(void)pcb;
	(void)len;
	if (st)
		st->work_pending = true;	/* send buffer space freed: retry pending output */
	return ERR_OK;
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


static err_t ssh_close_client_connection(ssh_server_t *st)
{
	err_t err = ERR_OK;

	cyw43_arch_lwip_begin();
	st->cstate = CS_NONE;
	st->login[0] = 0;
	st->work_pending = false;
	st->peer_closed = false;
	if (st->client) {
		err = close_client_connection(st->client);
		st->client = NULL;
	}
	if (st->ssh) {
		wolfSSH_free(st->ssh);
		st->ssh = NULL;
	}
	ssh_ringbuffer_flush(&st->rb_tcp_in);
	cyw43_arch_lwip_end();

	LOG_MSG(LOG_DEBUG, "ssh_close_client_connection(%p): %d",
		st, err);
	return err;
}


static err_t ssh_process_input(ssh_server_t *st)
{
	int res, rc;
	word32 channel_id = 0;


	res = SSH_TIMED(st, wolfSSH_worker(st->ssh, &channel_id));
	if (res < 0) {
		rc = wolfSSH_get_error(st->ssh);
		if (rc == WS_CHAN_RXD) {
			if (channel_id != 0)
				LOG_MSG(LOG_DEBUG,"Data waiting for channel: %u", channel_id);

		}
		else if (rc == WS_CHANNEL_CLOSED) {
			LOG_MSG(LOG_DEBUG, "Channel closed: %d", channel_id);
			return ERR_OK;
		}
		else if (rc == WS_REKEYING) {
			LOG_MSG(LOG_INFO, "SSH connection re-keying");
			return ERR_OK;
		}
		else if (rc == WS_WANT_READ || rc == WS_WANT_WRITE) {
			return ERR_OK;
		} else {
			LOG_MSG(LOG_NOTICE, "SSH unkown error: %d", rc);
			ssh_close_client_connection(st);
			return ERR_OK;
		}
	}
	else  {
		WOLFSSH_CHANNEL *cur = wolfSSH_ChannelFind(st->ssh, channel_id,	WS_CHANNEL_ID_SELF);
		if (cur) {
			if (wolfSSH_ChannelGetEof(cur)) {
				LOG_MSG(LOG_NOTICE, "SSH client closed connection");
				ssh_close_client_connection(st);
			}
		}
		return ERR_OK;
	}


	do {
		uint8_t tmp[64];

		if (channel_id != 0 || ssh_ringbuffer_free(&st->rb_in) >= sizeof(tmp)) {
			res = wolfSSH_ChannelIdRead(st->ssh, channel_id, tmp, sizeof(tmp));
			if (res > 0) {
				if (channel_id == 0)
					ssh_ringbuffer_add(&st->rb_in, tmp, res, false);
			}
		} else {
			res = 0;
		}
	} while (res > 0);

	if (ssh_ringbuffer_used(&st->rb_in) > 0) {
		if (st->cstate == CS_CONNECT && chars_available_callback) {
			chars_available_callback(chars_available_param);
		}
	}

	return ERR_OK;
}


static err_t ssh_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	ssh_server_t *st = (ssh_server_t*)arg;
	struct pbuf *buf;
	size_t len;


	if (!p) {
		/* Connection closed by client: tear down in ssh_server_service(). */
		LOG_MSG(LOG_NOTICE, "SSH client closed connection from %s:%u",
			ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port);
		st->peer_closed = true;
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
	if (len > ssh_ringbuffer_free(&st->rb_tcp_in)) {
		st->work_pending = true;
		return ERR_MEM;
	}
	for (buf = p; buf; buf = buf->next)
		ssh_ringbuffer_add(&st->rb_tcp_in, buf->payload, buf->len, false);
	tcp_recved(pcb, len);
	pbuf_free(p);

	st->work_pending = true;
	return ERR_OK;
}


static int ssh_server_flush_buffer(ssh_server_t *st)
{
	void *rbuf;
	int waiting;
	int wcount = 0;
	word32 channel_id = 0;


	if (!st)
		return -1;
	if (st->cstate != CS_CONNECT) {
		ssh_ringbuffer_flush(&st->rb_out);
		return 0;
	}

	while ((waiting = ssh_ringbuffer_used(&st->rb_out)) > 0) {
		size_t len = ssh_ringbuffer_peek(&st->rb_out, &rbuf, waiting);
		if (len > 0) {
			int res = wolfSSH_ChannelIdSend(st->ssh, channel_id, rbuf, len);
			if (res != len)
				break;
			ssh_ringbuffer_read(&st->rb_out, NULL, len);
			wcount++;
		} else {
			break;
		}
	}

	return wcount;
}



static err_t ssh_server_poll(void *arg, struct tcp_pcb *pcb)
{
	ssh_server_t *st = (ssh_server_t*)arg;

	(void)pcb;
	if (st)
		st->work_pending = true;	/* periodic retry (e.g. after WS_WANT_WRITE) */
	return ERR_OK;
}


void ssh_server_service(ssh_server_t *st)
{
	int res, err;

	if (!st || st->cstate == CS_NONE)
		return;

	if (st->peer_closed) {
		ssh_close_client_connection(st);
		return;
	}
	if (st->cstate == CS_CLOSING) {
		/* Waiting for the client to close after our channel close: discard its input and
		   give up after the timeout. */
		if (time_us_64() > st->closing_deadline_us) {
			ssh_close_client_connection(st);
		} else if (st->work_pending) {
			st->work_pending = false;
			cyw43_arch_lwip_begin();
			ssh_ringbuffer_flush(&st->rb_tcp_in);
			cyw43_arch_lwip_end();
		}
		return;
	}
	if (!st->work_pending)
		return;
	st->work_pending = false;

	if (st->cstate == CS_ACCEPT) {
		if ((res = SSH_TIMED(st, wolfSSH_accept(st->ssh))) == WS_SUCCESS) {
			LOG_MSG(LOG_DEBUG, "wolfSSH_accept(): ok");
			st->cstate = CS_CONNECT;
			st->session_id++;
			st->last_handshake_us = (uint32_t)(time_us_64() - st->accept_start_us);
			strncpy(st->login, wolfSSH_GetUsername(st->ssh), sizeof(st->login) - 1);
			st->login[sizeof(st->login) - 1] = 0;
			ssh_ringbuffer_add(&st->rb_out, connected_text, strlen(connected_text), true);
		} else {
			err = wolfSSH_get_error(st->ssh);
			if (err == WS_WANT_READ || err == WS_WANT_WRITE)
				return;
			if (err == WS_USER_AUTH_E) {
				ip_addr_t ip;
				uint16_t port;
				if (ssh_server_get_client_ip(st, &ip, &port) == ERR_OK)
					LOG_MSG(LOG_NOTICE, "SSH connection rejected from %s:%u",
						ip4addr_ntoa(&ip), port);
			} else {
				LOG_MSG(LOG_INFO, "wolfSSH_accept(): failed: %d (%d)", err, res);
			}
			ssh_close_client_connection(st);
			return;
		}
	}
	if (st->cstate == CS_CONNECT) {
		ssh_process_input(st);
		ssh_server_flush_buffer(st);
	}

	/* wolfSSH may process only part of the buffered input per call: come back for the rest. */
	if (st->cstate != CS_NONE && ssh_ringbuffer_used(&st->rb_tcp_in) > 0)
		st->work_pending = true;
}


static void ssh_server_err(void *arg, err_t err)
{
	ssh_server_t *st = (ssh_server_t*)arg;

	if (!st)
		return;

	/* lwIP has already freed the pcb. Forget it here and let ssh_server_service() free the
	   wolfSSH session, which may be in use by thread context right now. */
	st->client = NULL;
	st->peer_closed = true;

	if (err == ERR_RST) {
		LOG_MSG(LOG_ERR,"SSH client disconnected: connection reset");
	} else if (err != ERR_ABRT) {
		LOG_MSG(LOG_ERR,"ssh_server_err: client connection error: %d", err);
	}
}


static err_t ssh_server_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	ssh_server_t *st = (ssh_server_t*)arg;
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

	LOG_MSG(LOG_NOTICE, "SSH accepted connection from %s:%u",
		ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port);

	/* A session in CS_CLOSING has already sent its exit status and channel close and is
	   only waiting for its client to hang up; thread context no longer touches it, so it is
	   safe to finish closing it here and serve the new connection. */
	if (st->cstate == CS_CLOSING)
		ssh_close_client_connection(st);

	if (st->cstate != CS_NONE) {
		LOG_MSG(LOG_ERR, "SSH connection rejected: too many connections");
		return ERR_MEM;
	}

	if (!(ssh = wolfSSH_new(st->ssh_ctx))) {
		LOG_MSG(LOG_ERR, "ssh_server_accept: failed to create SSH session");
		return ERR_MEM;
	}
	wolfSSH_SetUserAuthCtx(ssh, st);
	wolfSSH_SetUserAuthResultCtx(ssh, st);
	wolfSSH_SetIOReadCtx(ssh, st);
	wolfSSH_SetIOWriteCtx(ssh, st);
	wolfSSH_SetChannelReqCtx(ssh, st);

	st->ssh = ssh;
	st->client = pcb;
	st->auth_tries = 0;
	st->login[0] = 0;
	st->accept_start_us = time_us_64();
	st->peer_closed = false;
	st->work_pending = true;
	st->cstate = CS_ACCEPT;
	tcp_arg(pcb, st);
	tcp_sent(pcb, ssh_server_sent);
	tcp_recv(pcb, ssh_server_recv);
	tcp_poll(pcb, ssh_server_poll, SSH_CLIENT_POLL_TIME);
	tcp_err(pcb, ssh_server_err);

	ssh_ringbuffer_flush(&st->rb_tcp_in);
	ssh_ringbuffer_flush(&st->rb_in);
	ssh_ringbuffer_flush(&st->rb_out);

	return ERR_OK;
}


static int receive_cb(WOLFSSH* ssh, void* buf, word32 buf_len, void *ctx)
{
	ssh_server_t *st = ctx;
	size_t size;
	int res;


	if (!ctx)
		return WS_CBIO_ERR_GENERAL;

	/* rb_tcp_in is filled by ssh_server_recv() in interrupt context. */
	cyw43_arch_lwip_begin();
	if ((size = ssh_ringbuffer_used(&st->rb_tcp_in)) < 1) {
		cyw43_arch_lwip_end();
		return st->peer_closed ? WS_CBIO_ERR_CONN_CLOSE : WS_WANT_READ;
	}
	if (size > buf_len)
		size = buf_len;
	res = ssh_ringbuffer_read(&st->rb_tcp_in, buf, size);
	cyw43_arch_lwip_end();

	return (res == 0 ? size : 0);
}


static int send_cb(WOLFSSH* ssh, void* buf, word32 buf_len, void *ctx)
{
	struct ssh_server_t *st = ctx;
	err_t err;


	if (!ctx)
		return WS_CBIO_ERR_GENERAL;

	if (buf_len < 1)
		return 0;

	/* Called from thread context (ssh_server_service): lwIP needs the lock. */
	cyw43_arch_lwip_begin();
	if (!st->client) {
		cyw43_arch_lwip_end();
		return WS_CBIO_ERR_CONN_CLOSE;
	}
	err = tcp_write(st->client, buf, buf_len, TCP_WRITE_FLAG_COPY);
	if (err == ERR_OK)
		tcp_output(st->client);
	cyw43_arch_lwip_end();

	return err == ERR_OK ? (int)buf_len : WS_WANT_WRITE;
}


static int user_auth_types_cb(WOLFSSH *ssh, void *ctx)
{
	struct ssh_server_t *st = ctx;
	int ret = 0;

	if (!ssh || !st)
		return WS_BAD_ARGUMENT;

	if (!st->auth_enabled)
		return WOLFSSH_USERAUTH_NONE;
	if (st->auth_cb)
		ret |= WOLFSSH_USERAUTH_PUBLICKEY;

	return ret;
}

#if 0
static int user_auth_result_cb(byte res, WS_UserAuthData* auth_data, void* ctx)
{
	ssh_server_t *st = ctx;
	char tmpbuf[64];

	if (st) {
		if (auth_data->type == WOLFSSH_USERAUTH_PUBLICKEY) {
			ssh_server_get_pubkey_hash(auth_data->sf.publicKey.publicKey,
						auth_data->sf.publicKey.publicKeySz,
						tmpbuf, sizeof(tmpbuf));
			if (res == WOLFSSH_USERAUTH_SUCCESS) {
				LOG_MSG(LOG_NOTICE, "SSH accepted publickey for %s (SHA256:%s)",
					auth_data->username, tmpbuf);
			} else {
				LOG_MSG(LOG_NOTICE, "SSH failed publickey authentication for %s",
					auth_data->username);
			}
		} else {
			LOG_MSG(LOG_NOTICE, "user_auth_result_cb: unhandled auth type");
		}
	}

	return WS_SUCCESS;
}
#endif

static int user_auth_cb(byte auth_type, WS_UserAuthData* auth_data, void *ctx)
{
        ssh_server_t *st = ctx;
	int ret = WOLFSSH_USERAUTH_FAILURE;

	if (!auth_data || !st || !ctx)
		return ret;
	if (!st->auth_cb)
		return ret;


	if (auth_type == WOLFSSH_USERAUTH_PASSWORD) {
		ret = st->auth_cb(st->auth_cb_ctx,
				auth_data->username,
				auth_data->usernameSz,
				auth_data->sf.password.password,
				auth_data->sf.password.passwordSz,
				auth_type);
		if (ret == WOLFSSH_USERAUTH_SUCCESS) {
			LOG_MSG(LOG_NOTICE, "SSH accepted password for user %s", auth_data->username);
		} else if (ret == WOLFSSH_USERAUTH_INVALID_USER) {
			LOG_MSG(LOG_NOTICE, "SSH failed password for invalid user %s",
				auth_data->username);
		} else {
			LOG_MSG(LOG_NOTICE, "SSH failed password for user %s",
				auth_data->username);
		}
	}
	else if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY) {
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
				LOG_MSG(LOG_NOTICE, "SSH accepted publickey for %s (SHA256:%s)",
					auth_data->username, hash);
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
		st->auth_tries++;
		if (st->max_auth_tries > 0 && st->auth_tries >= st->max_auth_tries) {
			LOG_MSG(LOG_NOTICE, "SSH too many failed authentication attempts for %s",
				auth_data->username);
			ret = WOLFSSH_USERAUTH_REJECTED;
		}
	}

	return ret;
}


static int channel_req_shell_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
        ssh_server_t *st = ctx;
	word32 id = 0;

	wolfSSH_ChannelGetId(channel, &id, WS_CHANNEL_ID_SELF);
	LOG_MSG(LOG_INFO, "SSH opening shell on channel: %u", id);

	return WS_SUCCESS;
}


static int channel_req_subsys_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
        ssh_server_t *st = ctx;

	LOG_MSG(LOG_INFO, "SSH subsystem not implemented: %s",
		wolfSSH_ChannelGetSessionCommand(channel));

	return WS_ERROR;
}


static int channel_req_exec_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
        ssh_server_t *st = ctx;

	LOG_MSG(LOG_INFO, "SSH exec not implemented: %s",
		wolfSSH_ChannelGetSessionCommand(channel));

	return WS_ERROR;
}


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
	wolfSSH_CTX_SetSshProtoIdStr(ctx, "SSH-2.0-picoSSH wolfSSHv"
                                    LIBWOLFSSH_VERSION_STRING "\r\n");
	wolfSSH_SetUserAuth(ctx, user_auth_cb);
#if 0
	wolfSSH_SetUserAuthResult(ctx, user_auth_result_cb);
#endif
	wolfSSH_SetUserAuthTypes(ctx, user_auth_types_cb);
	wolfSSH_SetIORecv(ctx, receive_cb);
	wolfSSH_SetIOSend(ctx, send_cb);
	wolfSSH_CTX_SetChannelReqShellCb(ctx, channel_req_shell_cb);
	wolfSSH_CTX_SetChannelReqSubsysCb(ctx, channel_req_subsys_cb);
	wolfSSH_CTX_SetChannelReqExecCb(ctx, channel_req_exec_cb);

	for (int i = 0; st->pkeys[i].key; i++) {
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

	if (!(st->listen = tcp_listen_with_backlog(pcb, SSH_SERVER_MAX_CONN))) {
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


static err_t ssh_server_close(void *arg) {
	ssh_server_t *st = (ssh_server_t*)arg;
	err_t err = ERR_OK;

	if (!arg)
		return ERR_VAL;

	st->cstate = CS_NONE;

	if (st->client) {
		err = ssh_close_client_connection(st);
	}

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


int ssh_server_write(ssh_server_t *st, const char *buf, int length)
{
	int written = 0;

	if (!st || st->cstate != CS_CONNECT)
		return -1;

	/* rb_out is only used in thread context; send_cb takes the lwIP lock itself. */
	while (written < length && st->cstate == CS_CONNECT) {
		size_t len = ssh_ringbuffer_free(&st->rb_out);
		if (len < 1)
			break;	/* output buffer full and not draining: drop the rest */
		if (len > (size_t)(length - written))
			len = length - written;
		ssh_ringbuffer_add(&st->rb_out, buf + written, len, false);
		written += len;
		ssh_server_flush_buffer(st);
	}

	return written;
}


int ssh_server_read(ssh_server_t *st, char *buf, int length)
{
	size_t len;

	if (!st || st->cstate != CS_CONNECT)
		return -1;
	if (length < 1 || (len = ssh_ringbuffer_used(&st->rb_in)) < 1)
		return 0;

	if (len > (size_t)length)
		len = length;

	/* rb_in is filled by ssh_process_input(), also in thread context. */
	ssh_ringbuffer_read(&st->rb_in, (uint8_t*)buf, len);

	return len;
}


static void stdio_ssh_out_chars(const char *buf, int length)
{
	ssh_server_write(stdio_sshserv, buf, length);
}


static int stdio_ssh_in_chars(char *buf, int length)
{
	int len = ssh_server_read(stdio_sshserv, buf, length);

	return len > 0 ? len : PICO_ERROR_NO_DATA;
}


static void stdio_ssh_set_chars_available_callback(void (*fn)(void*), void *param)
{
    chars_available_callback = fn;
    chars_available_param = param;
}


static stdio_driver_t stdio_ssh_driver = {
	.out_chars = stdio_ssh_out_chars,
	.in_chars = stdio_ssh_in_chars,
	.set_chars_available_callback = stdio_ssh_set_chars_available_callback,
	.crlf_enabled = PICO_STDIO_DEFAULT_CRLF
};


static void stdio_ssh_init(ssh_server_t *st)
{
	stdio_sshserv = st;
	chars_available_callback = NULL;
	chars_available_param = NULL;
	stdio_set_driver_enabled(&stdio_ssh_driver, true);
}


static void stdio_ssh_close(ssh_server_t *st)
{
	if (!stdio_sshserv || stdio_sshserv != st)
		return;
	stdio_set_driver_enabled(&stdio_ssh_driver, false);
	stdio_sshserv = NULL;
	chars_available_callback = NULL;
	chars_available_param = NULL;
}


bool ssh_server_start(ssh_server_t *st, bool stdio)
{
	cyw43_arch_lwip_begin();
	bool res = ssh_server_open(st);
	if (!res) {
		ssh_server_close(st);
	} else {
		if (stdio)
			stdio_ssh_init(st);
	}
	cyw43_arch_lwip_end();

	return res;
}


void ssh_server_destroy(ssh_server_t *st)
{
	cyw43_arch_lwip_begin();
	stdio_ssh_close(st);
	ssh_server_close(st);
	cyw43_arch_lwip_end();
	ssh_ringbuffer_free(&st->rb_tcp_in);
	ssh_ringbuffer_free(&st->rb_in);
	ssh_ringbuffer_free(&st->rb_out);
}


int ssh_server_add_priv_key(ssh_server_t *st, int type, const uint8_t *key, uint32_t key_len)
{
	if (!st || !key || key_len < 1)
		return -1;

	for (int i = 0; i <= MAX_SERVER_PKEYS; i++) {
		if (st->pkeys[i].key == NULL) {
			st->pkeys[i].key = key;
			st->pkeys[i].key_len = key_len;
			st->pkeys[i].key_type = type;
			return 0;
		}
	}

	return 1;
}


bool ssh_server_client_connected(ssh_server_t *st)
{
	return (st->cstate == CS_NONE ? false : true);
}


err_t ssh_server_get_client_ip(const ssh_server_t *st, ip_addr_t *ip, uint16_t *port)
{
	err_t res = ERR_CONN;

	/* st->client can be cleared by ssh_server_err() in interrupt context: check under lock. */
	cyw43_arch_lwip_begin();
	if (st->cstate != CS_NONE && st->client) {
		if (ip)
			ip_addr_set(ip, &st->client->remote_ip);
		if (port)
			*port = st->client->remote_port;
		res = ERR_OK;
	}
	cyw43_arch_lwip_end();

	return res;
}


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


err_t ssh_server_disconnect_client(ssh_server_t *st)
{
	ip_addr_t ip;
	uint16_t port;

	if (!st || st->cstate == CS_NONE || st->cstate == CS_CLOSING)
		return ERR_OK;

	if (ssh_server_get_client_ip(st, &ip, &port) == ERR_OK)
		LOG_MSG(LOG_NOTICE, "SSH user (%s) disconnected from %s:%u",
			st->login[0] ? st->login : "<unknown>", ip4addr_ntoa(&ip), port);

	if (st->cstate == CS_CONNECT && st->ssh && !st->peer_closed) {
		/* Send what the session still has queued, then exit status 0, EOF and channel
		   close. The client answers by closing; ssh_server_service() finishes the job. */
		ssh_server_flush_buffer(st);
		if (wolfSSH_stream_exit(st->ssh, 0) == WS_SUCCESS) {
			st->closing_deadline_us = time_us_64() + SSH_CLOSING_TIMEOUT_US;
			st->cstate = CS_CLOSING;
			return ERR_OK;
		}
	}

	return ssh_close_client_connection(st);
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


#if 0
time_t ssh_server_mytime(time_t *t)
{
	time_t tnow = 0;
	struct timespec ts;

	if (aon_timer_is_running()) {
		if (aon_timer_get_time(&ts)) {
			tnow = ts.tv_sec;
		}
	}
	if (t)
		*t = tnow;

	return tnow;
}
#endif

/* eof :-) */
