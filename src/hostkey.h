#ifndef SOS_HOSTKEY_H
#define SOS_HOSTKEY_H

#include <stdbool.h>
#include <stdint.h>

// Returns the device's SSH host key (Ed25519, DER) stored in the last flash sector,
// generating and persisting a new one on first boot. The private key never leaves the device.
// Must be called after wolfCrypt_Init() and before Wi-Fi is started.
bool hostkey_get(const uint8_t **der, uint32_t *len);

// Erase the stored host key; a new one is generated on next boot.
bool hostkey_erase(void);

#endif
