#ifndef USERPROG_HELLO_H
#define USERPROG_HELLO_H

#include <stddef.h>

extern const unsigned char out_hello_so[];
extern size_t out_hello_so_len;

const unsigned char *get_hello_ptr(void);
size_t get_hello_len(void);

#endif
