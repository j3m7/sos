// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/ssh/ringbuffer.c.

#include <stdint.h>
#include <stdlib.h>

#include "pico-sshd/ringbuffer.h"
#include "test.h"

// Fill a buffer with a recognizable pattern starting at `start`.
static void pattern(uint8_t *p, size_t n, unsigned start) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(start + i);
}

// --- basics ----------------------------------------------------------------------------------

TEST(init_allocates_and_destroy_frees) {
    ssh_ringbuffer_t rb;
    CHECK_EQ(ssh_ringbuffer_init(&rb, NULL, 16), 0);
    CHECK(rb.buf != NULL);
    CHECK(rb.free_buf);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 0);
    CHECK_EQ(ssh_ringbuffer_free(&rb), 16);
    CHECK_EQ(ssh_ringbuffer_destroy(&rb), 0);   // ASan reports a leak if this doesn't free
    CHECK(rb.buf == NULL);
    CHECK_EQ(rb.size, 0);
}

TEST(init_with_caller_buffer_is_not_freed) {
    uint8_t mem[8];
    ssh_ringbuffer_t rb;
    CHECK_EQ(ssh_ringbuffer_init(&rb, mem, sizeof(mem)), 0);
    CHECK(rb.buf == mem);
    CHECK(!rb.free_buf);
    CHECK_EQ(ssh_ringbuffer_destroy(&rb), 0);   // ASan reports a bad free if it frees `mem`
}

TEST(null_arguments) {
    ssh_ringbuffer_t rb;
    uint8_t b[4] = {0};
    void *p;
    CHECK_EQ(ssh_ringbuffer_init(NULL, NULL, 4), -1);
    CHECK_EQ(ssh_ringbuffer_destroy(NULL), -1);
    CHECK_EQ(ssh_ringbuffer_flush(NULL), -1);
    CHECK_EQ(ssh_ringbuffer_used(NULL), 0);
    CHECK_EQ(ssh_ringbuffer_free(NULL), 0);
    CHECK_EQ(ssh_ringbuffer_offset(NULL, 3, 1, 1), 0);
    CHECK_EQ(ssh_ringbuffer_add(NULL, b, 1, false), -1);
    CHECK_EQ(ssh_ringbuffer_read(NULL, b, 1), -1);
    CHECK_EQ(ssh_ringbuffer_read_char(NULL), -1);
    CHECK_EQ(ssh_ringbuffer_add_char(NULL, 1, false), -1);
    CHECK_EQ(ssh_ringbuffer_peek(NULL, &p, 1), 0);
    ssh_ringbuffer_init(&rb, NULL, 4);
    CHECK_EQ(ssh_ringbuffer_add(&rb, NULL, 1, false), -1);
    CHECK_EQ(ssh_ringbuffer_peek(&rb, NULL, 1), 0);
    ssh_ringbuffer_destroy(&rb);
}

TEST(add_and_read_in_order) {
    ssh_ringbuffer_t rb;
    uint8_t in[10], out[10];
    ssh_ringbuffer_init(&rb, NULL, 16);
    pattern(in, sizeof(in), 1);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in, 6, false), 0);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in + 6, 4, false), 0);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 10);
    CHECK_EQ(ssh_ringbuffer_free(&rb), 6);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 3), 0);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out + 3, 7), 0);
    CHECK_MEM(out, in, 10);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 0);
    ssh_ringbuffer_destroy(&rb);
}

TEST(error_codes) {
    ssh_ringbuffer_t rb;
    uint8_t b[32] = {0};
    ssh_ringbuffer_init(&rb, NULL, 8);
    CHECK_EQ(ssh_ringbuffer_add(&rb, b, 0, false), 0);      // zero length: no-op
    CHECK_EQ(ssh_ringbuffer_used(&rb), 0);
    CHECK_EQ(ssh_ringbuffer_add(&rb, b, 9, false), -2);     // larger than the buffer
    CHECK_EQ(ssh_ringbuffer_add(&rb, b, 9, true), -2);      // even with overwrite
    CHECK_EQ(ssh_ringbuffer_read(&rb, b, 0), -1);           // zero-length read
    CHECK_EQ(ssh_ringbuffer_read(&rb, b, 1), -2);           // more than is buffered
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), -2);            // empty
    CHECK_EQ(ssh_ringbuffer_add(&rb, b, 6, false), 0);
    CHECK_EQ(ssh_ringbuffer_add(&rb, b, 3, false), -3);     // doesn't fit, no overwrite
    CHECK_EQ(ssh_ringbuffer_used(&rb), 6);                  // unchanged by the failed add
    CHECK_EQ(ssh_ringbuffer_read(&rb, b, 7), -2);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 6);                  // unchanged by the failed read
    ssh_ringbuffer_destroy(&rb);
}

TEST(read_with_null_destination_discards) {
    ssh_ringbuffer_t rb;
    uint8_t in[6], out[2];
    ssh_ringbuffer_init(&rb, NULL, 8);
    pattern(in, sizeof(in), 10);
    ssh_ringbuffer_add(&rb, in, 6, false);
    CHECK_EQ(ssh_ringbuffer_read(&rb, NULL, 4), 0);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 2), 0);
    CHECK_MEM(out, in + 4, 2);
    ssh_ringbuffer_destroy(&rb);
}

TEST(flush_empties) {
    ssh_ringbuffer_t rb;
    uint8_t b[5] = {1, 2, 3, 4, 5};
    ssh_ringbuffer_init(&rb, NULL, 8);
    ssh_ringbuffer_add(&rb, b, 5, false);
    CHECK_EQ(ssh_ringbuffer_flush(&rb), 0);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 0);
    CHECK_EQ(ssh_ringbuffer_free(&rb), 8);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), -2);
    ssh_ringbuffer_destroy(&rb);
}

// --- wrap-around edge cases ------------------------------------------------------------------

// Position head/tail at `pos` in an empty buffer by writing and reading `pos` bytes.
static void advance(ssh_ringbuffer_t *rb, size_t pos) {
    uint8_t junk[64] = {0};
    ssh_ringbuffer_add(rb, junk, pos, false);
    ssh_ringbuffer_read(rb, NULL, pos);
}

TEST(write_ending_exactly_at_end) {
    ssh_ringbuffer_t rb;
    uint8_t in[8], out[8];
    ssh_ringbuffer_init(&rb, NULL, 8);
    advance(&rb, 5);
    pattern(in, 3, 40);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in, 3, false), 0);     // tail 5 -> 0
    CHECK_EQ(rb.tail, 0);
    pattern(in + 3, 2, 43);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in + 3, 2, false), 0);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 5), 0);
    CHECK_MEM(out, in, 5);
    ssh_ringbuffer_destroy(&rb);
}

TEST(write_and_read_across_the_end) {
    ssh_ringbuffer_t rb;
    uint8_t in[8], out[8];
    ssh_ringbuffer_init(&rb, NULL, 8);
    advance(&rb, 6);
    pattern(in, 5, 60);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in, 5, false), 0);     // bytes at 6,7,0,1,2
    CHECK_EQ(rb.tail, 3);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 5), 0);
    CHECK_MEM(out, in, 5);
    CHECK_EQ(rb.head, 3);
    ssh_ringbuffer_destroy(&rb);
}

TEST(fill_completely_from_middle) {
    ssh_ringbuffer_t rb;
    uint8_t in[8], out[8];
    ssh_ringbuffer_init(&rb, NULL, 8);
    advance(&rb, 3);
    pattern(in, 8, 80);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in, 8, false), 0);     // whole buffer, wraps
    CHECK_EQ(ssh_ringbuffer_free(&rb), 0);
    CHECK_EQ(rb.head, rb.tail);                             // full: head == tail
    CHECK_EQ(ssh_ringbuffer_add_char(&rb, 1, false), -2);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 8), 0);          // whole buffer back, wraps
    CHECK_MEM(out, in, 8);
    CHECK_EQ(ssh_ringbuffer_used(&rb), 0);
    ssh_ringbuffer_destroy(&rb);
}

TEST(read_char_from_full_buffer) {
    ssh_ringbuffer_t rb;
    uint8_t in[4] = {7, 8, 9, 10};
    ssh_ringbuffer_init(&rb, NULL, 4);
    ssh_ringbuffer_add(&rb, in, 4, false);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 7);             // full must not look empty
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 8);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 9);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 10);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), -2);
    ssh_ringbuffer_destroy(&rb);
}

TEST(read_char_returns_high_bytes_as_positive) {
    ssh_ringbuffer_t rb;
    ssh_ringbuffer_init(&rb, NULL, 4);
    ssh_ringbuffer_add_char(&rb, 0xff, false);
    ssh_ringbuffer_add_char(&rb, 0x80, false);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 0xff);          // not -1: callers test for < 0
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 0x80);
    ssh_ringbuffer_destroy(&rb);
}

// --- overwrite -------------------------------------------------------------------------------

TEST(overwrite_drops_oldest) {
    ssh_ringbuffer_t rb;
    uint8_t in[10], out[8];
    ssh_ringbuffer_init(&rb, NULL, 8);
    pattern(in, 10, 1);
    CHECK_EQ(ssh_ringbuffer_add(&rb, in, 6, false), 0);     // 1..6
    CHECK_EQ(ssh_ringbuffer_add(&rb, in + 6, 4, true), 0);  // 7..10 drops 1,2
    CHECK_EQ(ssh_ringbuffer_used(&rb), 8);
    CHECK_EQ(ssh_ringbuffer_read(&rb, out, 8), 0);
    CHECK_MEM(out, in + 2, 8);
    ssh_ringbuffer_destroy(&rb);
}

TEST(add_char_overwrite) {
    ssh_ringbuffer_t rb;
    ssh_ringbuffer_init(&rb, NULL, 3);
    for (int c = 1; c <= 5; c++)
        CHECK_EQ(ssh_ringbuffer_add_char(&rb, (uint8_t)c, true), 0);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 3);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 4);
    CHECK_EQ(ssh_ringbuffer_read_char(&rb), 5);
    ssh_ringbuffer_destroy(&rb);
}

// --- peek and offset -------------------------------------------------------------------------

TEST(peek_returns_contiguous_part) {
    ssh_ringbuffer_t rb;
    uint8_t in[6];
    void *p;
    ssh_ringbuffer_init(&rb, NULL, 8);
    CHECK_EQ(ssh_ringbuffer_peek(&rb, &p, 4), 0);           // empty
    advance(&rb, 5);
    pattern(in, 6, 20);
    ssh_ringbuffer_add(&rb, in, 6, false);                  // at 5,6,7 | 0,1,2
    CHECK_EQ(ssh_ringbuffer_peek(&rb, &p, 10), 3);          // only up to the end
    CHECK_MEM(p, in, 3);
    CHECK_EQ(ssh_ringbuffer_peek(&rb, &p, 2), 2);           // limited by the request
    CHECK_EQ(ssh_ringbuffer_used(&rb), 6);                  // peek doesn't consume
    ssh_ringbuffer_read(&rb, NULL, 3);
    CHECK_EQ(ssh_ringbuffer_peek(&rb, &p, 10), 3);          // rest, from the start
    CHECK_MEM(p, in + 3, 3);
    ssh_ringbuffer_destroy(&rb);
}

TEST(offset_wraps_both_ways) {
    ssh_ringbuffer_t rb;
    ssh_ringbuffer_init(&rb, NULL, 8);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 6, 3, 1), 1);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 6, 2, 1), 0);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 1, 3, -1), 6);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 3, 3, -1), 0);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 5, 8, 1), 5);       // whole lap
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 5, 0, -1), 5);
    CHECK_EQ(ssh_ringbuffer_offset(&rb, 12, 1, 1), 5);      // offset beyond size is reduced
    ssh_ringbuffer_destroy(&rb);
}

// --- randomized model test -------------------------------------------------------------------

// Reference queue: a plain array, oldest byte first.
typedef struct { uint8_t data[64]; size_t len, cap; } model_t;

static void model_drop(model_t *m, size_t n) {
    memmove(m->data, m->data + n, m->len - n);
    m->len -= n;
}

TEST(random_operations_match_model) {
    srand(12345);
    for (size_t size = 1; size <= 17; size++) {
        ssh_ringbuffer_t rb;
        model_t m = { .len = 0, .cap = size };
        uint8_t in[64], out[64];
        unsigned next = 0;
        ssh_ringbuffer_init(&rb, NULL, size);

        for (int step = 0; step < 4000; step++) {
            int op = rand() % 6;
            size_t n = (size_t)(rand() % (int)(size + 3));   // sometimes larger than size
            if (op == 0 || op == 1) {                        // add, with or without overwrite
                bool ow = (op == 1);
                pattern(in, n, next);
                int expect = n == 0 ? 0 : n > size ? -2 : (!ow && m.cap - m.len < n) ? -3 : 0;
                int got = ssh_ringbuffer_add(&rb, in, n, ow);
                CHECK_EQ(got, expect);
                if (got == 0 && n > 0) {
                    if (m.cap - m.len < n)
                        model_drop(&m, n - (m.cap - m.len));
                    memcpy(m.data + m.len, in, n);
                    m.len += n;
                    next += n;
                }
            } else if (op == 2) {                            // read
                int expect = n == 0 ? -1 : n > m.len ? -2 : 0;
                int got = ssh_ringbuffer_read(&rb, out, n);
                CHECK_EQ(got, expect);
                if (got == 0) {
                    CHECK_MEM(out, m.data, n);
                    model_drop(&m, n);
                }
            } else if (op == 3) {                            // read_char
                int got = ssh_ringbuffer_read_char(&rb);
                if (m.len == 0) {
                    CHECK_EQ(got, -2);
                } else {
                    CHECK_EQ(got, m.data[0]);
                    model_drop(&m, 1);
                }
            } else if (op == 4) {                            // add_char without overwrite
                int got = ssh_ringbuffer_add_char(&rb, (uint8_t)next, false);
                CHECK_EQ(got, m.len == m.cap ? -2 : 0);
                if (got == 0) {
                    m.data[m.len++] = (uint8_t)next;
                    next++;
                }
            } else {                                         // peek
                void *p;
                size_t want = n ? n : 1;
                size_t got = ssh_ringbuffer_peek(&rb, &p, want);
                size_t max = want < m.len ? want : m.len;
                CHECK(got <= max);
                CHECK(m.len == 0 ? got == 0 : got >= 1);
                if (got)
                    CHECK_MEM(p, m.data, got);
            }
            CHECK_EQ(ssh_ringbuffer_used(&rb), m.len);
            CHECK_EQ(ssh_ringbuffer_free(&rb), m.cap - m.len);
            if (test_failures)
                break;                                       // one report is enough
        }
        ssh_ringbuffer_destroy(&rb);
        if (test_failures) {
            fprintf(stderr, "  (buffer size %zu)\n", size);
            break;
        }
    }
}

TEST_MAIN(
    RUN(init_allocates_and_destroy_frees);
    RUN(init_with_caller_buffer_is_not_freed);
    RUN(null_arguments);
    RUN(add_and_read_in_order);
    RUN(error_codes);
    RUN(read_with_null_destination_discards);
    RUN(flush_empties);
    RUN(write_ending_exactly_at_end);
    RUN(write_and_read_across_the_end);
    RUN(fill_completely_from_middle);
    RUN(read_char_from_full_buffer);
    RUN(read_char_returns_high_bytes_as_positive);
    RUN(overwrite_drops_oldest);
    RUN(add_char_overwrite);
    RUN(peek_returns_contiguous_part);
    RUN(offset_wraps_both_ways);
    RUN(random_operations_match_model);
)
