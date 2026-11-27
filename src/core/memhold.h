// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_MEMHOLD_H
#define SOS_MEMHOLD_H

#include <stdbool.h>
#include <stddef.h>

// Test builds only (-DSOS_TEST_COMMANDS=ON): take heap memory and keep it, to make the
// out-of-memory paths happen on a board (issue #188). `mem hold` and `mem release` on the
// console. The blocks come from plain malloc, so `mem` counts them under "other".

#define MEMHOLD_MAX_BLOCKS 160

// Hold one more block of n bytes. False if malloc refused or the block list is full.
bool memhold_block(size_t n);

// Hold blocks until no more than `keep` bytes of the heap are left unallocated (malloc's
// bookkeeping makes it a few bytes either way). Returns the bytes taken.
size_t memhold_leave(size_t keep);

// Hold the whole heap except `count` holes of `size` bytes each, each hole between two held
// blocks: count * size bytes are free, but no request larger than `size` fits. Returns the
// number of holes made (fewer than count if the heap or the block list ran out).
int memhold_holes(int count, size_t size);

// Free everything held. Returns the bytes freed (and cancels a pending memhold_release_first).
size_t memhold_release(void);

// Free the block held first (the oldest), now, or `delay_ms` later from memhold_poll(): to free
// memory while a command waits for an answer on the console. Returns its size (0: none held).
size_t memhold_release_first(unsigned delay_ms);

// From the main loop: does a delayed memhold_release_first() when it's due. Not from an
// interrupt (it calls free).
void memhold_poll(void);

size_t memhold_bytes(void);     // held now
int memhold_count(void);        // blocks held now

// The largest single block malloc can give now, to within 16 bytes (probed by allocating).
size_t memhold_largest(void);

#endif
