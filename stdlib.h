#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/* Minimal freestanding stdlib.h for the kernel build. Declarations only. */

#define NULL ((void*)0)

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);

/* Abort/exit: no-op or infinite loop in freestanding environment */
void abort(void);
void exit(int status);

/* Simple PRNG used by mbedtls fallback code (if any) */
int rand(void);
void srand(unsigned int seed);

#endif /* STDLIB_H */
