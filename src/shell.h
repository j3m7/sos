#ifndef SOS_SHELL_H
#define SOS_SHELL_H

// Minimal line-oriented shell on stdio (USB, UART and SSH). Call regularly from the main loop;
// never blocks.
void shell_init(void);
void shell_poll(void);

#endif
