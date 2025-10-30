#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

extern volatile int user_exited;
extern volatile int user_exit_code;

void init_syscalls(void);

// syscall numbers
#define SYSCALL_WRITE 1
#define SYSCALL_EXIT  2

// Simple on-screen console helper used by syscall layer and other kernel code
void console_puts(const char *s);

#endif
