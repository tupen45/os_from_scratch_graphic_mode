#ifndef EXEC_ELF_H
#define EXEC_ELF_H

#include <stddef.h>

// Minimal ELF loader — loads ELF32 (ET_DYN) position-independent executables into
// kernel heap and jumps to their entry point. The loaded program must be compiled
// as a position-independent PIE (ET_DYN) and must not rely on syscalls.

// Load ELF image from memory and run it. Returns the integer return value from
// the program entry (if it returns int), or negative on error.
int elf32_load_and_run(const void *elf_data, size_t elf_size);

#endif
