#ifndef SOS_SSHD_H
#define SOS_SSHD_H

#include <stdbool.h>
#include <stdint.h>

// Start the SSH server on port 22 with the given host key (DER). The SSH session becomes
// an additional stdio channel, so the shell is reachable over SSH as well as USB/UART.
// Only public-key login for the user/key configured in local.cmake is accepted.
bool sshd_start(const uint8_t *hostkey, uint32_t hostkey_len);

// Print server state, connected client and host key fingerprint.
void sshd_status(void);

// Disconnect the current SSH client, if any.
void sshd_disconnect(void);

#endif
