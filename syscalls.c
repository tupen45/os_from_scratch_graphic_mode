#include <stdint.h>
#include "graphics.h"
#include "syscalls.h"

// Simple user-exit state (checked by exec_elf or for debugging)
volatile int user_exited = 0;
volatile int user_exit_code = 0;

// Small on-screen console for syscall write
static int cons_x = 6;
static int cons_y = 6;
static int cons_w = 80; // chars per line estimate
static int cons_h = 40; // lines

// make console_puts non-static so other files (kernel.c) can call it
void console_puts(const char *s){
    // draw string at current console pos and advance to next line
    draw_string(cons_x, cons_y, s, 0x000000);
    cons_y += 10;
    if(cons_y > (int)framebuffer_height - 10){
        // simple wrap: reset to top
        cons_y = 6;
    }
}

// IDT structures for 32-bit protected mode
struct idt_entry { uint16_t offset_low; uint16_t sel; uint8_t zero; uint8_t flags; uint16_t offset_high; };
struct idt_ptr { uint16_t limit; uint32_t base; };

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void irq80_stub(void);

static void set_idt_entry(int idx, uint32_t base, uint16_t sel, uint8_t flags){
    idt[idx].offset_low = base & 0xFFFF;
    idt[idx].sel = sel;
    idt[idx].zero = 0;
    idt[idx].flags = flags;
    idt[idx].offset_high = (base >> 16) & 0xFFFF;
}

// C handler called from assembly stub. "regs" points to the area pushed by pusha.
// pushad pushes: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI  (in that order)
void syscall_handler_c(uint32_t *regs){
    uint32_t num = regs[0];
    uint32_t arg_ecx = regs[1]; // ECX
    uint32_t arg_edx = regs[2]; // EDX
    uint32_t arg_ebx = regs[3]; // EBX

    if (num == 1) { // write to screen: (char *buf, int len) -> EBX=buf, ECX=len
        char tmp[512];
        int len = (int)arg_ecx;
        if (len < 0) return;
        if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
        // copy from user memory (shared address space)
        for (int i = 0; i < len; ++i) tmp[i] = ((char*)arg_ebx)[i];
        tmp[len] = '\0';
        console_puts(tmp);
    } else if (num == 2) { // exit: (int code) -> EBX=code
        user_exit_code = (int)arg_ebx;
        user_exited = 1;
        // Optionally print exit msg
        char buf[32];
        int n = 0, v = user_exit_code;
        if (v == 0) { buf[n++] = '0'; }
        else {
            int neg = 0; if (v < 0) { neg = 1; v = -v; }
            char tmp[16]; int ti = 0;
            while (v > 0 && ti < (int)sizeof(tmp)) { tmp[ti++] = '0' + (v % 10); v /= 10; }
            if (neg) tmp[ti++] = '-';
            while (ti--) buf[n++] = tmp[ti];
        }
        buf[n] = '\0';
        console_puts("[user exit]");
        console_puts(buf);

        // Do NOT halt; return to user code so its function can return naturally.
        // exec_elf will check user_exited and return control to kernel.
        return;
    }
}

void init_syscalls(void){
    // Zero IDT
    for (int i = 0; i < 256; ++i){
        set_idt_entry(i, 0, 0, 0);
    }
    // set int 0x80
    set_idt_entry(0x80, (uint32_t)irq80_stub, 0x08, 0x8E);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;
    // load IDT
    asm volatile ("lidt (%0)" :: "r"(&idtp));
}