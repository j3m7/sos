// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "sshd.h"

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

#include "config.h"
#include "log.h"
#include "sdcard.h"
#include "settings.h"

#define SSH_KEY_TYPE        "ssh-ed25519"     // the host key's type

#ifndef SOS_SSH_MAX_SESSIONS
#define SOS_SSH_MAX_SESSIONS 3
#endif

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

// Public key only: the user name must match the configured one and the key must be in the
// configured trusted list (checked at each login, so changes apply immediately). wolfSSH
// verifies the client's signature itself once this returns success.
static int auth_cb(void *ctx, const byte *login, word32 login_len,
                   const byte *auth, word32 auth_len, int auth_type) {
    (void)ctx;
    const sos_config_t *cfg = settings();
    if (auth_type != WOLFSSH_USERAUTH_PUBLICKEY)
        return WOLFSSH_USERAUTH_FAILURE;
    if (!login || !auth)
        return WOLFSSH_USERAUTH_FAILURE;
    if (login_len != strlen(cfg->ssh_user) || memcmp(login, cfg->ssh_user, login_len) != 0)
        return WOLFSSH_USERAUTH_INVALID_USER;
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
            if (session_shell_ids[i])
                shell_end(&session_shells[i]);      // cancel a running job (closes its files)
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
                shell_end(&session_shells[i]);
            session_shell_ids[i] = ss->session_id;
            shell_init(&session_shells[i], &session_io, ss);
        }
        session_poll_output[i] = 0;
        shell_poll(&session_shells[i]);
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
            snprintf(who, sizeof(who), "%s@%s:%u", ss->login[0] ? ss->login : "?", ipstr, port);
        }
        uint64_t secs = (time_us_64() - ss->accept_start_us) / 1000000;
        shell_printf(sh, "  %c %u  %-36s %-10s %-5s %llum%02llus\n",
                     sh == &session_shells[i] ? '*' : ' ', i + 1, who,
                     ssh_server_connection_state_name(ss->cstate), ss->sftp ? "sftp" : "shell",
                     secs / 60, secs % 60);
    }
}
