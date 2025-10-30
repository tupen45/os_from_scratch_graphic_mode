#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>

/* Minimal freestanding stdio stub for building mbedTLS in-kernel.
 * Provides only the types and prototypes expected by mbedTLS sources. */

typedef struct { int _dummy; } FILE;

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *format, ...);

#endif /* STDIO_H */
