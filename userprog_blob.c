#include <stdint.h>
#include "userprog_hello.h"

// Provide weak fallback definitions for the embedded blob so the kernel
// can be built even when the binary hasn't been embedded yet. The
// embed script will overwrite this file with a strong definition when
// invoked.
__attribute__((weak)) const unsigned char out_hello_so[] = { 0 };
__attribute__((weak)) size_t out_hello_so_len = 0;

const unsigned char *get_hello_ptr(void){ return out_hello_so; }
size_t get_hello_len(void){ return out_hello_so_len; }
