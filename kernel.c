// kernel.c — no-anim UI (welcome -> desktop -> apps) with static software cursor
#include "multiboot.h"
#include "graphics.h"
#include "mouse.h"
#include "io.h"
#include "string.h"
#include "http.h"
#include "dns.h"
#include "tls.h"
#include "userprog_hello.h"
#include "exec_elf.h"
#include "syscalls.h"
#include "stdio.h"
#include "json.h"

/* Expose mbedTLS debug buffer accessor implemented in platform_shim.c */
extern const char *mbedtls_get_debug(void);


// Networking
#include "rtl8139.h"
#include "net.h"
#include "endian.h"

typedef unsigned int u32;

static inline int clampi(int v,int lo,int hi){
    if(v<lo) return lo;
    if(v>hi) return hi;
    return v;
}

static void draw_rounded(int x,int y,int w,int h,int r,u32 col){
    // body
    draw_rect(x+r, y,     w-2*r, h,     col);
    draw_rect(x,   y+r,   r,     h-2*r, col);
    draw_rect(x+w-r, y+r, r,     h-2*r, col);
    // corners
    for(int dy=0; dy<r; dy++){
        for(int dx=0; dx<r; dx++){
            if(dx*dx + dy*dy <= r*r){
                put_pixel(x+r-dx,   y+r-dy,   col);
                put_pixel(x+w-r+dx, y+r-dy,   col);
                put_pixel(x+r-dx,   y+h-r+dy, col);
                put_pixel(x+w-r+dx, y+h-r+dy, col);
            }
        }
    }
}

/* ------------------------------------------------------------
- 
Static software cursor (8x8 arrow) with save/restore
----------------------------------------------------------*/
#define CUR_W 8
#define CUR_H 8
static u32 cursor_under[CUR_H][CUR_W];
static int cur_saved = 0;
static int cur_x = 0, cur_y = 0;

/* UDP proxy state for host-side UDP->HTTP helper */
static volatile int udp_proxy_ready = 0;
static int udp_proxy_len = 0;
static char udp_proxy_buf[2048];

static void cursor_save_under(int x, int y){
    for(int j=0;j<CUR_H;j++){
        for(int i=0;i<CUR_W;i++){
            int px = x+i, py = y+j;
            if(px>=0 && py>=0 && px<(int)framebuffer_width && py<(int)framebuffer_height){
                cursor_under[j][i] = get_pixel(px,py);
            }else{
                cursor_under[j][i] = 0; // offscreen safety
            }
        }
    }
    cur_saved = 1;
}

static void cursor_restore_under(void){
    if(!cur_saved) return;
    for(int j=0;j<CUR_H;j++){
        for(int i=0;i<CUR_W;i++){
            int px = cur_x+i, py = cur_y+j;
            if(px>=0 && py>=0 && px<(int)framebuffer_width && py<(int)framebuffer_height){
                put_pixel(px,py,cursor_under[j][i]);
            }
        }
    }
    cur_saved = 0;
}

static void cursor_draw_shape(int x, int y, u32 col){
    // simple 8×8 left-top arrow
    for(int j=0;j<CUR_H;j++){
        for(int i=0;i<=j;i++){
            int px=x+i, py=y+j;
            if(px>=0 && py>=0 && px<(int)framebuffer_width && py<(int)framebuffer_height){
                put_pixel(px,py,col);
            }
        }
    }
}

static void cursor_move_to(int x, int y){
    // restore previous background
    cursor_restore_under();
    // clamp and set new pos
    cur_x = clampi(x,0,(int)framebuffer_width-1);
    cur_y = clampi(y,0,(int)framebuffer_height-1);
    // save new background & draw cursor
    cursor_save_under(cur_x,cur_y);
    cursor_draw_shape(cur_x,cur_y,0x000000);
}

/* ------------------------------------------------------------
- 
Input: read one PS/2 mouse packet (3 bytes) if available
----------------------------------------------------------*/
static int mouse_read_packet(int *dx, int *dy, unsigned char *buttons){
    static unsigned char pkt[3];
    static int idx = 0;

    if(!(inb(0x64)&1)) return 0; // no data
    unsigned char d = inb(0x60);
    pkt[idx++] = d;
    if(idx<3) return 0;

    idx = 0; // complete packet
    signed char sdx = (signed char)pkt[1];
    signed char sdy = (signed char)pkt[2];
    *dx = (int)sdx;
    *dy = -(int)sdy;
    *buttons = pkt[0];
    return 1;
}

/* ===========================================================
Helpers for start menu (file-scope, no nested funcs)
===========================================================*/
static void draw_start_menu(int sm_x,int sm_y,int sm_w,int sm_h){
    draw_rect(sm_x,sm_y,sm_w,sm_h,0xDDDDDD);
    draw_string(sm_x+16, sm_y+20, "Calculator",   0x000000);
    draw_string(sm_x+16, sm_y+50, "Browser",      0x000000);
    draw_string(sm_x+16, sm_y+80, "Notes (dummy)",0x666666);
    draw_string(sm_x+16, sm_y+50, "Browser (Posts)",      0x000000);
    draw_string(sm_x+16, sm_y+80, "JSON Viewer (Users)",0x000000);
    draw_string(sm_x+16, sm_y+110,"Paint (dummy)",0x666666);
    // new menu item for running embedded user program
    draw_string(sm_x+16, sm_y+140, "Run embedded hello", 0x000000);
}

static void clear_start_menu(int sm_x,int sm_y,int sm_w,int sm_h,
                            int bar_h,int sb_x,int sb_y,int sb_w,int sb_h){
    // repaint the area with desktop bg and redraw the taskbar and start button
    draw_rect(sm_x,sm_y,sm_w,sm_h,0x87CEEB);
    draw_rect(0,framebuffer_height-bar_h,framebuffer_width,bar_h,0x333333);
    draw_rect(sb_x,sb_y,sb_w,sb_h,0x8888FF);
    draw_string(sb_x+6,sb_y+8,"S",0xFFFFFF);
}

/* ===========================================================
WELCOME SCREEN
===========================================================*/
static int show_welcome(void){
    draw_rect(0,0,framebuffer_width,framebuffer_height,0xFFFFFF);

    int bw=400,bh=200;
    int bx=(framebuffer_width-bw)/2;
    int by=(framebuffer_height-bh)/2;

    draw_rounded(bx,by,bw,bh,20,0xCCCCCC);
    draw_string(bx+80, by+90, "Welcome To MyOS!", 0xFF0000);

    int nx = bx+bw-120;
    int ny = by+bh-70;
    draw_rounded(nx,ny,100,50,15,0x8888FF);
    draw_string(nx+25,ny+15,"Next",0xFFFFFF);

    // place cursor initially
    cursor_move_to(framebuffer_width/2, framebuffer_height/2);

    while(1){
        if (rtl8139_is_ready()) rtl8139_poll();

        int dx, dy; unsigned char btn;
        if(mouse_read_packet(&dx,&dy,&btn)){
            cursor_move_to(cur_x+dx, cur_y+dy);

            if(btn & 1){ // left button
                if(cur_x>nx && cur_x<nx+100 && cur_y>ny && cur_y<ny+50){
                    cursor_restore_under();
                    return 1;
                }
            }
        }
    }
}

/* ===========================================================
DESKTOP + TASKBAR
Returns: 1 = Calculator, 2 = Browser
===========================================================*/
static int show_desktop(void){
    // desktop background
    draw_rect(0,0,framebuffer_width,framebuffer_height,0x87CEEB);
    if (rtl8139_is_ready()) {
        draw_string(20, 20, "NIC: 10.0.2.15 OK", 0x00AA00);
    } else {
        draw_string(20, 20, "NIC: NOT READY", 0xFF0000);
    }

    // taskbar
    const int bar_h=40;
    draw_rect(0,framebuffer_height-bar_h,framebuffer_width,bar_h,0x333333);

    // start button (bottom-left)
    const int sb_x=8, sb_y=(int)framebuffer_height-bar_h+6, sb_w=28, sb_h=28;
    draw_rect(sb_x,sb_y,sb_w,sb_h,0x8888FF);
    draw_string(sb_x+6,sb_y+8,"S",0xFFFFFF);

    // start menu rect (bottom-left popup)
    const int sm_x=0, sm_w=220, sm_h=180; // increased height to fit new item
    const int sm_y=(int)framebuffer_height-bar_h-sm_h;

    int start_open=0;

    // draw cursor (restore if any old)
    cursor_move_to(cur_x, cur_y);

    while(1){
        if (rtl8139_is_ready()) rtl8139_poll();

        int dx, dy; unsigned char btn;
        if(mouse_read_packet(&dx,&dy,&btn)){
            cursor_move_to(cur_x+dx, cur_y+dy);

            if(btn & 1){ // left click
                // start button toggle
                if(cur_x>sb_x && cur_x<sb_x+sb_w && cur_y>sb_y && cur_y<sb_y+sb_h){
                    start_open = !start_open;
                    if(start_open) draw_start_menu(sm_x,sm_y,sm_w,sm_h);
                    else           clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                    continue;
                }

                if(start_open){
                    // Calculator
                    if(cur_x>sm_x+16 && cur_x<sm_x+180 &&
                    cur_y>sm_y+16 && cur_y<sm_y+36){
                        clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                        return 1;
                    }
                    // Browser
                    if(cur_x>sm_x+16 && cur_x<sm_x+180 &&
                    cur_y>sm_y+46 && cur_y<sm_y+66){
                        clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                        return 2;
                    }
                    // JSON Viewer
                    if(cur_x>sm_x+16 && cur_x<sm_x+180 &&
                    cur_y>sm_y+76 && cur_y<sm_y+96){
                        clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                        return 3;
                    }
                    // Run embedded hello (new)
                    if(cur_x>sm_x+16 && cur_x<sm_x+180 &&
                    cur_y>sm_y+136 && cur_y<sm_y+156){
                        clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                        // attempt to run embedded ELF if present
                        if (get_hello_ptr && get_hello_len && get_hello_len() > 0) {
                            console_puts("Running embedded program...\n");
                            int rc = elf32_load_and_run((const void*)get_hello_ptr(), (size_t)get_hello_len());
                            // convert rc to string
                            char numbuf[16]; int n=0; int t = rc; if(t==0) numbuf[n++]='0'; else { if(t<0){ numbuf[n++]='-'; t=-t; } int st=0; int tmp=t; while(tmp>0){ numbuf[n+st++] = '0' + (tmp%10); tmp/=10; } for(int i=0;i<st/2;i++){
                                char c=n+i; char d=n+st-1-i; char tmpc = numbuf[c]; numbuf[c]=numbuf[d]; numbuf[d]=tmpc; }
                                n += st; }
                            numbuf[n]=0;
                            console_puts("User program exited with code: ");
                            console_puts(numbuf);
                            console_puts("\n");
                        } else {
                            console_puts("No embedded program present. Run make userprog and embed-userprog first.\n");
                        }
                    }
                    // click outside menu closes it
                    if(!(cur_x>sm_x && cur_x<sm_x+sm_w && cur_y>sm_y && cur_y<sm_y+sm_h)){
                        start_open=0;
                        clear_start_menu(sm_x,sm_y,sm_w,sm_h,bar_h,sb_x,sb_y,sb_w,sb_h);
                    }
                }
            }
        }
    }
}

/* ===========================================================
CALCULATOR
===========================================================*/
#define BW 60
#define BH 40
static char expr[64]={0}; static int expr_len=0;

static void calc_add(char c){ if(expr_len<63){expr[expr_len++]=c; expr[expr_len]=0;}}
static void calc_clear(void){ expr_len=0; expr[0]=0; }
static void calc_eval(void){
    int res=0; char op='+';
    for(int i=0;i<expr_len;i++){
        int v=0;
        while(i<expr_len && expr[i]>='0' && expr[i]<='9'){ v=v*10+(expr[i]-'0'); i++; }
        if(op=='+')res+=v;
        else if(op=='-')res-=v;
        else if(op=='*')res*=v;
        else if(op=='/')res/= (v? v:1);
        if(i<expr_len){ op=expr[i]; i++; }
    }
    calc_clear();
    // write result back
    char buf[20]; int n=0, t=res;
    if(t==0) buf[n++]='0';
    int neg=0; if(t<0){ neg=1; t=-t; }
    while(t>0){ buf[n++]=(t%10)+'0'; t/=10; }
    if(neg) buf[n++]='-';
    while(n--) calc_add(buf[n]);
}

static void draw_key(int x,int y,char c){
    draw_rect(x,y,BW,BH,0xAAAAAA);
    char s[2]={c,0};
    draw_string(x+25,y+13,s,0x000000);
}

static void calculator_ui(void){
    // redraw desktop bg behind window (no animation)
    draw_rect(0,0,framebuffer_width,framebuffer_height,0x87CEEB);

    int winw=300, winh=350;
    int wx=(framebuffer_width-winw)/2;
    int wy=(framebuffer_height-winh)/2;

    draw_rounded(wx,wy,winw,winh,15,0xBBBBBB);
    draw_rect(wx+10,wy+10,winw-20,60,0xFFFFFF);

    // close button
    int cx=wx+winw-30, cy=wy+10, cw=20, ch=20;
    draw_rect(cx,cy,cw,ch,0xFF0000);
    draw_string(cx+5,cy+2,"X",0xFFFFFF);

    // keys
    const char *keys="789/456*123-0.=+C";
    int idx=0;
    for(int r=0;r<4;r++){
        for(int c=0;c<4;c++){
            char k=keys[idx++];
            int bx=wx+10+c*(BW+5);
            int by=wy+80+r*(BH+5);
            draw_key(bx,by,k);
        }
    }

    // show cursor at current pos
    cursor_move_to(cur_x,cur_y);

    while(1){
        if (rtl8139_is_ready()) rtl8139_poll();

        // refresh display area (expression)
        draw_rect(wx+12,wy+12,winw-24,56,0xFFFFFF);
        draw_string(wx+20,wy+32,expr,0x000000);

        int dx, dy; unsigned char btn;
        if(mouse_read_packet(&dx,&dy,&btn)){
            cursor_move_to(cur_x+dx, cur_y+dy);

            if(btn & 1){
                // close
                if(cur_x>cx && cur_x<cx+cw && cur_y>cy && cur_y<cy+ch) {
                    cursor_restore_under();
                    return;
                }
                // keys
                idx=0;
                for(int rr=0;rr<4;rr++){
                    for(int cc=0;cc<4;cc++){
                        char k=keys[idx++];
                        int bx=wx+10+cc*(BW+5);
                        int by=wy+80+rr*(BH+5);
                        if(cur_x>bx && cur_x<bx+BW && cur_y>by && cur_y<by+BH){
                            if(k=='C') calc_clear();
                            else if(k=='=') calc_eval();
                            else            calc_add(k);
                        }
                    }
                }
            }
        }
    }
}

/* ===========================================================
BROWSER (layout window) with close - updated to fetch HTTP body
===========================================================*/

// very basic parser for http://host:port/path
static int parse_url(const char *url, char *host_buf, int host_buf_size, const char **path, uint16_t *port) {
    if (strncmp(url, "http://", 7) != 0) return 0;
    const char *host_start = url + 7;
    const char *p = host_start;
    while (*p && *p != ':' && *p != '/') p++;

    int host_len = p - host_start;
    if (host_len >= host_buf_size) return 0; // host too long
    memcpy(host_buf, host_start, host_len);
    host_buf[host_len] = 0;

    *port = 80;
    if (*p == ':') {
        *port = 0;
        p++;
        while (*p && *p >= '0' && *p <= '9') {
            *port = *port * 10 + (*p - '0');
            p++;
        }
    }

    if (*p == '/') {
        *path = p;
    } else {
        *path = "/";
    }
    return 1;
}


static void browser_ui(void){
    // redraw desktop bg
    draw_rect(0,0,framebuffer_width,framebuffer_height,0x87CEEB);

    const int ww=600, wh=400;
    const int wx=(framebuffer_width - ww)/2;
    const int wy=(framebuffer_height - wh)/2;

    // window frame
    draw_rounded(wx,wy,ww,wh,12,0xCCCCCC);

    // title bar
    const int tb_h=30;
    draw_rect(wx,wy,ww,tb_h,0x1E90FF);
    draw_string(wx+10, wy+8, "TBHCR Browser", 0xFFFFFF);

    // close button
    const int cx=wx+ww-26, cy=wy+5, cw=20, ch=20;
    draw_rect(cx,cy,cw,ch,0xFF0000);
    draw_string(cx+5,cy+2,"X",0xFFFFFF);

    // address bar
    const int ab_x=wx+10, ab_y=wy+tb_h+8, ab_w=ww-20, ab_h=24;
    draw_rect(ab_x,ab_y,ab_w,ab_h,0xDDDDDD);
    const char *url = "http://jsonplaceholder.typicode.com/posts/2";
    draw_string(ab_x+6,ab_y+5,url,0x000000);

    // content area
    const int ct_x=wx+10, ct_y=ab_y+ab_h+8, ct_w=ww-20, ct_h=wh - (tb_h+8+ab_h+8+12);
    draw_rect(ct_x,ct_y,ct_w,ct_h,0xFFFFFF);

    // initially show placeholder
    draw_string(ct_x+6,ct_y+8,"Fetching...",0x000000);

    // Cursor
    cursor_move_to(cur_x,cur_y);

    // --- perform HTTP fetch once when browser opens ---
    char host[128];
    const char *path;
    uint16_t port;
    int parsed_ok = parse_url(url, host, sizeof(host), &path, &port);

    uint32_t resolved_ip = 0;
    int dns_ok = 0;
    if (parsed_ok) {
        dns_ok = dns_resolve(host, &resolved_ip);
    }

    static char bodybuf[8192];
    int got = 0;
    if (dns_ok) {
        char host_port[256];
        // custom itoa
        char port_s[6];
        char *p = port_s + 5;
        *p-- = 0;
        int n = port;
        if (n==0) *p-- = '0';
        while(n>0) { *p-- = (n%10)+'0'; n/=10; }
        p++;

        int host_len = strlen(host);
        memcpy(host_port, host, host_len);
        host_port[host_len] = ':';
        int port_len = strlen(p);
        memcpy(host_port + host_len + 1, p, port_len);
        host_port[host_len + 1 + port_len] = 0;

        got = http_get_by_ip_port(resolved_ip, port, host_port, path, bodybuf, sizeof(bodybuf));
    }


    // If plain HTTP failed but IP resolved, try HTTPS (if your server supports it)
    if (got <= 0 && resolved_ip != 0) {
        int tgot = tls_http_get_by_ip(resolved_ip, host, path, bodybuf, sizeof(bodybuf));
        if (tgot > 0) {
            got = tgot;
        }
    }

    // redraw content area with result (truncate sensibly)
    draw_rect(ct_x,ct_y,ct_w,ct_h,0xFFFFFF);

    if (got > 0) {
        int max_lines = (ct_h - 16) / 10;
        int line = 0;
        char* p = bodybuf;
        while (line < max_lines && *p) {
            char tmp[128]; int ti = 0;
            // copy up to line width or newline
            while (*p && *p != '\n' && ti < (int)sizeof(tmp)-1) tmp[ti++] = *p++;
            tmp[ti] = '\0';
            if (*p == '\n') p++;
            // trim leading spaces if too long
            int start = 0; while (tmp[start] == ' ' && start < ti) start++;
            draw_string(ct_x+6, ct_y+8 + line*10, tmp + start, 0x000000);
            line++;
            if (line >= max_lines) break;
        }
        if (*p) draw_string(ct_x+6, ct_y + ct_h - 18, "...(truncated)", 0x555555);
    } else {
        char dbg_buf[128];
        snprintf(dbg_buf, sizeof(dbg_buf), "Fetch failed! dns_ok=%d, resolved_ip=%u, got=%d", dns_ok, resolved_ip, got);
        draw_string(ct_x+6, ct_y+8, dbg_buf, 0xFF0000);
    }

    // Cursor placed after drawing
    cursor_move_to(cur_x,cur_y);

    while(1){
        if (rtl8139_is_ready()) rtl8139_poll();

        int dx, dy; unsigned char btn;
        if(mouse_read_packet(&dx,&dy,&btn)){
            cursor_move_to(cur_x+dx, cur_y+dy);

            if(btn & 1){ // close
                if(cur_x>cx && cur_x<cx+cw && cur_y>cy && cur_y<cy+ch){
                    cursor_restore_under();
                    return;
                }
            }
        }
    }
}

/* ===========================================================
JSON VIEWER
===========================================================*/
static void json_viewer_ui(void){
    // redraw desktop bg
    draw_rect(0,0,framebuffer_width,framebuffer_height,0x87CEEB);

    const int ww=600, wh=400;
    const int wx=(framebuffer_width - ww)/2;
    const int wy=(framebuffer_height - wh)/2;

    // window frame
    draw_rounded(wx,wy,ww,wh,12,0xCCCCCC);

    // title bar
    const int tb_h=30;
    draw_rect(wx,wy,ww,tb_h,0x1E90FF);
    draw_string(wx+10, wy+8, "JSON Viewer", 0xFFFFFF);

    // close button
    const int cx=wx+ww-26, cy=wy+5, cw=20, ch=20;
    draw_rect(cx,cy,cw,ch,0xFF0000);
    draw_string(cx+5,cy+2,"X",0xFFFFFF);

    // content area
    const int ct_x=wx+10, ct_y=wy+tb_h+8, ct_w=ww-20, ct_h=wh - (tb_h+8+12);
    draw_rect(ct_x,ct_y,ct_w,ct_h,0xFFFFFF);

    // initially show placeholder
    draw_string(ct_x+6,ct_y+8,"Fetching...",0x000000);

    // Cursor
    cursor_move_to(cur_x,cur_y);

    // --- perform HTTP fetch once when browser opens ---
    const char *url = "http://jsonplaceholder.typicode.com/users/1";
    char host[128];
    const char *path;
    uint16_t port;
    int parsed_ok = parse_url(url, host, sizeof(host), &path, &port);

    uint32_t resolved_ip = 0;
    int dns_ok = 0;
    if (parsed_ok) {
        dns_ok = dns_resolve(host, &resolved_ip);
    }

    static char bodybuf[8192];
    int got = 0;
    if (dns_ok) {
        char host_port[256];
        // custom itoa
        char port_s[6];
        char *p = port_s + 5;
        *p-- = 0;
        int n = port;
        if (n==0) *p-- = '0';
        while(n>0) { *p-- = (n%10)+'0'; n/=10; }
        p++;

        int host_len = strlen(host);
        memcpy(host_port, host, host_len);
        host_port[host_len] = ':';
        int port_len = strlen(p);
        memcpy(host_port + host_len + 1, p, port_len);
        host_port[host_len + 1 + port_len] = 0;

        got = http_get_by_ip_port(resolved_ip, port, host_port, path, bodybuf, sizeof(bodybuf));
    }

    // redraw content area with result (truncate sensibly)
    draw_rect(ct_x,ct_y,ct_w,ct_h,0xFFFFFF);

    if (got > 0) {
        jsmn_parser p;
        jsmntok_t t[128];
        jsmn_init(&p);
        int r = jsmn_parse(&p, bodybuf, strlen(bodybuf), t, 128);
        if (r < 0) {
            draw_string(ct_x+6, ct_y+8, "Failed to parse JSON", 0xFF0000);
        } else {
            int line = 0;
            int max_lines = (ct_h - 16) / 10;
            for (int i = 1; i < r; i++) {
                if (t[i].type == JSMN_STRING && t[i-1].type == JSMN_STRING) {
                    char key[64];
                    char val[64];
                    char line_buf[128];
                    int key_len = t[i-1].end - t[i-1].start;
                    int val_len = t[i].end - t[i].start;
                    if (key_len > 63) key_len = 63;
                    if (val_len > 63) val_len = 63;
                    memcpy(key, bodybuf + t[i-1].start, key_len);
                    key[key_len] = 0;
                    memcpy(val, bodybuf + t[i].start, val_len);
                    val[val_len] = 0;
                    snprintf(line_buf, sizeof(line_buf), "%s: %s", key, val);
                    draw_string(ct_x+6, ct_y+8 + line*10, line_buf, 0x000000);
                    line++;
                    if (line >= max_lines) break;
                }
            }
        }
    } else {
        char dbg_buf[128];
        snprintf(dbg_buf, sizeof(dbg_buf), "Fetch failed! dns_ok=%d, resolved_ip=%u, got=%d", dns_ok, resolved_ip, got);
        draw_string(ct_x+6, ct_y+8, dbg_buf, 0xFF0000);
    }

    // Cursor placed after drawing
    cursor_move_to(cur_x,cur_y);

    while(1){
        if (rtl8139_is_ready()) rtl8139_poll();

        int dx, dy; unsigned char btn;
        if(mouse_read_packet(&dx,&dy,&btn)){
            cursor_move_to(cur_x+dx, cur_y+dy);

            if(btn & 1){ // close
                if(cur_x>cx && cur_x<cx+cw && cur_y>cy && cur_y<cy+ch){
                    cursor_restore_under();
                    return;
                }
            }
        }
    }
}

/* Tiny early UART init so COM1 is usable very early in boot.
 * Kept minimal and safe for both QEMU and real hardware. */
static void uart_init_early(void){
    unsigned short port = 0x3F8; // COM1
    outb(port + 1, 0x00);    // Disable all interrupts
    outb(port + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x01);    // Divisor low byte (115200)
    outb(port + 1, 0x00);    // Divisor high byte
    outb(port + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    (void)inb(port);         // dummy read to settle
}
/* Very small early serial write helper (polls LSR then writes). Used to
 * emit a guaranteed boot message visible on QEMU -serial stdio. Kept
 * independent of other serial helpers to ensure it's available at the
 * very start of kernel execution. */
static void serial_early_puts(const char *s){
    unsigned short port = 0x3F8;
    while(s && *s){
        /* wait for THR empty */
        for(int i=0;i<100000;i++){
            uint8_t lsr = inb(port + 5);
            if(lsr & 0x20) break;
        }
        outb(port + 0, (uint8_t)*s);
        s++;
    }
}

/* ===========================================================
kmain
===========================================================*/
void kmain(unsigned magic,unsigned addr){
    (void)magic;
    uart_init_early();
    /* Emit a short serial boot banner to help diagnose -serial stdio visibility */
    serial_early_puts("serial: kernel start\n");
    /* Probe xHCI early and always so we get controller/port logs on serial even
     * when a PCI NIC is present. These extern declarations reference the
     * implementations in usb/xhci.c. */
    extern int xhci_probe(void);
    extern void xhci_dump_ports(void);
    extern int xhci_enumerate_once(void);
    xhci_probe();
    xhci_dump_ports();
    /* Attempt a simple enumerate (may be dry-run depending on xhci_hw_enable) */
    xhci_enumerate_once();
    init_graphics((void*)addr);
    init_mouse();

    // Bring up NIC + set IP (QEMU slirp defaults)
    // Try PCI NIC first
    if (rtl8139_init() == 0){
        // guest 10.0.2.15/24, gateway 10.0.2.2
        uint32_t ip      = (10<<24)|(0<<16)|(2<<8)|15;
        uint32_t netmask = (255<<24)|(255<<16)|(255<<8)|0;
        uint32_t gw      = (10<<24)|(0<<16)|(2<<8)|2;
        net_set_ipv4(ip, netmask, gw);

        // ✅ Debug: show that NIC is working
        draw_string(20, 20, "NIC init OK, IP=10.0.2.15", 0x00AA00);
    } else {
    // Try USB NIC stub for testing (no real USB host implemented yet)
        extern int usb_nic_init(void);
        extern void usb_nic_enable(void);
        extern void usb_nic_inject_test_frame(const uint8_t *frame,int len);
        usb_nic_init();
        usb_nic_enable();

    /* Probe xHCI controller and dump ports for debugging. This is a safe
     * read-only probe that will log PCI/BAR/op_base info even with
     * xhci_hw_enable == 0. Useful to verify the controller and port state. */
    extern int xhci_probe(void);
    extern void xhci_dump_ports(void);
    xhci_probe();
    xhci_dump_ports();

    /* Attempt a simple xHCI enumeration test in dry-run mode. This will
     * not touch hardware unless `xhci_hw_enable` is set to 1. It prints
     * any descriptor bytes it can read via the control-transfer helpers.
     */
    extern int xhci_enumerate_once(void);
    xhci_enumerate_once();

        // Build a minimal Ethernet frame (ARP reply or a small IPv4 UDP payload)
        // We'll inject an IPv4 UDP packet with small payload that the net layer can parse.
        uint8_t test_frame[64] = {0};
        // dst MAC = our netif mac (unknown), but net_rx doesn't check dst; set some bytes
        test_frame[0]=0x02;test_frame[1]=0x00;test_frame[2]=0x00;test_frame[3]=0x00;test_frame[4]=0x00;test_frame[5]=0x01;
        // src MAC
        test_frame[6]=0x02;test_frame[7]=0x00;test_frame[8]=0x00;test_frame[9]=0x00;test_frame[10]=0x00;test_frame[11]=0x02;
        // Ethertype = IPv4
        test_frame[12]=0x08; test_frame[13]=0x00;
        // Minimal IP header (20 bytes)
        test_frame[14] = 0x45; // v4, ihl=5
        test_frame[15] = 0; // tos
        uint16_t totlen = htons(20 + 8 + 12); // IP hdr + UDP hdr + payload
        memcpy(&test_frame[16], &totlen, 2);
        test_frame[18]=0; test_frame[19]=0; // id
        test_frame[20]=0; test_frame[21]=0; // frag
        test_frame[22]=64; // ttl
        test_frame[23]=17; // protocol UDP
        test_frame[24]=0; test_frame[25]=0; // checksum (ignored)
        // src IP 192.168.31.235
        test_frame[26]=192; test_frame[27]=168; test_frame[28]=31; test_frame[29]=235;
        // dst IP 10.0.2.15 (guest)
        test_frame[30]=10; test_frame[31]=0; test_frame[32]=2; test_frame[33]=15;
        // UDP header at offset 34
        uint16_t srcp = htons(6000);
        uint16_t dstp = htons(6001);
        memcpy(&test_frame[34], &srcp, 2);
        memcpy(&test_frame[36], &dstp, 2);
        uint16_t udplen = htons(8 + 12);
        memcpy(&test_frame[38], &udplen, 2);
        test_frame[40]=0; test_frame[41]=0; // checksum
        // UDP payload (12 bytes) at 42
        const char *msg = "{\"title\":\"USB\"}";
        memcpy(&test_frame[42], msg, 12);

        usb_nic_inject_test_frame(test_frame, 42 + 12);

        draw_string(20, 20, "NIC: usb_stub enabled (test frame injected)", 0xFFD700);
    }

    // Start with cursor at center (save & draw once)
    cursor_move_to((int)framebuffer_width/2,(int)framebuffer_height/2);

    if(show_welcome()){
        while(1){
            if (rtl8139_is_ready()) {
                rtl8139_poll();
            }

            int app = show_desktop();
            if(app==1){
                calculator_ui();
            }else if(app==2){
                browser_ui();
            }else if(app==3){
                json_viewer_ui();
            }
        }
    }
}
