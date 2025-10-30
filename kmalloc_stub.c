#include <stdint.h>
#include <stddef.h>
#include "stdlib.h"

static uint8_t *heap=(uint8_t*)0x01000000; // 16MB; adjust to your memory map

void *kmalloc(size_t sz){ void *p=heap; heap += (sz+15)&~15; return p; }

void *malloc(size_t sz){ return kmalloc(sz); }
void free(void *p){ (void)p; }
void *calloc(size_t n, size_t size){ size_t tot = n*size; void *p=kmalloc(tot); if(p) memset(p,0,tot); return p; }
void abort(void){ for(;;); }

static unsigned int rng_state = 1;
int rand(void){ rng_state = rng_state*1103515245 + 12345; return (int)(rng_state>>16); }
void srand(unsigned int seed){ rng_state = seed ? seed : 1; }