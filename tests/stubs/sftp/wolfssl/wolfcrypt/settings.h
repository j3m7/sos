// Host-test stand-in for wolfSSL's settings header, for tests/test_sftp_fs.c only (the other
// tests use the real one): src/ssh/sftp_fs.c includes it before wolfssh/error.h, as wolfSSH
// requires; the stand-in for that needs nothing from it.
#pragma once
