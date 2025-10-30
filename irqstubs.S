.section .text
    .globl irq80_stub
irq80_stub:
    /* Simple ISR stub for int 0x80 -> pushad; call C handler; addad; iret */
    pusha
    push %ds
    push %es
    /* ensure data segment registers are sane (flat) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    /* pass pointer to pushed regs to C: push pointer then call */
    mov %esp, %eax
    push %eax
    call syscall_handler_c
    add $4, %esp
    pop %es
    pop %ds
    popa
    iret
