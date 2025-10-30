#include "graphics.h"
#include "multiboot.h"

u32 framebuffer_addr=0, framebuffer_width=0, framebuffer_height=0;
u32 framebuffer_bpp=0, framebuffer_pitch=0;

void init_graphics(multiboot_info_t *m)
{
    multiboot_tag_t *tag;
    for(tag=(multiboot_tag_t*)(m+1);tag->type!=0;tag=(multiboot_tag_t*)
         ((u8*)tag+((tag->size+7)&~7)))
    {
        if(tag->type == 8) {
            multiboot_tag_framebuffer_t *fb=(void*)tag;
            framebuffer_addr   = fb->framebuffer_addr;
            framebuffer_width  = fb->framebuffer_width;
            framebuffer_height = fb->framebuffer_height;
            framebuffer_bpp    = fb->framebuffer_bpp;
            framebuffer_pitch  = fb->framebuffer_pitch;
            return;
        }
    }
}

void put_pixel(int x,int y,u32 c){
    if(!framebuffer_addr)return;
    if(x<0 || y<0 || x >= (int)framebuffer_width || y >= (int)framebuffer_height) return;
    ((u32*)framebuffer_addr)[ y*(framebuffer_pitch/4) + x ]=c;
}

u32 get_pixel(int x,int y){
    if(!framebuffer_addr)return 0;
    if(x<0 || y<0 || x >= (int)framebuffer_width || y >= (int)framebuffer_height) return 0;
    return ((u32*)framebuffer_addr)[ y*(framebuffer_pitch/4) + x ];
}

void draw_rect(int x,int y,int w,int h,u32 c){
    for(int i=y;i<y+h;i++)
        for(int j=x;j<x+w;j++)
            put_pixel(j,i,c);
}

void draw_char(int x,int y,char c,u32 color)
{
    extern const u8 font[256][8];
    const u8 *glyph = font[(int)c];

    for(int i=0;i<8;i++){
        for(int j=0;j<8;j++){
            if((glyph[i] >> (7-j)) & 1){
                put_pixel(x+j, y+i, color);
            }
        }
    }
}

void draw_string(int x,int y,const char*s,u32 color)
{
    for(int i=0;s[i];i++){
        draw_char(x + i*8, y, s[i], color);
    }
}

void xor_pixel(int x,int y,u32 color){
    if(!framebuffer_addr)return;
    if(x<0 || y<0 || x >= (int)framebuffer_width || y >= (int)framebuffer_height) return;
    u32 *fb=(u32*)framebuffer_addr;
    fb[y*(framebuffer_pitch/4) + x] ^= color;
}

void xor_cursor(int x,int y){
    for(int dy=0;dy<6;dy++){
        for(int dx=0;dx<6;dx++){
            xor_pixel(x+dx,y+dy,0x00FFFFFF); // any mask color
        }
    }
}
