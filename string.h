#ifndef STRING_H
#define STRING_H
#include <stddef.h>

#include "common.h"

void *memset(void *s, int c, u32 n);
void *memcpy(void *dest, const void *src, u32 n);
u32 strlen(const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
int atoi(const char *s);char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
int atoi(const char *s);
void *memmove(void *dst, const void *src, size_t n);
int   snprintf(char *buf, size_t size, const char *fmt, ...);

// Added helpers
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *dst, const char *src, size_t n);


#endif
