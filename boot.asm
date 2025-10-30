bits 32

section .multiboot
align 8
multiboot2_header:
    dd 0xE85250D6
    dd 0
    dd header_end - multiboot2_header
    dd -(0xE85250D6 + 0 + (header_end - multiboot2_header))
    dw 5, 0
    dd 24
    dd 1450,1000
    dd 32
    dd 0
    dw 0,0
    dd 8
header_end:

section .text
global _start
extern kmain

_start:
    cli
    lgdt [gdt_descr]
    mov ax, 0x10
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    mov esp, stack_top
    push ebx
    push eax
    call kmain
.hang: hlt
    jmp .hang

section .gdt
gdt_start: dq 0x0000000000000000
           dq 0x00cf9a000000ffff
           dq 0x00cf92000000ffff
gdt_descr:
    dw gdt_end - gdt_start -1
    dd gdt_start
gdt_end:

section .bss
resb 16384
stack_top:
