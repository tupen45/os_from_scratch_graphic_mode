/* Tiny user program for exec_elf loader
 * Builds to a 32-bit position-independent ET_DYN ELF with entry point 'entry'
 * It uses int 0x80 with the kernel's minimal syscall convention:
 *   EAX = syscall number, EBX = arg1, ECX = arg2, EDX = arg3
 * Syscalls implemented: write (1), exit (2)
 */

typedef unsigned int uint32_t;

int entry(void){
    const char msg[] = "Hello from userprog!\n";
    unsigned int len = sizeof(msg)-1;

    /* syscall: write(ptr, len) */
    asm volatile(
        "movl %0, %%ebx\n\t"   /* arg1 = msg */
        "movl %1, %%ecx\n\t"   /* arg2 = len */
        "movl $1, %%eax\n\t"   /* syscall 1 = write */
        "int $0x80\n\t"
        : /* no outputs */
        : "r" (msg), "r" (len)
        : "eax", "ebx", "ecx"
    );

    /* syscall: exit(code) */
    asm volatile(
        "movl $42, %%ebx\n\t" /* exit code 42 */
        "movl $2, %%eax\n\t"  /* syscall 2 = exit */
        "int $0x80\n\t"
        : /* no outputs */
        : /* no inputs */
        : "eax", "ebx"
    );

    return 0; /* not reached */
}
