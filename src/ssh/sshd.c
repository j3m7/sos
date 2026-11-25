// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "ssh/sshd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssh/ssh.h>

#include "pico-sshd.h"
#include "pico-sshd/log.h"

#include "chat/chat.h"
#include "core/config.h"
#include "core/log.h"
#include "core/settings.h"
#include "files/sdcard.h"

#define SSH_KEY_TYPE        "ssh-ed25519"     // the host key's type

#ifndef SOS_SSH_MAX_SESSIONS
#define SOS_SSH_MAX_SESSIONS 3
#endif

_Static_assert(SOS_SSH_MAX_SESSIONS + 1 <= CHAT_MAX_PEERS, "chat: a peer for every shell");

static ssh_server_t *server;
// One shell per session slot, restarted whenever the slot gets a new login.
static shell_t session_shells[SOS_SSH_MAX_SESSIONS];
static uint32_t session_shell_ids[SOS_SSH_MAX_SESSIONS];   // session_id each shell serves; 0 = none
// Output each session's shell has produced in the current shell_poll() call. Once it reaches
// SESSION_POLL_OUTPUT_BUDGET the shell gets no more input until the next main-loop pass, so a
// burst of piped commands can't hold up the USB console and the other sessions for seconds.
#define SESSION_POLL_OUTPUT_BUDGET 2048
static uint32_t session_poll_output[SOS_SSH_MAX_SESSIONS];
static char hostkey_fingerprint[64];

static const char banner[] = "\r\nSOS - Small/Secure OS\r\n\r\n";

// --- helpers ---------------------------------------------------------------

// The user name a client gave, for display: any name logs in (the keys decide), so it's the
// client's text and may hold escape sequences. Only printable ASCII is kept; other bytes
// become '?'. out has room for strlen(in) + 1.
static void printable_name(char *out, const char *in) {
    for (; *in; in++)
        *out++ = (*in >= 0x20 && *in < 0x7f) ? *in : '?';
    *out = '\0';
}

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// "SHA256:<base64 without padding>", as printed by ssh-keygen -l.
void sshd_fingerprint(const uint8_t *blob, uint32_t len, char *out, size_t out_len) {
    uint8_t digest[WC_SHA256_DIGEST_SIZE];
    uint8_t b64[64];
    word32 b64_len = sizeof(b64);

    snprintf(out, out_len, "(unknown)");
    if (wc_Sha256Hash(blob, len, digest) != 0)
        return;
    if (Base64_Encode_NoNl(digest, sizeof(digest), b64, &b64_len) != 0)
        return;
    while (b64_len > 0 && (b64[b64_len - 1] == '=' || b64[b64_len - 1] == '\0'))
        b64_len--;
    snprintf(out, out_len, "SHA256:%.*s", (int)b64_len, b64);
}

// Derive the host public key from the private DER and compute its fingerprint, so it can be
// checked over the trusted USB console before trusting the device on first connect.
static void compute_host_fingerprint(const uint8_t *der, uint32_t der_len) {
    ed25519_key key;
    word32 idx = 0;
    uint8_t pub[ED25519_PUB_KEY_SIZE];
    word32 pub_len = sizeof(pub);
    uint8_t blob[4 + sizeof(SSH_KEY_TYPE) - 1 + 4 + ED25519_PUB_KEY_SIZE];
    size_t type_len = strlen(SSH_KEY_TYPE);

    snprintf(hostkey_fingerprint, sizeof(hostkey_fingerprint), "(unknown)");
    if (wc_ed25519_init(&key) != 0)
        return;
    if (wc_Ed25519PrivateKeyDecode(der, &idx, &key, der_len) == 0 &&
        wc_ed25519_export_public(&key, pub, &pub_len) == 0) {
        put_u32be(blob, type_len);
        memcpy(blob + 4, SSH_KEY_TYPE, type_len);
        put_u32be(blob + 4 + type_len, pub_len);
        memcpy(blob + 8 + type_len, pub, pub_len);
        sshd_fingerprint(blob, 8 + type_len + pub_len, hostkey_fingerprint, sizeof(hostkey_fingerprint));
    }
    wc_ed25519_free(&key);
}

// --- pico-sshd callbacks -----------------------------------------------------

// Server messages go to the system log (may be called in interrupt context; the log defers
// printing). pico-sshd's levels are syslog's, like the log module's.
static void log_cb(int priority, const char *format, ...) {
    char msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, format);
    vsnprintf(msg, sizeof(msg), format, ap);
    va_end(ap);
    log_msg(priority, "sshd: %s", msg);
}

// Public key only: the key must be in the configured trusted list (checked at each login, so
// changes apply immediately). Any user name is accepted: there's one user, and the keys are what
// decide who gets in; the name a client gives is only shown (`ssh`, `who`, the log). wolfSSH
// verifies the client's signature itself once this returns success.
static int auth_cb(void *ctx, const byte *login, word32 login_len,
                   const byte *auth, word32 auth_len, int auth_type) {
    (void)ctx;
    (void)login;
    (void)login_len;
    const sos_config_t *cfg = settings();
    if (auth_type != WOLFSSH_USERAUTH_PUBLICKEY || !auth)
        return WOLFSSH_USERAUTH_FAILURE;
    if (!config_key_find(cfg, auth, auth_len))
        return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
    return WOLFSSH_USERAUTH_SUCCESS;
}

// --- public API --------------------------------------------------------------

bool sshd_start(const uint8_t *hostkey, uint32_t hostkey_len) {
    if (server)
        return true;
    compute_host_fingerprint(hostkey, hostkey_len);

    if (!(server = ssh_server_new(SOS_SSH_MAX_SESSIONS, 4096, 8192))) {
        log_msg(SOS_LOG_ERR, "sshd: out of memory");
        return false;
    }
    server->auth_enabled = true;
    server->auth_cb = auth_cb;
    server->auth_cb_ctx = NULL;
    server->log_cb = log_cb;
    server->banner = banner;
    // Every key a client offers counts as a try, including ones it only asks about, so keep
    // OpenSSH's default: with 3, an ssh-agent holding several keys fails before the right one.
    server->max_auth_tries = 6;

    if (ssh_server_add_priv_key(server, WOLFSSH_FORMAT_ASN1, hostkey, hostkey_len) != 0) {
        log_msg(SOS_LOG_ERR, "sshd: failed to load host key");
        ssh_server_destroy(server);
        server = NULL;
        return false;
    }
    if (!ssh_server_start(server)) {
        log_msg(SOS_LOG_ERR, "sshd: failed to start");
        ssh_server_destroy(server);
        server = NULL;
        return false;
    }
    log_msg(SOS_LOG_NOTICE, "sshd: listening on port %u (up to %u sessions), host key %s %s",
            server->port, server->max_sessions, SSH_KEY_TYPE, hostkey_fingerprint);
    return true;
}

bool sshd_running(void) {
    return server != NULL;
}

const char *sshd_host_fingerprint(void) {
    return hostkey_fingerprint;
}

// --- per-session shells ---------------------------------------------------------

static int session_read(void *ctx, char *buf, int len) {
    ssh_session_t *ss = ctx;
    if (session_poll_output[ss->index] >= SESSION_POLL_OUTPUT_BUDGET)
        return 0;                   // budget used up: continue on the next pass
    int n = ssh_session_read(ss, buf, len);
    return n > 0 ? n : 0;
}

static void session_write(void *ctx, const char *buf, int len) {
    ssh_session_t *ss = ctx;
    session_poll_output[ss->index] += (uint32_t)len;
    ssh_session_write(ss, buf, len);
}

// Jobs (cat, cp ...) size their output by this, so a slow client slows the job instead of the
// write blocking or dropping output. Capped by what's left of this pass's fairness budget.
static int session_write_space(void *ctx) {
    ssh_session_t *ss = ctx;
    int left = SESSION_POLL_OUTPUT_BUDGET - (int)session_poll_output[ss->index];
    int space = ssh_session_write_space(ss);
    if (left < 0)
        left = 0;
    return space < left ? space : left;
}

static void session_logout(void *ctx) {
    ssh_session_disconnect((ssh_session_t *)ctx);
}

// Who a new session is, for wall/chat/who: the comment of the trusted key it logged in with
// ("key <n>" without one), or the slot if that key has been removed since. Where it's from.
static void session_register(shell_t *sh, ssh_session_t *ss) {
    char name[CONFIG_KEY_COMMENT_MAX + 16];
    char from[48] = "ssh";
    const sos_config_t *cfg = settings();
    snprintf(name, sizeof(name), "ssh%u", ss->index + 1);
    for (unsigned k = 0; k < cfg->key_count && ss->auth_key_len; k++) {
        const config_key_t *key = &cfg->keys[k];
        if (key->len == ss->auth_key_len && !memcmp(key->blob, ss->auth_key, key->len)) {
            if (key->comment[0])
                snprintf(name, sizeof(name), "%s", key->comment);
            else
                snprintf(name, sizeof(name), "key %u", k + 1);
            break;
        }
    }
    ip_addr_t ip;
    uint16_t port;
    if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK) {
        char ipstr[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&ip, ipstr, sizeof(ipstr));
        snprintf(from, sizeof(from), "ssh %s", ipstr);
    }
    if (!chat_register(sh, ss->index + 1, name, from))
        log_msg(SOS_LOG_WARNING, "chat: no room to register session %u", ss->index + 1);
}

static void session_end(unsigned i) {
    shell_end(&session_shells[i]);          // cancel a running job (closes its files)
    chat_unregister(&session_shells[i]);
}

static const shell_io_t session_io = {
    .name = "ssh",
    .read = session_read,
    .write = session_write,
    .logout = session_logout,
    .crlf = true,               // raw channel: no stdio CRLF translation
    .admin = false,             // configuration changes only on the console
    .write_space = session_write_space,
};

void sshd_poll(void) {
    if (!server)
        return;
    ssh_server_service(server);     // all wolfSSH work happens here, not in lwIP callbacks

    for (unsigned i = 0; i < server->max_sessions; i++) {
        ssh_session_t *ss = ssh_server_session(server, i);
        if (ss->cstate != CS_CONNECT) {
            if (session_shell_ids[i])       // (harmless for an SFTP slot: no job, not registered)
                session_end(i);
            session_shell_ids[i] = 0;
            continue;
        }
        if (ss->sftp) {                     // served by the SSH server itself (no shell)
            if (session_shell_ids[i] != ss->session_id) {
                session_shell_ids[i] = ss->session_id;
                sdcard_mount(NULL);         // SFTP works on the SD card
            }
            continue;
        }
        if (session_shell_ids[i] != ss->session_id) {
            if (session_shell_ids[i])
                session_end(i);
            session_shell_ids[i] = ss->session_id;
            shell_init(&session_shells[i], &session_io, ss);
            session_register(&session_shells[i], ss);
        }
        session_poll_output[i] = 0;
        shell_poll(&session_shells[i]);
        chat_deliver(&session_shells[i]);   // messages from the others, as far as they fit
        ss->busy = shell_busy(&session_shells[i]);  // a client's EOF waits for running commands
        ssh_session_send_output(ss);        // everything the shell wrote, in as few packets as fit
    }
}

void sshd_status(shell_t *sh) {
    if (!server) {
        shell_printf(sh, "sshd: not running\n");
        return;
    }
    unsigned in_use = 0;
    for (unsigned i = 0; i < server->max_sessions; i++)
        if (ssh_server_session(server, i)->cstate != CS_NONE)
            in_use++;

    shell_printf(sh, "host key:  %s %s\n", SSH_KEY_TYPE, hostkey_fingerprint);
    shell_printf(sh, "sessions:  %u of %u in use, %lu refused (all busy)\n",
                 in_use, server->max_sessions, (unsigned long)server->rejected_busy);
    shell_printf(sh, "limits:    %lu refused (address penalized), %lu dropped before login (slots full)\n",
                 (unsigned long)server->rejected_penalty, (unsigned long)server->dropped_stale);
    shell_printf(sh, "logins:    %lu, last handshake %lu ms\n",
                 (unsigned long)server->logins,
                 (unsigned long)(server->last_handshake_us / 1000));
    shell_printf(sh, "ssh work:  %lu calls, %llu ms total, longest %lu ms\n",
                 (unsigned long)server->work_calls,
                 (unsigned long long)(server->work_us_total / 1000),
                 (unsigned long)(server->work_us_max / 1000));

    for (unsigned i = 0; i < server->max_sessions; i++) {
        ssh_session_t *ss = ssh_server_session(server, i);
        if (ss->cstate == CS_NONE)
            continue;
        char who[64] = "-";
        ip_addr_t ip;
        uint16_t port;
        if (ssh_session_get_client_ip(ss, &ip, &port) == ERR_OK) {
            char ipstr[IPADDR_STRLEN_MAX];
            ipaddr_ntoa_r(&ip, ipstr, sizeof(ipstr));
            char login[sizeof(ss->login)];
            printable_name(login, ss->login);
            snprintf(who, sizeof(who), "%s@%s:%u", login[0] ? login : "?", ipstr, port);
        }
        uint64_t secs = (time_us_64() - ss->accept_start_us) / 1000000;
        shell_printf(sh, "  %c %u  %-36s %-10s %-5s %llum%02llus\n",
                     sh == &session_shells[i] ? '*' : ' ', i + 1, who,
                     ssh_server_connection_state_name(ss->cstate), ss->sftp ? "sftp" : "shell",
                     secs / 60, secs % 60);
    }
}
