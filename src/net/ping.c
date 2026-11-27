// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `ping`: ICMP echo requests from a shell job, over an lwIP raw PCB. See ping.h; the packets,
// arguments and statistics are in ping_msg.c (pure, host tested).
//
// One request at a time: request n goes out, then the run waits up to INTERVAL_US for its reply,
// or for an ICMP error about it (destination unreachable, time exceeded). Request n+1 goes out
// INTERVAL_US after request n, so a missing reply costs no extra time. A reply or error that
// comes later than that is dropped, not counted.

#include "net/ping.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"

#include "core/heaptag.h"
#include "net/ping_msg.h"
#include "net/wifi.h"

#define PING_MAX_RUNS   4           // a shell each: the console and 3 SSH sessions. lwIP has
                                    // MEMP_NUM_RAW_PCB (4) raw PCBs, and nothing else uses them
#define INTERVAL_US     1000000     // between requests, and so how long a reply is waited for
#define DNS_TIMEOUT_US  20000000    // lwIP's own retries give up sooner; this is a backstop

typedef enum {
    P_RESOLVING,    // waiting for DNS
    P_WAITING,      // request `seq` is out; waiting for its reply
    P_SETTLED,      // request `seq` was answered, timed out or not sent: next one, or the end
} ping_state_t;

typedef enum {
    ANSWER_NONE,
    ANSWER_REPLY,   // an echo reply
    ANSWER_ERROR,   // an ICMP error about the request
} ping_answer_t;

typedef struct {
    uint32_t id;                    // for the DNS callback, which can come after the run ended
    struct raw_pcb *pcb;
    uint16_t icmp_id;               // the requests' identifier: this run's replies carry it
    uint16_t count;
    bool literal;                   // the host was an address: don't print it twice
    uint64_t start_us;
    uint64_t sent_us;               // request `seq`
    ping_stats_t stats;
    // Shared with the lwIP callbacks (interrupt context): changed and read under
    // cyw43_arch_lwip_begin() on this side.
    ping_state_t state;
    uint16_t seq;                   // the latest request's sequence number (the first is 1)
    bool dns_done, dns_ok;
    ip_addr_t addr;
    ping_answer_t answer;           // what came back for request `seq`:
    uint64_t reply_us;              //   when a reply arrived
    uint8_t reply_ttl;              //   a reply's TTL
    uint8_t error_type, error_code; //   an error's
    ip_addr_t reply_from;           //   who sent it
    char host[PING_HOST_MAX + 1];
} ping_run_t;

// Runs in progress, for the DNS callback to find its run by id. Changed under the lwIP lock.
static ping_run_t *runs[PING_MAX_RUNS];
static uint32_t next_id = 1;
static uint16_t next_icmp_id;
static bool icmp_id_seeded;

// --- lwIP side (interrupt context: no heap, no logging, no shell output) ------------------------

static ping_run_t *find_run(uint32_t id) {
    for (unsigned i = 0; i < PING_MAX_RUNS; i++)
        if (runs[i] && runs[i]->id == id)
            return runs[i];
    return NULL;
}

static void dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    ping_run_t *r = find_run((uint32_t)(uintptr_t)arg);
    if (!r)
        return;                                 // ended while resolving
    if (addr)
        r->addr = *addr;
    r->dns_ok = addr != NULL;
    r->dns_done = true;
}

// Every ICMP packet the board receives comes here first (p starts at the IP header). Take the
// replies to this run's requests; leave everything else to lwIP (return 0), which answers pings
// to the board and passes other ICMP on. An ICMP error about the request in flight is recorded
// but still left to lwIP, which counts it. The pcb is removed before the run is freed, under the
// lock, so `arg` is always alive here.
static u8_t recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb;
    uint64_t now = time_us_64();
    ping_run_t *r = arg;
    uint8_t buf[PING_RECV_MAX];                 // longer packets are neither (RFC 4884
    uint16_t seq;                               // extensions included: they show as a timeout)
    uint8_t ttl;
    ping_error_t e;
    if (p->tot_len > sizeof(buf) || pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len)
        return 0;
    if (ping_parse_reply(buf, p->tot_len, r->icmp_id, &seq, &ttl)) {
        if (seq == 0 || seq > r->seq)
            return 0;
        if (r->state == P_WAITING && seq == r->seq && r->answer == ANSWER_NONE) {
            r->answer = ANSWER_REPLY;
            r->reply_us = now;
            r->reply_ttl = ttl;
            r->reply_from = *addr;
        }
        pbuf_free(p);                           // ours, also when late or repeated
        return 1;
    }
    if (ping_parse_error(buf, p->tot_len, r->icmp_id, &e) && r->state == P_WAITING &&
        e.seq == r->seq && r->answer == ANSWER_NONE) {
        r->answer = ANSWER_ERROR;
        r->error_type = e.type;
        r->error_code = e.code;
        r->reply_from = *addr;
    }
    return 0;
}

// --- shell side ----------------------------------------------------------------------------------

static void run_free(ping_run_t *r) {
    cyw43_arch_lwip_begin();
    for (unsigned i = 0; i < PING_MAX_RUNS; i++)
        if (runs[i] == r)
            runs[i] = NULL;
    if (r->pcb)
        raw_remove(r->pcb);
    cyw43_arch_lwip_end();
    heap_release(HEAP_PING, r);
}

static void summary(shell_t *sh, ping_run_t *r) {
    char line[160];
    ping_summary(&r->stats, line, sizeof(line));
    shell_printf(sh, "--- %s: %s\n", r->host, line);
}

static const char *send_error(err_t err) {
    switch (err) {
    case ERR_RTE:
        return "no route (Wi-Fi down?)";
    case ERR_MEM:
    case ERR_BUF:
        return "out of memory";
    default:
        return NULL;
    }
}

// Send the next request. Its sequence number and the state change under the lock, so a reply
// can't be matched against the old ones.
static void send_next(shell_t *sh, ping_run_t *r) {
    uint16_t seq = r->seq + 1;
    uint8_t icmp[PING_ICMP_SIZE];
    ping_build_request(icmp, r->icmp_id, seq);
    err_t err = ERR_MEM;
    cyw43_arch_lwip_begin();
    r->seq = seq;
    r->answer = ANSWER_NONE;
    r->sent_us = time_us_64();
    struct pbuf *p = pbuf_alloc(PBUF_IP, PING_ICMP_SIZE, PBUF_RAM);
    if (p) {
        memcpy(p->payload, icmp, PING_ICMP_SIZE);   // PBUF_RAM: one piece
        err = raw_sendto(r->pcb, p, &r->addr);
        pbuf_free(p);
    }
    r->state = err == ERR_OK ? P_WAITING : P_SETTLED;
    cyw43_arch_lwip_end();
    if (err == ERR_OK) {
        r->stats.sent++;
        return;
    }
    r->stats.not_sent++;
    const char *why = send_error(err);
    if (why)
        shell_printf(sh, "seq=%u not sent: %s\n", seq, why);
    else
        shell_printf(sh, "seq=%u not sent: error %d\n", seq, (int)err);
}

// Request `seq` is out: has its reply or an error about it come, or its time run out?
static void check_reply(shell_t *sh, ping_run_t *r, uint64_t now) {
    ping_answer_t answer;
    uint64_t reply_us;
    uint8_t ttl, type, code;
    ip_addr_t from;
    cyw43_arch_lwip_begin();
    answer = r->answer;
    reply_us = r->reply_us;
    ttl = r->reply_ttl;
    type = r->error_type;
    code = r->error_code;
    from = r->reply_from;
    if (answer != ANSWER_NONE || now - r->sent_us >= INTERVAL_US)
        r->state = P_SETTLED;                   // a later reply or error is dropped
    cyw43_arch_lwip_end();
    char ip[IPADDR_STRLEN_MAX];
    if (answer != ANSWER_NONE)
        ipaddr_ntoa_r(&from, ip, sizeof(ip));
    if (answer == ANSWER_REPLY) {
        uint32_t rtt = (uint32_t)(reply_us - r->sent_us);
        char ms[16];
        ping_ms_text(rtt, ms);
        ping_stats_reply(&r->stats, rtt);
        shell_printf(sh, "reply from %s: seq=%u ttl=%u time=%s ms\n", ip, r->seq, ttl, ms);
    } else if (answer == ANSWER_ERROR) {
        char what[40];
        ping_error_text(type, code, what);
        r->stats.icmp_errors++;
        shell_printf(sh, "error from %s: seq=%u %s\n", ip, r->seq, what);
    } else if (r->state == P_SETTLED) {
        shell_printf(sh, "seq=%u timeout\n", r->seq);
    }
}

static bool ping_step(shell_t *sh, void *arg, bool cancel) {
    ping_run_t *r = arg;
    if (cancel) {               // Ctrl-C, or the session ended (output then goes nowhere)
        if (r->state != P_RESOLVING)
            summary(sh, r);
        run_free(r);
        return false;
    }
    uint64_t now = time_us_64();
    if (r->state == P_RESOLVING) {
        bool done, ok;
        ip_addr_t addr;
        cyw43_arch_lwip_begin();
        done = r->dns_done;
        ok = r->dns_ok;
        addr = r->addr;
        cyw43_arch_lwip_end();
        if (!done && now - r->start_us < DNS_TIMEOUT_US)
            return true;
        if (!done || !ok) {
            shell_printf(sh, "ping: DNS lookup of %s %s\n", r->host, done ? "failed" : "timed out");
            run_free(r);
            return false;
        }
        char ip[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&addr, ip, sizeof(ip));
        if (r->literal)
            shell_printf(sh, "ping %s: ", r->host);
        else
            shell_printf(sh, "ping %s (%s): ", r->host, ip);
        shell_printf(sh, "%u request%s of %u data bytes, one a second\n", r->count,
                     r->count == 1 ? "" : "s", PING_DATA_SIZE);
        send_next(sh, r);
        return true;
    }
    if (r->state == P_WAITING) {
        check_reply(sh, r, now);
        if (r->state == P_WAITING)
            return true;
    }
    // P_SETTLED: request `seq` is done with.
    if (r->seq >= r->count) {
        summary(sh, r);
        run_free(r);
        return false;
    }
    if (time_us_64() - r->sent_us >= INTERVAL_US)
        send_next(sh, r);
    return true;
}

void ping_command(shell_t *sh, char *args) {
    char *host;
    unsigned count;
    const char *bad = ping_parse_args(args, &host, &count);
    if (bad) {
        shell_printf(sh, "%s\n", bad);
        return;
    }
    // Any shell, SSH included: SSH users are the device's trusted owners, and `http` already
    // connects to any host they name.
    if (!wifi_up()) {
        shell_printf(sh, "ping: Wi-Fi is not connected\n");
        return;
    }
    ping_run_t *r = heap_zalloc(HEAP_PING, sizeof(*r));
    if (!r) {
        shell_printf(sh, "ping: out of memory\n");
        return;
    }
    strcpy(r->host, host);                      // ping_parse_args() checked the length
    r->count = (uint16_t)count;
    r->start_us = time_us_64();
    ip_addr_t literal;
    r->literal = ipaddr_aton(host, &literal);
    r->state = P_RESOLVING;

    const char *refused = NULL;
    cyw43_arch_lwip_begin();
    unsigned slot = 0;
    while (slot < PING_MAX_RUNS && runs[slot])
        slot++;
    if (slot == PING_MAX_RUNS || !(r->pcb = raw_new(IP_PROTO_ICMP))) {
        refused = "ping: too many pings at once";
    } else {
        if (!icmp_id_seeded) {                  // not the same identifiers after every boot
            next_icmp_id = (uint16_t)LWIP_RAND();
            icmp_id_seeded = true;
        }
        r->id = next_id++;
        r->icmp_id = next_icmp_id++;
        raw_recv(r->pcb, recv_cb, r);
        runs[slot] = r;
        err_t err = dns_gethostbyname(r->host, &r->addr, dns_found, (void *)(uintptr_t)r->id);
        if (err == ERR_OK)
            r->dns_ok = r->dns_done = true;     // an address, or a name in lwIP's cache
        else if (err != ERR_INPROGRESS)
            r->dns_done = true;                 // dns_ok stays false
    }
    cyw43_arch_lwip_end();
    if (refused) {
        shell_printf(sh, "%s\n", refused);
        run_free(r);
        return;
    }
    shell_run_job(sh, ping_step, r);
}
