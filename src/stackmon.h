#ifndef SOS_STACKMON_H
#define SOS_STACKMON_H

#include <stddef.h>

// Core 0 stack high-water mark. stackmon_paint() fills the unused part of the stack with a
// pattern; call it first thing in main(). stackmon_peak() reports the deepest usage seen since.
// Interrupt handlers (including lwIP/cyw43 background processing) run on this same stack.
void stackmon_paint(void);
size_t stackmon_peak(void);
size_t stackmon_size(void);

#endif
