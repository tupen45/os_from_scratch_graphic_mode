#include "io.h"
#include "graphics.h"
#include "mouse.h"

int mouse_x = 100, mouse_y = 100;

static void mouse_wait_read(){
    while (!(inb(0x64) & 1));
}
static void mouse_wait_write(){
    while  (inb(0x64) & 2);
}

static void mouse_write(unsigned char val){
    mouse_wait_write();
    outb(0x64, 0xD4);
    mouse_wait_write();
    outb(0x60, val);
}
static unsigned char mouse_read(){
    mouse_wait_read();
    return inb(0x60);
}

void init_mouse(){
    unsigned char status;

    // enable aux mouse
    mouse_wait_write(); outb(0x64,0xA8);
    // enable IRQ12
    mouse_wait_write(); outb(0x64,0x20);
    mouse_wait_read();  status = inb(0x60);
    status |= 2;
    mouse_wait_write(); outb(0x64,0x60);
    mouse_wait_write(); outb(0x60,status);

    mouse_write(0xF6); mouse_read(); // default
    mouse_write(0xF4); mouse_read(); // enable

    mouse_x = framebuffer_width/2;
    mouse_y = framebuffer_height/2;
}
