#include "exec_elf.h"
#include "string.h"
#include "syscalls.h"
#include <stdint.h>
#include <stddef.h>

// Declare kmalloc provided by kmalloc_stub.c (don't include the .c to avoid duplicate symbol defs)
void *kmalloc(size_t sz);

// Very small ELF32 loader: supports program headers PT_LOAD only.
// Not robust; for demo only.

#define EI_NIDENT 16
struct elf32_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct elf32_phdr{
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

#define PT_LOAD 1

int elf32_load_and_run(const void *elf_data, size_t elf_size){
    const uint8_t *buf = (const uint8_t*)elf_data;
    if(elf_size < sizeof(struct elf32_hdr)) return -1;
    const struct elf32_hdr *eh = (const struct elf32_hdr*)buf;
    // check magic
    if(!(eh->e_ident[0]==0x7f && eh->e_ident[1]=='E' && eh->e_ident[2]=='L' && eh->e_ident[3]=='F')) return -2;
    // program headers
    if(eh->e_phoff == 0 || eh->e_phnum == 0) return -3;

    // allocate space for program: sum memsz
    uint32_t min_vaddr = 0xFFFFFFFF, max_vaddr = 0;
    for(int i=0;i<eh->e_phnum;i++){
        const struct elf32_phdr *ph = (const struct elf32_phdr*)(buf + eh->e_phoff + i*eh->e_phentsize);
        if(ph->p_type != PT_LOAD) continue;
        if(ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        if(ph->p_vaddr + ph->p_memsz > max_vaddr) max_vaddr = ph->p_vaddr + ph->p_memsz;
    }
    if(min_vaddr == 0xFFFFFFFF) return -4;
    uint32_t total = max_vaddr - min_vaddr;
    uint8_t *mem = kmalloc(total);
    if(!mem) return -5;
    // zero memory
    for(uint32_t i=0;i<total;i++) mem[i]=0;

    // load segments
    for(int i=0;i<eh->e_phnum;i++){
        const struct elf32_phdr *ph = (const struct elf32_phdr*)(buf + eh->e_phoff + i*eh->e_phentsize);
        if(ph->p_type != PT_LOAD) continue;
        if(ph->p_offset + ph->p_filesz > elf_size) return -6;
        uint32_t dest = ph->p_vaddr - min_vaddr;
        memcpy(mem + dest, buf + ph->p_offset, ph->p_filesz);
    }

    // compute entry relative pointer
    uint32_t entry_rel = eh->e_entry - min_vaddr;
    int (*entry)(void) = (int(*)(void))(mem + entry_rel);

    // init syscalls
    init_syscalls();

    // call entry and run until it returns or performs exit syscall
    int r = entry();

    // if program used exit syscall, return its code, else return entry return
    if(user_exited) return user_exit_code;
    return r;
}
