// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/certbundle.c: PEM bundles and DER certificates -> DER.

#include <stdint.h>

#include "certbundle.h"
#include "test.h"

// Synthetic "certificates": DER SEQUENCEs (the parser checks the framing; wolfSSL checks the
// contents when TLS loads them).
static const uint8_t der_a[] = {0x30, 0x03, 0x02, 0x01, 0x05};           // SEQUENCE { INTEGER 5 }
#define PEM_A "MAMCAQU="
static const uint8_t der_b[] = {0x30, 0x04, 0x04, 0x02, 0xab, 0xcd};     // SEQUENCE { OCTETS }
#define PEM_B "MAQEAqvN"

static uint8_t out[4096];
static certbundle_cert_t certs[CERTBUNDLE_MAX_CERTS];

static size_t parse_str(const char *s, size_t max, const char **why) {
    return certbundle_parse((const uint8_t *)s, strlen(s), out, sizeof(out), certs, max, why);
}

TEST(pem_bundle_with_comments) {
    const char *why = "unset";
    const char *bundle =
        "# ISRG Root X1 (comment lines, as in system CA bundles)\n"
        "-----BEGIN CERTIFICATE-----\n" PEM_A "\n-----END CERTIFICATE-----\n"
        "\n# another\r\n"
        "-----BEGIN CERTIFICATE-----\r\n  " PEM_B "\r\n-----END CERTIFICATE-----\r\n";
    CHECK_EQ(parse_str(bundle, CERTBUNDLE_MAX_CERTS, &why), 2);
    CHECK(why == NULL);
    CHECK_EQ(certs[0].len, sizeof(der_a));
    CHECK_MEM(out + certs[0].off, der_a, sizeof(der_a));
    CHECK_EQ(certs[1].len, sizeof(der_b));
    CHECK_MEM(out + certs[1].off, der_b, sizeof(der_b));
}

TEST(pem_base64_over_several_lines) {
    const char *why;
    // 128 content bytes: the base64 wraps over lines like real PEM.
    uint8_t der[131] = {0x30, 0x81, 0x80};
    for (int i = 0; i < 128; i++)
        der[3 + i] = (uint8_t)i;
    // base64 of der, 64 characters per line.
    static const char b64[] =
        "MIGAAAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKiss\n"
        "LS4vMDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltc\n"
        "XV5fYGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn8=\n";
    char pem[512];
    snprintf(pem, sizeof(pem), "-----BEGIN CERTIFICATE-----\n%s-----END CERTIFICATE-----\n", b64);
    CHECK_EQ(parse_str(pem, CERTBUNDLE_MAX_CERTS, &why), 1);
    CHECK_EQ(certs[0].len, sizeof(der));
    CHECK_MEM(out + certs[0].off, der, sizeof(der));
}

TEST(der_files) {
    const char *why;
    uint8_t two[sizeof(der_a) + sizeof(der_b)];
    memcpy(two, der_a, sizeof(der_a));
    memcpy(two + sizeof(der_a), der_b, sizeof(der_b));
    CHECK_EQ(certbundle_parse(der_a, sizeof(der_a), out, sizeof(out), certs, 8, &why), 1);
    CHECK_EQ(certbundle_parse(two, sizeof(two), out, sizeof(out), certs, 8, &why), 2);
    CHECK_EQ(certs[1].off, sizeof(der_a));
    CHECK_MEM(out + certs[1].off, der_b, sizeof(der_b));

    uint8_t trailing[sizeof(der_a) + 1];
    memcpy(trailing, der_a, sizeof(der_a));
    trailing[sizeof(der_a)] = 0x00;             // garbage after the certificate
    CHECK_EQ(certbundle_parse(trailing, sizeof(trailing), out, sizeof(out), certs, 8, &why), 0);
    CHECK(why != NULL);
    uint8_t truncated[] = {0x30, 0x05, 0x02, 0x01};
    CHECK_EQ(certbundle_parse(truncated, sizeof(truncated), out, sizeof(out), certs, 8, &why), 0);
}

TEST(rejected_files) {
    const char *why;
    CHECK_EQ(certbundle_parse((const uint8_t *)"", 0, out, sizeof(out), certs, 8, &why), 0);
    CHECK(!strcmp(why, "the file is empty"));

    CHECK_EQ(parse_str("just some text\n", 8, &why), 0);      // neither PEM nor DER
    CHECK(why != NULL);

    CHECK_EQ(parse_str("-----BEGIN CERTIFICATE-----\n" PEM_A "\n", 8, &why), 0);
    CHECK(!strcmp(why, "unterminated certificate block"));

    // A private key in the bundle is refused, even next to certificates.
    CHECK_EQ(parse_str("-----BEGIN CERTIFICATE-----\n" PEM_A "\n-----END CERTIFICATE-----\n"
                       "-----BEGIN PRIVATE KEY-----\nMAMCAQU=\n-----END PRIVATE KEY-----\n",
                       8, &why), 0);
    CHECK(!strcmp(why, "the file holds a PEM block that isn't a certificate"));

    CHECK_EQ(parse_str("-----BEGIN CERTIFICATE-----\n@@@@\n-----END CERTIFICATE-----\n", 8, &why), 0);
    CHECK(!strcmp(why, "a certificate block isn't valid base64 DER"));
    // Valid base64, but not a DER SEQUENCE.
    CHECK_EQ(parse_str("-----BEGIN CERTIFICATE-----\nAgEF\n-----END CERTIFICATE-----\n", 8, &why), 0);

    // Only comments: no certificate.
    CHECK_EQ(parse_str("# nothing here\n-----BEGIN ", 8, &why), 0);
}

TEST(certificate_limit) {
    const char *why;
    const char *three =
        "-----BEGIN CERTIFICATE-----\n" PEM_A "\n-----END CERTIFICATE-----\n"
        "-----BEGIN CERTIFICATE-----\n" PEM_B "\n-----END CERTIFICATE-----\n"
        "-----BEGIN CERTIFICATE-----\n" PEM_A "\n-----END CERTIFICATE-----\n";
    CHECK_EQ(parse_str(three, 3, &why), 3);
    CHECK_EQ(parse_str(three, 2, &why), 0);
    CHECK(!strcmp(why, "too many certificates"));
}

TEST_MAIN(
    RUN(pem_bundle_with_comments);
    RUN(pem_base64_over_several_lines);
    RUN(der_files);
    RUN(rejected_files);
    RUN(certificate_limit);
)
