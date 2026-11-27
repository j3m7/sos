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
   - Password authentication removed (sha512_crypt dependency dropped); the advertised
     methods are public key only.
   - The default authentication callback (a table of user name and key pairs) is removed:
     the application sets auth_cb, and without one every login fails.
   - Multiple concurrent sessions: per-connection state lives in ssh_session_t; lwIP and
     wolfSSH callbacks get the session as context. The stdio driver is removed in favour of
     ssh_session_read()/ssh_session_write().
   - All wolfSSH processing moved out of lwIP callbacks (interrupt context with the
     threadsafe_background arch) into ssh_server_service(), called from the main loop. The
     callbacks only buffer input and flag work; connections are torn down in thread context.
     Logins no longer wait for the 500 ms TCP poll between handshake steps.
   - ssh_server_recv() no longer drops/leaks input when the buffer is full (returns ERR_MEM so
     lwIP redelivers it).
   - Graceful disconnect: ssh_session_disconnect() enters CS_CLOSING; queued output is sent
     (at most SSH_DRAIN_TIMEOUT_US), then exit status 0 and channel close, then it waits (at
     most SSH_CLOSING_TIMEOUT_US) for the client to close the TCP connection. A new connection arriving when no slot is free reclaims a closing session.
   - Timing statistics for wolfSSH calls.
   - Session buffers are allocated when a connection is set up (thread context) and freed when
     it closes, so idle session slots cost no heap.
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
   - I/O callbacks return WS_CBIO_ERR_WANT_READ/WRITE instead of WS_WANT_READ/WRITE; a full
     TCP send buffer was treated as a fatal socket error.
   - Output flow control instead of dropping output: ssh_session_read() holds input back while
     rb_out is more than half full, and ssh_session_write() waits (bounded) for room.
   - Channel input is drained on every pass, not only on WS_CHAN_RXD. Client EOF ends the
     session cleanly once its input is consumed; stderr-type extended data is discarded
     instead of ending the session.
   - Output is batched: ssh_session_write() only queues (sending early only when rb_out is
     full) and ssh_session_send_output() sends it once the shell has run, so a burst of small
     writes becomes a few large packets. ssh_session_write_space() reports the free room.
   - Nagle's algorithm is disabled on client connections (like TCP_NODELAY elsewhere).
   - tcp_poll() intervals are given in milliseconds (SSH_POLL_TICKS), so they don't change with
     lwIP's TCP_TMR_INTERVAL.
   - Connections that don't log in can't hold the server: when every slot is in use, ones
     older than SSH_LOGIN_GRACE_BUSY_US are dropped, and an address with SSH_PENALTY_FAILS such
     connections within SSH_PENALTY_WINDOW_US is refused until it has been quiet that long.
   - The public key a login was signed with is kept in ssh_session_t (auth_key), recorded by a
     user-auth result callback (after wolfSSH has verified the signature), so the application
     can tell which of its trusted keys a session uses.
*/

#include <stdint.h>
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
#include <wolfssh/wolfsftp.h>

#include "core/heaptag.h"
#include "pico-sshd.h"
#include "pico-sshd/log.h"


#define SSH_DEFAULT_PORT 22

/* tcp_poll() intervals count lwIP's coarse TCP timer, which ticks every 2 * TCP_TMR_INTERVAL
   ms (SOS sets 50 ms in lwipopts.h, lwIP's default is 250): convert from milliseconds so the
   times don't depend on it. */
#ifndef TCP_TMR_INTERVAL
#define TCP_TMR_INTERVAL 250
#endif
#define SSH_POLL_TICKS(ms) ((ms) / (2 * TCP_TMR_INTERVAL) > 0 ? (ms) / (2 * TCP_TMR_INTERVAL) : 1)
#define SSH_CLIENT_POLL_TIME SSH_POLL_TICKS(500)

#define LOG_MAX_MSG_LEN 256
#define SSH_CLOSING_TIMEOUT_US (2 * 1000 * 1000)
/* Longest a disconnecting session may spend sending its remaining output before the exit status
   and channel close go out. */
#define SSH_DRAIN_TIMEOUT_US (10 * 1000 * 1000)
/* lwIP send space required before sending exit status, EOF and channel close: wolfSSH must not
   hit would-block part way through them (wolfSSH_stream_exit cannot be resumed). */
#define SSH_EXIT_MSGS_ROOM 512
#define SSH_EXIT_MSGS_QUEUE 8
/* Output flow control. The shell gets no more input while less than this share of rb_out is
   free, so a command's output always fits; a write that still finds rb_out full keeps the
   session moving for up to SSH_WRITE_WAIT_US before giving up on a client that isn't reading. */
#define SSH_OUTPUT_RESERVE_DIV 2
#define SSH_WRITE_WAIT_US (500 * 1000)
/* Connections that have not logged in within this time are dropped (like sshd LoginGraceTime),
   so idle or stalled clients cannot hold every session slot. */
#define SSH_LOGIN_GRACE_US (30 * 1000 * 1000)
/* ...and after this time when every slot is in use, so a client that connects and then says
   nothing holds a slot others are waiting for only briefly. A login takes well under a second. */
#define SSH_LOGIN_GRACE_BUSY_US (5 * 1000 * 1000)
/* An address whose connections end without a login this many times within the window gets no
   new connections until it has stayed away for a whole window (every refused attempt restarts
   it). Successful logins don't count. Like OpenSSH's PerSourcePenalties. */
#define SSH_PENALTY_FAILS 30
#define SSH_PENALTY_WINDOW_US (60 * 1000 * 1000)
/* TCP keepalive: probe after this much idle time, give up after COUNT unanswered probes. */
#define SSH_KEEPALIVE_IDLE_MS  60000
#define SSH_KEEPALIVE_INTVL_MS 10000
#define SSH_KEEPALIVE_COUNT    6

#define SSH_PROTO_ID_STR "SSH-2.0-picoSSH wolfSSHv" LIBWOLFSSH_VERSION_STRING "\r\n"

/* Refused ("server busy") connections: at most this many are kept open at once to deliver the
   disconnect message (beyond that they are reset), each for at most about 2 s. */
#define SSH_BUSY_MAX_PENDING 2
#define SSH_BUSY_POLL_TIME SSH_POLL_TICKS(2000)
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

	if ((priority & LOG_PRIMASK) > global_log_level)
		return;

	if (!(buf = heap_alloc(HEAP_NET, LOG_MAX_MSG_LEN)))
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

	heap_release(HEAP_NET, buf);
}




ssh_server_t* ssh_server_new(unsigned max_sessions, size_t rxbuf_size, size_t txbuf_size)
{
	ssh_server_t *st;

	if (max_sessions < 1)
		return NULL;
	if (!(st = heap_zalloc(HEAP_NET, sizeof(ssh_server_t))))
		return NULL;
	if (max_sessions > SIZE_MAX / sizeof(ssh_session_t) ||
	    !(st->sessions = heap_zalloc(HEAP_NET, max_sessions * sizeof(ssh_session_t)))) {
		heap_release(HEAP_NET, st);
		return NULL;
	}

	st->max_sessions = max_sessions;
	st->log_cb = ssh_server_log_msg;
	st->auth_enabled = true;
	st->auth_cb = NULL;        /* set by the application; without one every login fails */
	st->port = SSH_DEFAULT_PORT;
	st->banner = ssh_default_banner;
	st->allow_connect_cb = NULL;
	st->max_auth_tries = 6;

	/* rb_tcp_in must hold a full TCP window: lwIP may deliver everything the window allows in
	   one pbuf chain (e.g. after reassembling out-of-order segments), and a chain that can
	   never fit would stall the connection. */
	st->tcp_in_size = rxbuf_size > TCP_WND ? rxbuf_size : TCP_WND;
	st->rxbuf_size = rxbuf_size;
	st->txbuf_size = txbuf_size;

	/* Session buffers (about tcp_in_size + rxbuf_size + txbuf_size per session) are allocated
	   when a connection is set up and freed when it closes, so idle slots cost no heap. The
	   zeroed ring buffers from heap_zalloc() are valid empty buffers until then. */
	for (unsigned i = 0; i < max_sessions; i++) {
		ssh_session_t *ss = &st->sessions[i];
		ss->server = st;
		ss->index = i;
		ss->cstate = CS_NONE;
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
		LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH client closed connection from %s:%u",
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

	/* The buffers are allocated by ssh_session_setup() in thread context (this callback runs in
	   interrupt context, where the heap must not be used). Hold input that arrives before then;
	   the window isn't reopened until wolfSSH consumes data, so it can't exceed TCP_WND, which
	   is the size of rb_tcp_in. */
	if (!ss->rb_tcp_in.buf) {
		if (ss->rx_held)
			pbuf_cat(ss->rx_held, p);
		else
			ss->rx_held = p;
		ss->work_pending = true;
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


/* --- per-address penalty (callers hold the lwIP lock or run in interrupt context) -------- */

static ssh_penalty_t *penalty_find(ssh_server_t *st, const ip_addr_t *ip)
{
	uint64_t now = time_us_64();

	for (int i = 0; i < SSH_PENALTY_SLOTS; i++) {
		ssh_penalty_t *p = &st->penalties[i];
		if (p->fails && now - p->start_us > SSH_PENALTY_WINDOW_US)
			p->fails = 0;	/* window over: forgotten */
		if (p->fails && ip_addr_cmp(&p->ip, ip))
			return p;
	}
	return NULL;
}

/* A connection from ip ended without logging in. Returns true if that reached the limit. */
static bool penalty_note_failure(ssh_server_t *st, const ip_addr_t *ip)
{
	ssh_penalty_t *p = penalty_find(st, ip);

	if (!p) {
		/* A free entry, or else the one whose window started first. */
		p = &st->penalties[0];
		for (int i = 0; i < SSH_PENALTY_SLOTS; i++) {
			ssh_penalty_t *q = &st->penalties[i];
			if (!q->fails) {
				p = q;
				break;
			}
			if (q->start_us < p->start_us)
				p = q;
		}
		ip_addr_set(&p->ip, ip);
		p->fails = 0;
		p->start_us = time_us_64();
	}
	if (p->fails < 255)
		p->fails++;
	return p->fails == SSH_PENALTY_FAILS;
}

/* True if new connections from ip are refused for now; an attempt restarts the window. */
static bool penalty_blocked(ssh_server_t *st, const ip_addr_t *ip)
{
	ssh_penalty_t *p = penalty_find(st, ip);

	if (!p || p->fails < SSH_PENALTY_FAILS)
		return false;
	p->start_us = time_us_64();
	return true;
}


/* Tear down a session and free its slot. Thread context only (wolfSSH_free uses the heap);
   takes the lwIP lock (recursive), so callers may already hold it. */
static err_t ssh_close_session(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	err_t err = ERR_OK;

	cyw43_arch_lwip_begin();
	/* Never logged in: counts against its address. */
	bool penalized = ss->cstate == CS_ACCEPT && penalty_note_failure(st, &ss->remote_ip);
	ss->cstate = CS_NONE;
	ss->login[0] = 0;
	ss->auth_key_len = 0;
	ss->work_pending = false;
	ss->peer_closed = false;
	ss->setup_pending = false;
	ss->eof_received = false;
	ss->exit_sent = false;
	ss->output_stalled = false;
	if (ss->client) {
		err = close_client_connection(ss->client);
		ss->client = NULL;
	}
	if (ss->ssh) {
		wolfSSH_free(ss->ssh);
		ss->ssh = NULL;
	}
	if (ss->rx_held) {
		pbuf_free(ss->rx_held);
		ss->rx_held = NULL;
	}
	/* Release the session buffers; the next connection in this slot allocates new ones. */
	ssh_ringbuffer_destroy(&ss->rb_tcp_in);
	ssh_ringbuffer_destroy(&ss->rb_in);
	ssh_ringbuffer_destroy(&ss->rb_out);
	cyw43_arch_lwip_end();

	if (penalized) {
		char ip[IPADDR_STRLEN_MAX];
		ipaddr_ntoa_r(&ss->remote_ip, ip, sizeof(ip));
		LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH: %u connections from %s without a login; refusing it until"
			" it has been quiet for %u s", SSH_PENALTY_FAILS, ip,
			(unsigned)(SSH_PENALTY_WINDOW_US / 1000000));
	}
	LOG_MSG(LOG_DEBUG, "ssh_close_session(%u): %d", ss->index, err);
	return err;
}


static ssh_session_t *find_free_session(ssh_server_t *st)
{
	for (unsigned i = 0; i < st->max_sessions; i++)
		if (st->sessions[i].cstate == CS_NONE)
			return &st->sessions[i];

	/* No free slot: reclaim one that is only waiting for its client to hang up. Such a session
	   has already sent its exit status and channel close (exit_sent; before that, thread context
	   is still flushing its output), and thread context only touches it under the lwIP lock
	   (ssh_session_service), so the TCP side can be closed here. Its
	   wolfSSH session is left for ssh_session_setup() to free: this runs in interrupt
	   context, where the (unlocked) heap must not be used. */
	for (unsigned i = 0; i < st->max_sessions; i++) {
		ssh_session_t *ss = &st->sessions[i];
		if (ss->cstate == CS_CLOSING && ss->exit_sent) {
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
			LOG_MSG(LOG_AUTH | LOG_ERR, "SSH connection rejected from %s",
				ip4addr_ntoa(&pcb->remote_ip));
			return ERR_MEM;
		}
	}

	if (penalty_blocked(st, &pcb->remote_ip)) {
		st->rejected_penalty++;
		return ERR_MEM;		/* lwIP resets the connection */
	}

	if (!(ss = find_free_session(st))) {
		st->rejected_busy++;
		LOG_MSG(LOG_AUTH | LOG_ERR, "SSH connection from %s:%u rejected: all %u sessions in use",
			ip4addr_ntoa(&pcb->remote_ip), pcb->remote_port, st->max_sessions);
		return ssh_server_reject_busy(st, pcb);
	}

	LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH accepted connection from %s:%u (session %u)",
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
	ss->sftp = false;
	ss->busy = false;
	ss->exit_sent = false;
	ss->output_stalled = false;
	ss->client = pcb;
	ip_addr_set(&ss->remote_ip, &pcb->remote_ip);
	ss->auth_tries = 0;
	ss->login[0] = 0;
	ss->auth_key_len = 0;
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
	/* Like TCP_NODELAY in other SSH servers. With Nagle, a packet smaller than the MSS waits
	   for the previous one to be ACKed, and clients delay ACKs (~40 ms): output written in
	   small pieces (a shell job printing a file) then crawled at ~1 KB per round trip. */
	tcp_nagle_disable(pcb);

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
		return ss->peer_closed ? WS_CBIO_ERR_CONN_CLOSE : WS_CBIO_ERR_WANT_READ;
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
	/* Queue what fits; wolfSSH calls again with the rest (it handles partial sends). */
	word32 len = tcp_sndbuf(ss->client);
	if (len > buf_len)
		len = buf_len;
	err = len > 0 ? tcp_write(ss->client, buf, (u16_t)len, TCP_WRITE_FLAG_COPY) : ERR_MEM;
	if (err == ERR_OK)
		tcp_output(ss->client);
	cyw43_arch_lwip_end();

	/* I/O callbacks report WS_CBIO_ERR_* codes. WS_WANT_WRITE is not one: wolfSSH treated it
	   as a socket error, killing the session (and duplicating the unsent packet) whenever the
	   TCP send buffer filled. The sent callback retries once lwIP has room. */
	return err == ERR_OK ? (int)len : WS_CBIO_ERR_WANT_WRITE;
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


/* Called by wolfSSH after it has verified the signature of a public key request (user_auth_cb
   runs before that, so it can't tell a good signature from a junk one). Records the key the
   login is made with, so the application can tell which of its keys a session uses. */
static int user_auth_result_cb(byte result, WS_UserAuthData *auth_data, void *ctx)
{
	ssh_session_t *ss = ctx;

	if (!ss || !auth_data)
		return WS_SUCCESS;
	ss->auth_key_len = 0;
	if (result == WOLFSSH_USERAUTH_SUCCESS && auth_data->type == WOLFSSH_USERAUTH_PUBLICKEY) {
		word32 klen = auth_data->sf.publicKey.publicKeySz;
		if (klen <= sizeof(ss->auth_key)) {
			memcpy(ss->auth_key, auth_data->sf.publicKey.publicKey, klen);
			ss->auth_key_len = (uint16_t)klen;
		}
	}
	return WS_SUCCESS;
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
		/* Where from, so a line in the auth log stands on its own. */
		char ip[IPADDR_STRLEN_MAX];
		ipaddr_ntoa_r(&ss->remote_ip, ip, sizeof(ip));
		if (ret == WOLFSSH_USERAUTH_SUCCESS) {
			if (auth_data->sf.publicKey.hasSignature)
				LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH accepted publickey for %s"
					" (SHA256:%s) from %s (session %u)",
					auth_data->username, hash, ip, ss->index + 1);
		} else if (ret == WOLFSSH_USERAUTH_INVALID_USER) {
			LOG_MSG(LOG_AUTH | LOG_INFO, "SSH failed publickey authentication for invalid user"
				" %s (SHA256:%s) from %s", auth_data->username, hash, ip);
		} else {
			LOG_MSG(LOG_AUTH | LOG_INFO, "SSH failed publickey authentication for user"
				" %s (SHA256:%s) from %s", auth_data->username, hash, ip);
		}
	}
	else if (auth_type == WOLFSSH_USERAUTH_NONE && !st->auth_enabled) {
		ret = WOLFSSH_USERAUTH_SUCCESS;
		LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH unauthenticated connection accepted for user %s",
			auth_data->username);
	}
	else {
		ret = WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
	}


	/* Check for too many failed authentication attempts... */
	if (ret != WOLFSSH_USERAUTH_SUCCESS && ret != WOLFSSH_USERAUTH_INVALID_AUTHTYPE) {
		ss->auth_tries++;
		if (st->max_auth_tries > 0 && ss->auth_tries >= st->max_auth_tries) {
			LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH too many failed authentication attempts for %s",
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
	const char *cmd = wolfSSH_ChannelGetSessionCommand(channel);

	/* "sftp" exactly (length too: no embedded NUL), and on the session's first channel:
	   wolfSSH_SFTP_accept() works on the head of the channel list. Only while logging in
	   (CS_ACCEPT, where wolfSSH_accept() then does the SFTP version exchange): a session that
	   already runs a shell can't turn into an SFTP one. */
	if (ss && cmd && wolfSSH_ChannelGetSessionCommandSz(channel) == 4 && !strcmp(cmd, "sftp") &&
	    channel == wolfSSH_ChannelNext(ss->ssh, NULL) && ss->cstate == CS_ACCEPT) {
		ss->sftp = true;
		return WS_SUCCESS;
	}
	LOG_MSG(LOG_INFO, "SSH subsystem not available: %s", cmd ? cmd : "(none)");
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

	if (ss->cstate != CS_CONNECT && ss->cstate != CS_CLOSING) {
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
	if (ssh_ringbuffer_used(&ss->rb_out) == 0)
		ss->output_stalled = false;

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
		if (room < 1)
			break;	/* ssh_session_service() resumes once the shell has made room */
		res = wolfSSH_ChannelIdRead(ss->ssh, 0, tmp, room < sizeof(tmp) ? room : sizeof(tmp));
		if (res <= 0)
			break;
		ssh_ringbuffer_add(&ss->rb_in, tmp, res, false);
	}

	/* Backstop for a missed WS_EOF status: the channel itself records the client's EOF. */
	WOLFSSH_CHANNEL *ch = wolfSSH_ChannelFind(ss->ssh, 0, WS_CHANNEL_ID_SELF);
	if (ch && wolfSSH_ChannelGetEof(ch))
		ss->eof_received = true;
}


static void ssh_session_process_input(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, rc;
	word32 channel_id = 0;

	res = SSH_TIMED(st, wolfSSH_worker(ss->ssh, &channel_id));
	if (res < 0) {
		/* The return value carries once-only statuses (WS_EOF, WS_EXTDATA, WS_CHAN_RXD) even
		   when a failed output flush in the same pass left WS_WANT_WRITE in ssh->error; only a
		   generic WS_FATAL_ERROR needs the detail from wolfSSH_get_error(). */
		rc = (res == WS_FATAL_ERROR) ? wolfSSH_get_error(ss->ssh) : res;
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


static bool ssh_session_tcp_room(ssh_session_t *ss, u16_t bytes, u16_t queue)
{
	bool ok;

	cyw43_arch_lwip_begin();
	ok = ss->client && tcp_sndbuf(ss->client) >= bytes &&
		tcp_sndqueuelen(ss->client) + queue <= TCP_SND_QUEUELEN;
	cyw43_arch_lwip_end();
	return ok;
}


/* First phase of a graceful disconnect (CS_CLOSING, exit not sent yet): keep wolfSSH running
   (window adjusts, output flushing) until everything the session produced has gone out, then
   send exit status 0, EOF and channel close, once. Client input is discarded. */
static void ssh_session_finish(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	uint8_t tmp[64];
	int res, rc;

	if (time_us_64() > ss->closing_deadline_us) {
		LOG_MSG(LOG_NOTICE, "SSH session %u: output not drained in time, closing",
			ss->index + 1);
		ssh_close_session(ss);
		return;
	}

	if (ss->work_pending) {
		ss->work_pending = false;
		res = SSH_TIMED(st, wolfSSH_worker(ss->ssh, NULL));
		rc = (res == WS_FATAL_ERROR) ? wolfSSH_get_error(ss->ssh) : res;
		if (res < 0 && rc != WS_CHAN_RXD && rc != WS_EXTDATA && rc != WS_EOF &&
		    rc != WS_WANT_READ && rc != WS_WANT_WRITE && rc != WS_REKEYING) {
			/* WS_CHANNEL_CLOSED included: the client closed first, nothing left to say. */
			ssh_close_session(ss);
			return;
		}
		while (wolfSSH_ChannelIdRead(ss->ssh, 0, tmp, sizeof(tmp)) > 0)
			;
		while (wolfSSH_ChannelIdReadExt(ss->ssh, 0, tmp, sizeof(tmp)) > 0)
			;
		if (ssh_ringbuffer_used(&ss->rb_tcp_in) > 0)
			ss->work_pending = true;
	}

	ssh_session_flush(ss);
	if (ssh_ringbuffer_used(&ss->rb_out) > 0 || wolfSSH_OutputPending(ss->ssh) ||
	    wolfSSH_RekeyPending(ss->ssh) ||
	    !ssh_session_tcp_room(ss, SSH_EXIT_MSGS_ROOM, SSH_EXIT_MSGS_QUEUE))
		return;	/* the sent callback or poll tick brings us back */

	if ((res = wolfSSH_stream_exit(ss->ssh, 0)) != WS_SUCCESS)
		LOG_MSG(LOG_NOTICE, "SSH session %u: sending exit status failed: %d",
			ss->index + 1, res);
	/* Second phase: wait for the client to close. From here interrupt context may reclaim
	   the slot, so exit_sent is set last and the session is not touched again here. */
	ss->closing_deadline_us = time_us_64() + SSH_CLOSING_TIMEOUT_US;
	ss->exit_sent = true;
}


/* SFTP session (version exchange already done by wolfSSH_accept()): handle one request per call (reading
   and sending at most WOLFSSH_MAX_SFTP_RW bytes of file data). wolfSSH reads the channel data
   itself, so ssh_session_process_input() (the shell's path) must not run for these sessions. */
static void ssh_session_sftp(ssh_session_t *ss)
{
	ssh_server_t *st = ss->server;
	int res, err;

	/* Like wolfSSH's example server (which waits with select()), call into SFTP only with
	   input waiting or a reply still to send; otherwise let the worker take in what has
	   arrived, and come back when there's more. */
	if (!wolfSSH_SFTP_PendingSend(ss->ssh) && wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) <= 0) {
		res = SSH_TIMED(st, wolfSSH_worker(ss->ssh, NULL));
		err = wolfSSH_get_error(ss->ssh);
		if (res < 0 && res != WS_CHAN_RXD && err != WS_WANT_READ && err != WS_WANT_WRITE &&
		    err != WS_CHAN_RXD && err != WS_REKEYING && err != WS_WINDOW_FULL) {
			if (err == WS_EOF || err == WS_CHANNEL_CLOSED || res == WS_EOF ||
			    res == WS_CHANNEL_CLOSED) {
				ssh_session_disconnect(ss);	/* client done */
			} else {
				LOG_MSG(LOG_INFO, "SSH session %u: sftp: worker %d (error %d)",
					ss->index + 1, res, err);
				ssh_close_session(ss);
			}
			return;
		}
		if (wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) <= 0) {
			if (ssh_ringbuffer_used(&ss->rb_tcp_in) > 0)
				ss->work_pending = true;	/* more on the wire to decrypt */
			return;
		}
	}

	res = SSH_TIMED(st, wolfSSH_SFTP_read(ss->ssh));
	err = wolfSSH_get_error(ss->ssh);
	if (res < 0 && err != WS_WANT_READ && err != WS_WANT_WRITE && err != WS_CHAN_RXD &&
	    err != WS_REKEYING && err != WS_WINDOW_FULL) {
		if (err == WS_EOF || err == WS_CHANNEL_CLOSED) {
			ssh_session_disconnect(ss);	/* client done: exit status, close */
		} else {
			LOG_MSG(LOG_INFO, "SSH session %u: sftp error %d (error %d)",
				ss->index + 1, res, err);
			ssh_close_session(ss);
		}
		return;
	}
	/* A reply that didn't fit is resumed on the next pass, as is input wolfSSH holds. */
	if (wolfSSH_SFTP_PendingSend(ss->ssh) || wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) > 0 ||
	    ssh_ringbuffer_used(&ss->rb_tcp_in) > 0)
		ss->work_pending = true;
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

	/* Allocate the session buffers, unless this slot was reclaimed from a closing session
	   (interrupt context can't free, so it keeps them and they are reused). */
	if (!ss->rb_tcp_in.buf) {
		ssh_ringbuffer_t tcp_in = {0}, in = {0}, out = {0};
		if (ssh_ringbuffer_init(&tcp_in, NULL, st->tcp_in_size) ||
		    ssh_ringbuffer_init(&in, NULL, st->rxbuf_size) ||
		    ssh_ringbuffer_init(&out, NULL, st->txbuf_size)) {
			ssh_ringbuffer_destroy(&tcp_in);
			ssh_ringbuffer_destroy(&in);
			ssh_ringbuffer_destroy(&out);
			LOG_MSG(LOG_ERR, "ssh_session_setup: out of memory for session buffers");
			return false;
		}
		/* Install them and move any input held by ssh_server_recv() under the lock, so the
		   receive callback sees either no buffer or a complete one. */
		bool fits = true;
		cyw43_arch_lwip_begin();
		ss->rb_in = in;
		ss->rb_out = out;
		ss->rb_tcp_in = tcp_in;
		if (ss->rx_held) {
			fits = ss->rx_held->tot_len <= ssh_ringbuffer_free(&ss->rb_tcp_in);
			for (struct pbuf *q = ss->rx_held; fits && q; q = q->next)
				ssh_ringbuffer_add(&ss->rb_tcp_in, q->payload, q->len, false);
			pbuf_free(ss->rx_held);
			ss->rx_held = NULL;
		}
		cyw43_arch_lwip_end();
		if (!fits) {	/* can't happen while the window is bounded by TCP_WND */
			LOG_MSG(LOG_ERR, "ssh_session_setup: early input larger than the input buffer");
			return false;
		}
	}
	ssh_ringbuffer_flush(&ss->rb_in);
	ssh_ringbuffer_flush(&ss->rb_out);

	if (!(ssh = wolfSSH_new(st->ssh_ctx))) {
		LOG_MSG(LOG_ERR, "ssh_session_setup: failed to create SSH session");
		return false;
	}
	/* SFTP starts at the card's root; otherwise wolfSSH derives "/0:" from FatFs's cwd. */
	wolfSSH_SFTP_SetDefaultPath(ssh, "/");
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

	/* Interrupt context may reclaim a CS_CLOSING slot whose exit has been sent for a new
	   connection at any time, so decide what to do with closed and closing sessions under the
	   lock. Past this block the session is in CS_ACCEPT, CS_CONNECT or the output-draining part
	   of CS_CLOSING, none of which interrupt context takes over. */
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
	if (ss->cstate == CS_CLOSING && !ss->exit_sent) {
		/* Still sending output: not reclaimable until exit_sent, so work unlocked. */
		cyw43_arch_lwip_end();
		ssh_session_finish(ss);
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
		/* With every slot in use, others are waiting for this one: less time to log in. */
		bool full = true;
		for (unsigned i = 0; i < st->max_sessions; i++) {
			const ssh_session_t *o = &st->sessions[i];
			if (o->cstate == CS_NONE || (o->cstate == CS_CLOSING && o->exit_sent))
				full = false;	/* free, or reclaimable by a new connection */
		}
		uint64_t grace = full ? SSH_LOGIN_GRACE_BUSY_US : SSH_LOGIN_GRACE_US;
		if (full && time_us_64() - ss->accept_start_us > grace)
			st->dropped_stale++;
		if (time_us_64() - ss->accept_start_us > grace) {
			ip_addr_t ip;
			uint16_t port;
			if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK)
				LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH login timed out for %s:%u (session %u)",
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
	   rb_in after every service pass) and finished what it's doing (`busy`: a command still
	   running, or input it has read but not run yet), sending any output it produced first. */
	if (ss->cstate == CS_CONNECT && ss->eof_received && !ss->busy &&
	    ssh_ringbuffer_used(&ss->rb_in) == 0 && wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) <= 0) {
		ssh_session_disconnect(ss);
		return;
	}

	/* Input left in wolfSSH because rb_in was full: resume once the shell has made room,
	   without waiting for the client to send more (it may be out of window). */
	if (ss->cstate == CS_CONNECT && !ss->work_pending && ssh_ringbuffer_free(&ss->rb_in) > 0 &&
	    wolfSSH_ChannelIdPeek(ss->ssh, 0, NULL, 1) > 0)
		ss->work_pending = true;

	if (!ss->work_pending)
		return;
	ss->work_pending = false;

	if (ss->cstate == CS_ACCEPT) {
		res = SSH_TIMED(st, wolfSSH_accept(ss->ssh));
		if (res == WS_SUCCESS || res == WS_SFTP_COMPLETE) {	/* shell, or sftp subsystem */
			LOG_MSG(LOG_DEBUG, "wolfSSH_accept(): ok");
			ss->cstate = CS_CONNECT;
			ss->session_id = ++st->logins;
			ss->handshake_us = (uint32_t)(time_us_64() - ss->accept_start_us);
			st->last_handshake_us = ss->handshake_us;
			strncpy(ss->login, wolfSSH_GetUsername(ss->ssh), sizeof(ss->login) - 1);
			ss->login[sizeof(ss->login) - 1] = 0;
			if (ss->sftp) {
				/* wolfSSH_accept() returns WS_SFTP_COMPLETE once it has also done the
				   SFTP version exchange (it calls wolfSSH_SFTP_accept() itself). */
				LOG_MSG(LOG_INFO, "SSH session %u: sftp", ss->index + 1);
			} else {	/* SFTP is a binary protocol: no greeting there */
				ssh_ringbuffer_add(&ss->rb_out, connected_text, strlen(connected_text), true);
			}
		} else {
			err = wolfSSH_get_error(ss->ssh);
			if (err == WS_WANT_READ || err == WS_WANT_WRITE)
				return;
			if (err == WS_USER_AUTH_E) {
				ip_addr_t ip;
				uint16_t port;
				if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK)
					LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH connection rejected from %s:%u",
						ip4addr_ntoa(&ip), port);
			} else {
				LOG_MSG(LOG_INFO, "wolfSSH_accept(): failed: %d (%d)", err, res);
			}
			ssh_close_session(ss);
			return;
		}
	}
	if (ss->cstate == CS_CONNECT && ss->sftp) {
		ssh_session_sftp(ss);
	} else if (ss->cstate == CS_CONNECT) {
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
	wolfSSH_SetUserAuthResult(ctx, user_auth_result_cb);
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
	heap_release(HEAP_NET, st->sessions);
	heap_release(HEAP_NET, st);
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

int ssh_session_write_space(ssh_session_t *ss)
{
	if (!ss || ss->cstate != CS_CONNECT || ss->output_stalled || ss->peer_closed)
		return 0;
	return (int)ssh_ringbuffer_free(&ss->rb_out);
}

int ssh_session_write(ssh_session_t *ss, const char *buf, int length)
{
	ssh_server_t *st = ss ? ss->server : NULL;
	int written = 0;

	if (!ss || ss->cstate != CS_CONNECT)
		return -1;

	/* rb_out is only used in thread context; send_cb takes the lwIP lock itself. */
	uint64_t deadline = 0;
	while (written < length && ss->cstate == CS_CONNECT) {
		size_t len = ssh_ringbuffer_free(&ss->rb_out);
		if (len < 1) {
			/* Full despite the input-side flow control in ssh_session_read() (one command
			   printing more than the reserve). Keep the session moving: lwIP processes ACKs
			   in the background, and the worker handles the client's window adjusts. */
			if (ss->output_stalled || ss->peer_closed)
				break;	/* client not reading (or gone): drop without waiting */
			if (!deadline)
				deadline = time_us_64() + SSH_WRITE_WAIT_US;
			if (time_us_64() > deadline) {
				LOG_MSG(LOG_NOTICE, "SSH session %u: client not reading, dropping output",
					ss->index + 1);
				ss->output_stalled = true;
				break;
			}
			if (ss->work_pending) {
				ss->work_pending = false;
				ssh_session_process_input(ss);
			} else {
				sleep_us(200);
			}
			ssh_session_flush(ss);
			continue;
		}
		if (len > (size_t)(length - written))
			len = length - written;
		ssh_ringbuffer_add(&ss->rb_out, buf + written, len, false);
		written += len;
	}
	/* Sent by ssh_session_send_output() after the shell has run (or above when rb_out fills):
	   one write per piece of output would cost one encrypted packet each. */
	return written;
}

void ssh_session_send_output(ssh_session_t *ss)
{
	if (ss && ss->cstate == CS_CONNECT && ssh_ringbuffer_used(&ss->rb_out) > 0)
		ssh_session_flush(ss);
}


int ssh_session_read(ssh_session_t *ss, char *buf, int length)
{
	size_t len;

	if (!ss || ss->cstate != CS_CONNECT)
		return -1;
	if (length < 1 || (len = ssh_ringbuffer_used(&ss->rb_in)) < 1)
		return 0;
	/* Output flow control: hold input back while earlier output is still queued, so the
	   commands it runs have room for theirs. Unread input stays in rb_in and then in wolfSSH,
	   whose window then makes the client wait too. */
	if (ssh_ringbuffer_free(&ss->rb_out) < ss->rb_out.size / SSH_OUTPUT_RESERVE_DIV)
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
		LOG_MSG(LOG_AUTH | LOG_NOTICE, "SSH user (%s) disconnected from %s:%u (session %u)",
			ss->login[0] ? ss->login : "<unknown>", ip4addr_ntoa(&ip), port,
			ss->index + 1);

	if (ss->cstate == CS_CONNECT && ss->ssh && !ss->peer_closed) {
		/* ssh_session_finish() (via ssh_server_service) sends what the session still has
		   queued, then exit status 0, EOF and channel close; the client answers by closing.
		   exit_sent is cleared before entering CS_CLOSING so the slot cannot be reclaimed
		   while output is still going out. */
		ss->exit_sent = false;
		ss->closing_deadline_us = time_us_64() + SSH_DRAIN_TIMEOUT_US;
		ss->cstate = CS_CLOSING;
		ss->work_pending = true;
		return ERR_OK;
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
