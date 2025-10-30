#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "common.h"
#include "multiboot.h"

extern u32 framebuffer_addr;
extern u32 framebuffer_width;
extern u32 framebuffer_height;
extern u32 framebuffer_bpp;

void init_graphics(multiboot_info_t* mbd);
void put_pixel(int x, int y, u32 color);
void draw_rect(int x, int y, int width, int height, u32 color);
u32  get_pixel(int x,int y); 
void draw_string(int x, int y, const char *s, u32 color);
void draw_button(int x, int y, int width, int height, u32 color, const char *text);
void draw_window(int x, int y, int width, int height, u32 color, const char *title);
void draw_line(int x0, int y0, int x1, int y1, u32 color);

#endif
