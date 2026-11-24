#include "hostkey.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include <wolfssh/ssh.h>
#include <wolfssh/keygen.h>

// The host key lives in the last 4 KB sector of flash, well clear of the firmware image.
// `picotool load` only rewrites the sectors the image occupies, so it survives reflashing.
#define HOSTKEY_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define HOSTKEY_MAGIC   0x4b534f53u   // "SOSK"
#define HOSTKEY_VERSION 1u
#define HOSTKEY_MAX_DER (FLASH_PAGE_SIZE - 16)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t len;
    uint32_t check;                 // FNV-1a over der[0..len)
    uint8_t der[HOSTKEY_MAX_DER];
} hostkey_record_t;

_Static_assert(sizeof(hostkey_record_t) == FLASH_PAGE_SIZE, "record must be one flash page");

static hostkey_record_t ram_copy;

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static const hostkey_record_t *flash_record(void) {
    return (const hostkey_record_t *)(XIP_BASE + HOSTKEY_OFFSET);
}

static bool record_valid(const hostkey_record_t *r) {
    return r->magic == HOSTKEY_MAGIC && r->version == HOSTKEY_VERSION &&
           r->len > 0 && r->len <= HOSTKEY_MAX_DER && r->check == fnv1a(r->der, r->len);
}

static void do_write(void *param) {
    flash_range_erase(HOSTKEY_OFFSET, FLASH_SECTOR_SIZE);
    if (param)
        flash_range_program(HOSTKEY_OFFSET, (const uint8_t *)param, FLASH_PAGE_SIZE);
}

static bool flash_write(const hostkey_record_t *r) {
    extern char __flash_binary_end;
    if ((uintptr_t)&__flash_binary_end > XIP_BASE + HOSTKEY_OFFSET) {
        printf("hostkey: firmware image overlaps host key sector\n");
        return false;
    }
    return flash_safe_execute(do_write, (void *)r, 1000) == PICO_OK;
}

bool hostkey_get(const uint8_t **der, uint32_t *len) {
    const hostkey_record_t *fr = flash_record();

    if (!record_valid(fr)) {
        printf("hostkey: no valid host key in flash, generating Ed25519 key...\n");
        memset(&ram_copy, 0xff, sizeof(ram_copy));
        int n = wolfSSH_MakeEd25519Key(ram_copy.der, sizeof(ram_copy.der), WOLFSSH_ED25519KEY);
        if (n <= 0) {
            printf("hostkey: key generation failed (%d)\n", n);
            return false;
        }
        ram_copy.magic = HOSTKEY_MAGIC;
        ram_copy.version = HOSTKEY_VERSION;
        ram_copy.len = (uint32_t)n;
        ram_copy.check = fnv1a(ram_copy.der, ram_copy.len);

        if (!flash_write(&ram_copy) || !record_valid(fr)) {
            printf("hostkey: failed to store host key in flash\n");
            return false;
        }
        printf("hostkey: stored new host key (%lu bytes)\n", (unsigned long)ram_copy.len);
        memset(&ram_copy, 0, sizeof(ram_copy));
    }

    *der = fr->der;
    *len = fr->len;
    return true;
}

bool hostkey_erase(void) {
    return flash_safe_execute(do_write, NULL, 1000) == PICO_OK;
}
