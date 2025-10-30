#include "usb.h"
#include "pci.h"
#include "stdio.h"
#include <stdint.h>
/* allocator provided by kernel */
void *kmalloc(size_t sz);

/* runtime gate: set to 1 to allow actual hardware writes (DANGEROUS). Default 0. */
int xhci_hw_enable = 1; /* ENABLED: set to 1 for VM passthrough testing; be careful on bare-metal */

/* Small serial debug helper: mirror driver printf to COM1 (0x3f8) so
 * kernel debug messages are visible with QEMU -serial stdio. We override
 * printf locally in this file to ensure xhci-specific prints appear on the
 * serial console used for debugging. This keeps the on-screen framebuffer
 * behavior unchanged when not using serial.
 */
#include <stdarg.h>
#include "io.h"
extern int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
static void uart_init_once(void){
    static int inited = 0;
    if(inited) return;
    /* Configure 0x3f8 COM1: 115200, 8N1, enable FIFO */
    outb(0x3f8 + 1, 0x00); // Disable all interrupts
    outb(0x3f8 + 3, 0x80); // Enable DLAB
    outb(0x3f8 + 0, 0x01); // Divisor low byte (115200)
    outb(0x3f8 + 1, 0x00); // Divisor high byte
    outb(0x3f8 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(0x3f8 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(0x3f8 + 4, 0x0B); // IRQs enabled, RTS/DSR set
    inited = 1;
}

static void serial_putc(char c){
    uart_init_once();
    /* Wait for Transmit Holding Register empty (LSR bit 5) */
    for(int i=0;i<10000;i++){
        uint8_t lsr = inb(0x3f8 + 5);
        if(lsr & 0x20) break;
    }
    outb(0x3f8 + 0, (uint8_t)c);
}
static void serial_puts(const char *s){
    while(s && *s) serial_putc(*s++);
}
static int serial_printf(const char *fmt, ...){
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n>0) serial_puts(buf);
    return n;
}
/* Redirect printf in this file to serial_printf so xhci: logs show on -serial stdio */
#undef printf
#define printf(...) serial_printf(__VA_ARGS__)

/* Simple no-dep hex printers used during very early boot diagnostics */
static void serial_puthex8(uint8_t v){
    const char *hex = "0123456789ABCDEF";
    char b[3];
    b[0] = hex[(v>>4)&0xF]; b[1]=hex[v&0xF]; b[2]=0;
    serial_puts(b);
}
static void serial_puthex32(uint32_t v){
    const char *hex = "0123456789ABCDEF";
    char b[9];
    for(int i=0;i<8;i++){
        b[7-i] = hex[(v >> (i*4)) & 0xF];
    }
    b[8]=0;
    serial_puts(b);
}
/* small decimal printer (positive numbers only) */
static void serial_putdec(uint32_t v){
    char buf[12];
    int pos = 0;
    if(v==0){ serial_putc('0'); return; }
    while(v>0 && pos < (int)sizeof(buf)-1){ buf[pos++] = '0' + (v % 10); v /= 10; }
    for(int i=pos-1;i>=0;--i) serial_putc(buf[i]);
}

/* TRB and command ring minimal scaffolding */
#define XHCI_TRB_SIZE 16
#define XHCI_CMD_RING_TRBS 256

struct xhci_trb {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
};

static struct xhci_trb *cmd_ring = NULL;
static uintptr_t cmd_ring_phys = 0;
/* Simple pending bitmap for command TRBs so we can correlate completions */
static uint8_t cmd_ring_pending[XHCI_CMD_RING_TRBS];
/* Simple head pointer for a circular command ring and a cycle bit tracker */
static int cmd_ring_head = 0; /* next free slot index */
static int cmd_ring_cycle = 1; /* current cycle bit used for new TRBs */
/* per-slot token for correlating commands to completions */
static uint32_t cmd_ring_token[XHCI_CMD_RING_TRBS];
static uint32_t cmd_token_counter = 0;

/* last prepared command parameters (for submit) */
static int last_cmd_start = -1;
static int last_cmd_trb_count = 0;
static uint32_t last_cmd_token = 0;

static void xhci_cmd_mark_pending(int idx){
    if(idx<0 || idx>=XHCI_CMD_RING_TRBS) return;
    cmd_ring_pending[idx]=1;
}

static void xhci_cmd_print_pending(void){
    int count=0;
    for(int i=0;i<XHCI_CMD_RING_TRBS;i++) if(cmd_ring_pending[i]) count++;
    serial_puts("usb/xhci: cmd_ring pending count="); serial_putdec((uint32_t)count);
    serial_puts("/"); serial_putdec((uint32_t)XHCI_CMD_RING_TRBS); serial_puts("\n");
}

/* DCBAA and device context bookkeeping allocated once (forward-declare so
 * functions earlier in this file can reference them). Real allocation occurs
 * lazily in the submission path. */
static void *g_dcbaa = NULL;
static uintptr_t g_dcbaa_phys = 0;
static void *g_dev_ctx = NULL;
static uintptr_t g_dev_ctx_phys = 0;
/* slot bookkeeping */
static int g_slot_enabled = 0; /* 0 = no slot, 1 = slot enabled (soft) */
/* endpoint-0 ring bookkeeping (one simple segment) */
static void *ep0_ring = NULL;
static uintptr_t ep0_ring_phys = 0;
/* endpoint-0 Ring Segment Table (RST) bookkeeping */
static void *ep0_rst = NULL;
static uintptr_t ep0_rst_phys = 0;

/* helpers to perform slot enable / device context initialization
 * These are minimal, conservative implementations that prepare local state
 * and populate the allocated device context page with safe defaults.
 * A full implementation must construct proper command TRBs and parse
 * hardware command-completion events; here we allocate and initialize
 * memory so the controller has plausible context pointers to read.
 */
int xhci_send_enable_slot(void);
int xhci_init_device_context(void);

/* MMIO helper prototypes (defined later) */
static inline uint32_t xhci_cap_read32(uint32_t off);
static inline uint32_t xhci_op_read32(uint32_t off);
static inline void xhci_op_write32(uint32_t off, uint32_t val);
/* Event ring helper prototypes */
int xhci_init_event_ring(void);
void xhci_dump_event_ring(void);
/* New helpers */
static inline void xhci_op_write64(uint32_t off_lo, uint64_t val);
int xhci_program_event_ring(void);
void xhci_ring_doorbell(uint32_t db_index, uint32_t value);
/* Poll and parse event TRBs; if an IN data buffer is provided and a completion
 * is observed, copy data from the DMA buffer (data_v) into the user buffer
 * (user_buf). Returns 0 on observed completion, -1 on timeout or error.
 */
int xhci_poll_event_ring(int timeout_ms, void *data_v, void *user_buf, int data_len, int direction_in);

int xhci_init_command_ring(void){
    if(cmd_ring) return 0;
    /* allocate a contiguous buffer for TRBs (not high-performance DMA mapping) */
    size_t sz = XHCI_CMD_RING_TRBS * sizeof(struct xhci_trb);
    void *p = kmalloc(sz);
    if(!p) return -1;
    cmd_ring = (struct xhci_trb*)p;
    /* In this simple kernel environment VIRT_TO_PHYS may be a macro; fallback to cast */
#ifdef VIRT_TO_PHYS
    cmd_ring_phys = (uintptr_t)VIRT_TO_PHYS(cmd_ring);
#else
    cmd_ring_phys = (uintptr_t)(uintptr_t)cmd_ring;
#endif
    /* zero ring */
    for(size_t i=0;i< (size_t)XHCI_CMD_RING_TRBS;i++){
        cmd_ring[i].a = cmd_ring[i].b = cmd_ring[i].c = cmd_ring[i].d = 0;
        cmd_ring_pending[i] = 0;
        cmd_ring_token[i] = 0;
    }
    cmd_ring_head = 0;
    cmd_ring_cycle = 1;
    return 0;
}
void *xhci_command_ring_virt(void){ return cmd_ring; }
uintptr_t xhci_command_ring_phys(void){ return cmd_ring_phys; }

/* Simple TRB helpers to build Setup/Data/Status TRBs in the command ring for
 * a control transfer. These helpers only prepare TRBs in memory and log them.
 * They do NOT activate rings or touch controller registers. That will be
 * implemented in the next step (event ring, CRCR, doorbells, DCBAA, etc.).
 */
#define XHCI_TRB_TYPE_SETUP_STAGE 2
#define XHCI_TRB_TYPE_DATA_STAGE 1
#define XHCI_TRB_TYPE_STATUS_STAGE 3
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD 9
#define XHCI_TRB_CYCLE_BIT 0x1

/* Build a Setup TRB that points to a 8-byte setup packet in memory. This just
 * stores the 64-bit pointer into TRB.a/b and sets type fields in d for debug.
 */
void xhci_build_setup_trb(int idx, uintptr_t buf_phys, uint32_t length){
    if(!cmd_ring) return;
    struct xhci_trb *t = &cmd_ring[idx];
    t->a = (uint32_t)((uint64_t)buf_phys & 0xFFFFFFFFu);
    t->b = (uint32_t)(((uint64_t)buf_phys >> 32) & 0xFFFFFFFFu);
    t->c = length; /* length in bytes */
    /* d: bits [10:8] TRB type in xHCI, use simplified placement here */
    t->d = (XHCI_TRB_TYPE_SETUP_STAGE << 10);
}

void xhci_build_data_trb(int idx, uintptr_t buf_phys, uint32_t length, int direction_in){
    if(!cmd_ring) return;
    struct xhci_trb *t = &cmd_ring[idx];
    t->a = (uint32_t)((uint64_t)buf_phys & 0xFFFFFFFFu);
    t->b = (uint32_t)(((uint64_t)buf_phys >> 32) & 0xFFFFFFFFu);
    t->c = length;
    uint32_t type = XHCI_TRB_TYPE_DATA_STAGE;
    if(direction_in) t->d = (type << 10) | (1<<1); else t->d = (type << 10);
}

void xhci_build_status_trb(int idx){
    if(!cmd_ring) return;
    struct xhci_trb *t = &cmd_ring[idx];
    t->a = t->b = t->c = 0;
    t->d = (XHCI_TRB_TYPE_STATUS_STAGE << 10);
}

/* Generic command TRB builder: place opcode/type into d and optional param
 * values into a/b/c as convenience. Caller must manage cycle bit as needed.
 */
void xhci_build_command_trb(int idx, uint32_t type, uint32_t param_lo, uint32_t param_hi){
    if(!cmd_ring) return;
    struct xhci_trb *t = &cmd_ring[idx];
    t->a = param_lo;
    t->b = param_hi;
    t->c = 0;
    t->d = (type << 10);
}

/* Dump first n TRBs from the command ring for debugging */
void xhci_dump_cmd_ring(int n){
    if(!cmd_ring){ printf("usb/xhci: cmd_ring not allocated\n"); return; }
    if(n> XHCI_CMD_RING_TRBS) n = XHCI_CMD_RING_TRBS;
    for(int i=0;i<n;i++){
        struct xhci_trb *t = &cmd_ring[i];
        serial_puts("usb/xhci: TRB["); serial_putdec((uint32_t)i); serial_puts("]= a=0x"); serial_puthex32(t->a);
        serial_puts(" b=0x"); serial_puthex32(t->b); serial_puts(" c=0x"); serial_puthex32(t->c);
        serial_puts(" d=0x"); serial_puthex32(t->d); serial_puts("\n");
    }
}

/* Prepare a minimal set of TRBs for a standard control transfer (Setup ->
 * optional Data -> Status). This only writes TRBs into the command ring and
 * returns the number of TRBs prepared, or -1 on error.
 */
int xhci_prepare_control_transfer(const void *setup8, uintptr_t setup_phys,
                                  void *data, uintptr_t data_phys, int data_len, int direction_in){
    if(!cmd_ring){
        if(xhci_init_command_ring()<0) return -1;
    }
    /* We'll write TRBs directly into the command ring at cmd_ring_head as staging,
     * record the start index and TRB count in last_cmd_* so submit can finalize.
     */
    int start = cmd_ring_head;
    int idx = start;
    /* Setup TRB */
    cmd_ring[idx].a = (uint32_t)((uint64_t)setup_phys & 0xFFFFFFFFu);
    cmd_ring[idx].b = (uint32_t)(((uint64_t)setup_phys >> 32) & 0xFFFFFFFFu);
    cmd_ring[idx].c = 8;
    cmd_ring[idx].d = (XHCI_TRB_TYPE_SETUP_STAGE << 10);
    idx++;
    int trbs = 1;
    if(data && data_len>0){
        if(idx >= XHCI_CMD_RING_TRBS) idx = 0;
        cmd_ring[idx].a = (uint32_t)((uint64_t)data_phys & 0xFFFFFFFFu);
        cmd_ring[idx].b = (uint32_t)(((uint64_t)data_phys >> 32) & 0xFFFFFFFFu);
        cmd_ring[idx].c = (uint32_t)data_len;
        cmd_ring[idx].d = (XHCI_TRB_TYPE_DATA_STAGE << 10) | (direction_in? (1<<1) : 0);
        idx++;
        trbs++;
        if(idx >= XHCI_CMD_RING_TRBS) idx = 0;
        cmd_ring[idx].a = cmd_ring[idx].b = cmd_ring[idx].c = 0;
        cmd_ring[idx].d = (XHCI_TRB_TYPE_STATUS_STAGE << 10);
        idx++;
        trbs++;
    } else {
        if(idx >= XHCI_CMD_RING_TRBS) idx = 0;
        cmd_ring[idx].a = cmd_ring[idx].b = cmd_ring[idx].c = 0;
        cmd_ring[idx].d = (XHCI_TRB_TYPE_STATUS_STAGE << 10);
        idx++;
        trbs++;
    }
    last_cmd_start = start;
    last_cmd_trb_count = trbs;
    return trbs;
}

/* Submit the prepared command ring to the controller. NOT IMPLEMENTED: this
 * currently only logs the planned submission. Real submission requires
 * writing CRCR/doorbells and handling event ring completions.
 */
int xhci_submit_command_ring(int trb_count, void *user_buf, void *data_v, int data_len, int direction_in){
    serial_puts("usb/xhci: submit_command_ring trb_count="); serial_putdec((uint32_t)trb_count);
    serial_puts(" phys=0x"); serial_puthex32((uint32_t)cmd_ring_phys);
    serial_puts(" hw_enable="); serial_putdec((uint32_t)xhci_hw_enable); serial_puts("\n");
    if(!xhci_hw_enable){
        /* dry-run mode: no hardware activity. Mark the first command TRB as pending
         * to make diagnostics visible without touching hardware. This helps when
         * running on real machines but intentionally keeping MMIO writes disabled.
         */
        if(last_cmd_start>=0){
            /* mark the TRBs we prepared as pending so diagnostics reflect them */
            for(int i=0;i<last_cmd_trb_count;i++){
                int dst = last_cmd_start + i;
                if(dst>=XHCI_CMD_RING_TRBS) dst -= XHCI_CMD_RING_TRBS;
                xhci_cmd_mark_pending(dst);
            }
            xhci_cmd_print_pending();
        }
        return 0;
    }
    /* Real submission path (simplified):
     * - allocate DCBAA and device context pages if not already present
     * - set DCBAAP to point to DCBAA
     * - enable a slot (via xhci_send_enable_slot), initialize device context
     * - write CRCR to point at the command ring
     * - set USBCMD run bit and program event ring
     * - ring doorbell for command ring and poll for completion via event ring
     */
    if(!g_dcbaa){
    g_dcbaa = kmalloc(4096);
    if(!g_dcbaa) return -1;
#ifdef VIRT_TO_PHYS
    g_dcbaa_phys = (uintptr_t)VIRT_TO_PHYS(g_dcbaa);
#else
    g_dcbaa_phys = (uintptr_t)g_dcbaa;
#endif
    /* zero DCBAA */
    for(size_t i=0;i<4096/sizeof(uintptr_t);i++) ((uintptr_t*)g_dcbaa)[i]=0;
    }
    /* allocate and zero a device context page for slot 1 */

    g_dev_ctx = kmalloc(4096);
    if(!g_dev_ctx) return -1;
#ifdef VIRT_TO_PHYS
    g_dev_ctx_phys = (uintptr_t)VIRT_TO_PHYS(g_dev_ctx);
#else
    g_dev_ctx_phys = (uintptr_t)g_dev_ctx;
#endif
    for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)g_dev_ctx)[i]=0;

    /* place the device context physical address into DCBAA entry 1 (slot 1)
     * per xHCI DCBAA layout (index 0 reserved). */
    ((uintptr_t*)g_dcbaa)[1] = g_dev_ctx_phys;
    printf("usb/xhci: allocated DCBAA at %p phys=0x%p and dev_ctx at %p phys=0x%p\n",
           g_dcbaa, (void*)g_dcbaa_phys, g_dev_ctx, (void*)g_dev_ctx_phys);
    serial_puts("usb/xhci: allocated DCBAA at ");
    serial_puthex32((uint32_t)(uintptr_t)g_dcbaa);
    serial_puts(" phys=0x"); serial_puthex32((uint32_t)g_dcbaa_phys);
    serial_puts(" and dev_ctx at "); serial_puthex32((uint32_t)(uintptr_t)g_dev_ctx);
    serial_puts(" phys=0x"); serial_puthex32((uint32_t)g_dev_ctx_phys); serial_puts("\n");

    /* If slot not yet enabled, try a minimal enable flow (command TRB) */
    if(!g_slot_enabled){
        if(xhci_send_enable_slot()==0){
                if(xhci_init_device_context()==0){
                g_slot_enabled = 1;
                serial_puts("usb/xhci: slot enabled (reported by command completion)\n");
            }
        }
    }

    /* DCBAAP register offset in xHCI spec (operational) is 0x30 */
    xhci_op_write32(0x30, (uint32_t)(((uint64_t)g_dcbaa_phys) & 0xFFFFFFFFu));
    xhci_op_write32(0x34, (uint32_t)((((uint64_t)g_dcbaa_phys)>>32)&0xFFFFFFFFu));

    /* We wrote TRBs in-place into the ring at last_cmd_start during prepare.
     * Finalize them by assigning a unique token, setting the cycle bit, and
     * marking pending. Then advance the head.
     */
    if(!cmd_ring) return -1;
    if(last_cmd_start < 0 || last_cmd_trb_count <= 0) return -1;
    uint32_t token = ++cmd_token_counter;
    for(int i=0;i<last_cmd_trb_count;i++){
        int dst = last_cmd_start + i;
        if(dst >= XHCI_CMD_RING_TRBS) dst -= XHCI_CMD_RING_TRBS;
        /* set cycle bit in d according to current ring cycle */
        uint32_t d = cmd_ring[dst].d;
        if(cmd_ring_cycle & 0x1) d |= XHCI_TRB_CYCLE_BIT; else d &= ~XHCI_TRB_CYCLE_BIT;
        cmd_ring[dst].d = d;
        /* store token in param_lo (DW0) so completion events referencing this TRB
         * carry our token for easier matching */
        cmd_ring[dst].a = token;
        cmd_ring_token[dst] = token;
        xhci_cmd_mark_pending(dst);
    }
    /* compute new head */
    cmd_ring_head += last_cmd_trb_count;
    if(cmd_ring_head >= XHCI_CMD_RING_TRBS){
        cmd_ring_head -= XHCI_CMD_RING_TRBS;
        /* toggle cycle on wrap */
        cmd_ring_cycle ^= 1;
    }
    last_cmd_token = token;

    /* Ensure controller is running before publishing the CRCR pointer. Some
     * controllers ignore CRCR writes while the controller is stopped. Set the
     * USBCMD Run bit first, then write CRCR. */
    uint32_t usbcmd = xhci_op_read32(0x00);
    usbcmd |= 0x1;
    xhci_op_write32(0x00, usbcmd);

    /* Dump a few TRBs for diagnostics before publishing CRCR */
    serial_puts("usb/xhci: dumping first 16 TRBs before CRCR\n");
    xhci_dump_cmd_ring(16);

    /* Program event ring and ring doorbell for command ring (db 0) */
    xhci_init_event_ring();
    xhci_dump_event_ring();
    if(xhci_program_event_ring()<0){
        serial_puts("usb/xhci: failed to program event ring\n");
    } else {
    /* After event ring is programmed, publish the CRCR pointer so the controller
     * can process the command TRBs. Some controllers require ERST/ERDP to be
     * visible before CRCR/CRR is written. */
    uint64_t crcr_phys = (uint64_t)cmd_ring_phys + ((uint64_t)last_cmd_start * sizeof(struct xhci_trb));
    xhci_op_write32(0x18, (uint32_t)(crcr_phys & 0xFFFFFFFFu));
    xhci_op_write32(0x1C, (uint32_t)(((uint64_t)crcr_phys >> 32) & 0xFFFFFFFFu));
    serial_puts("usb/xhci: CRCR written low=0x"); serial_puthex32((uint32_t)(crcr_phys & 0xFFFFFFFFu));
    serial_puts(" high=0x"); serial_puthex32((uint32_t)((crcr_phys>>32)&0xFFFFFFFFu)); serial_puts("\n");
    /* Read back CRCR and USBSTS to verify controller observed the pointer */
    uint32_t crcr_lo_rb = xhci_op_read32(0x18);
    uint32_t crcr_hi_rb = xhci_op_read32(0x1C);
    uint32_t usbsts_rb = xhci_op_read32(0x04);
    serial_puts("usb/xhci: CRCR rb low=0x"); serial_puthex32(crcr_lo_rb);
    serial_puts(" high=0x"); serial_puthex32(crcr_hi_rb);
    serial_puts(" USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
    /* small delay after CRCR/ERDP programming to give controller time to observe pointers */
    for(volatile int z=0; z<10000; z++);
    /* ring doorbell for command ring (db 0). Value is 0 for command ring in many controllers. */
    xhci_ring_doorbell(0, 0);
    /* read back USBSTS/CRCR after ringing doorbell */
    crcr_lo_rb = xhci_op_read32(0x18);
    crcr_hi_rb = xhci_op_read32(0x1C);
    usbsts_rb = xhci_op_read32(0x04);
    serial_puts("usb/xhci: after doorbell CRCR rb low=0x"); serial_puthex32(crcr_lo_rb);
    serial_puts(" high=0x"); serial_puthex32(crcr_hi_rb);
    serial_puts(" USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
    int r = xhci_poll_event_ring(500, data_v, user_buf, data_len, direction_in);
        if(r==0) serial_puts("usb/xhci: completion detected\n"); else serial_puts("usb/xhci: completion timeout\n");
    }
    return 0;
}

// xHCI PCI class: base 0x0C (Serial Bus), subclass 0x03
#define XHCI_CLASS 0x0C
#define XHCI_SUBCLASS 0x03

struct xhci_context {
    uint8_t found;
    uint8_t bus, slot, func;
    uintptr_t phys_base; /* BAR0 physical base */
    volatile uint8_t *cap_base; /* capability regs base */
    volatile uint8_t *op_base;   /* operational regs base (caplen offset)
                                   * op_base = cap_base + caplen
                                   */
    uint8_t caplen;
    uint8_t num_ports;
    int cap_is_io; /* true if BAR0 is IO-space */
};

static struct xhci_context xhci = {0};

/* Event ring bookkeeping (allocated but not written to hardware until enabled) */
static void *er_buffer = NULL;
static uintptr_t er_buffer_phys = 0;
static void *erst = NULL; /* Event Ring Segment Table memory */
static uintptr_t erst_phys = 0;
static uint32_t erst_size = 0;
/* Event ring runtime dequeue state: index within the single-segment ER buffer
 * and expected cycle state (initially 1). These persist across polls so we
 * correctly follow the xHCI cycle-bit protocol when consuming events. */
static uint32_t er_dequeue_idx = 0;
static uint32_t er_expected_cycle = 1;

int xhci_init_event_ring(void){
    if(er_buffer) return 0;
    /* allocate one page for event ring buffer */
    void *p = kmalloc(4096);
    if(!p) return -1;
    er_buffer = p;
#ifdef VIRT_TO_PHYS
    er_buffer_phys = (uintptr_t)VIRT_TO_PHYS(er_buffer);
#else
    er_buffer_phys = (uintptr_t)er_buffer;
#endif
    /* zero buffer */
    for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)er_buffer)[i]=0;

    /* allocate a small ERST (we'll keep one entry) */
    void *s = kmalloc(4096);
    if(!s) return -1;
    erst = s;
#ifdef VIRT_TO_PHYS
    erst_phys = (uintptr_t)VIRT_TO_PHYS(erst);
#else
    erst_phys = (uintptr_t)erst;
#endif
    for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)erst)[i]=0;
    erst_size = 1; /* one segment */
    /* Populate ERST first entry: qword base address + dword segment size
     * Per xHCI ERST entry layout: [0x0] qword Segment Base Address
     * [0x8] dword Segment Size (number of TRBs), [0xC] reserved
     */
    {
        uint64_t *e64 = (uint64_t*)(uintptr_t)erst;
        uint32_t *e32 = (uint32_t*)((uintptr_t)erst + 8);
        e64[0] = (uint64_t)er_buffer_phys;
        /* number of TRBs in our single segment = page(4096)/16 */
        uint32_t seg_trbs = (uint32_t)(4096 / 16);
        e32[0] = seg_trbs;
        /* ensure remaining fields zeroed (already cleared above) */
    }
    /* start dequeue at 0 and expect cycle 1 initially */
    er_dequeue_idx = 0;
    er_expected_cycle = 1;
    return 0;
}

/* Minimal soft-enable slot: on real hardware this should submit an Enable Slot
 * command via the command ring and wait for a Command Completion Event. Here
 * we perform conservative bookkeeping and do not touch hardware unless
 * xhci_hw_enable is set; the function returns 0 on local success and -1 on
 * error.
 */
int xhci_send_enable_slot(void){
    /* If hardware writes are disabled, act as soft success */
    if(!xhci_hw_enable){
        serial_puts("usb/xhci: (soft) enable slot - hw disabled, pretending success\n");
        return 0;
    }

    /* Ensure command ring exists */
    if(!cmd_ring){
        if(xhci_init_command_ring()<0) return -1;
    }

    /* Build a single-command TRB at index 0 for Enable Slot. Real drivers
     * must follow xHCI command TRB encodings; here we use a conservative
     * placement consistent with our TRB parsing (type in d >> 10) and set
     * the cycle bit so the controller will process it when CRCR is written.
     */
    xhci_build_command_trb(0, XHCI_TRB_TYPE_ENABLE_SLOT_CMD, 0, 0);
    /* explicitly set cycle bit in TRB.d */
    cmd_ring[0].d |= XHCI_TRB_CYCLE_BIT;

    /* Ensure controller is running before publishing the CRCR pointer. Set
     * the USBCMD Run bit first on the operational registers, then write CRCR. */
    uint32_t usbcmd = xhci_op_read32(0x00);
    usbcmd |= 0x1;
    xhci_op_write32(0x00, usbcmd);

    /* Dump TRB[0..7] for diagnostics so we can see the Enable Slot TRB we built */
    serial_puts("usb/xhci: dumping cmd ring first 8 TRBs (enable_slot)\n");
    xhci_dump_cmd_ring(8);
    /* Program event ring and ring doorbell for command ring (DB 0) before CRCR write.
     * Ensure ERST/ERDP are present so controller can post events in response to
     * the command we will publish. */
    xhci_init_event_ring();
    xhci_program_event_ring();
    /* After event ring is ready, publish CRCR pointing to the command ring */
    uint64_t crcr = (uint64_t)xhci_command_ring_phys();
    if(crcr==0) return -1;
    xhci_op_write64(0x18, crcr | 1ULL);
    serial_puts("usb/xhci: CRCR written (enable_slot) low=0x"); serial_puthex32((uint32_t)(crcr & 0xFFFFFFFFu));
    serial_puts(" high=0x"); serial_puthex32((uint32_t)((crcr>>32)&0xFFFFFFFFu)); serial_puts(" flags=1\n");
    uint32_t crcr_lo_rb = xhci_op_read32(0x18);
    uint32_t crcr_hi_rb = xhci_op_read32(0x1C);
    serial_puts("usb/xhci: CRCR readback low=0x"); serial_puthex32(crcr_lo_rb);
    serial_puts(" high=0x"); serial_puthex32(crcr_hi_rb); serial_puts("\n");
    uint32_t usbsts_rb = xhci_op_read32(0x04);
    serial_puts("usb/xhci: USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
    /* give controller a short time to observe CRCR/ERDP before doorbell */
    for(volatile int z=0; z<10000; z++);
    xhci_ring_doorbell(0, 0);

    /* Wait for a command completion event. Our event parser treats any
     * valid TRB at the dequeue as a completion for now. A robust driver must
     * parse Command Completion Events specifically and decode completion
     * codes. We reuse xhci_poll_event_ring which copies IN payloads if
     * present; for Enable Slot no payload expected.
     */
    int comp = -1;
    /* Try several attempts with longer timeout to accommodate slow controllers */
    const int attempts = 3;
    for(int a=0;a<attempts;a++){
        int r = xhci_wait_for_command_completion(2000, &comp);
        if(r==0){
            serial_puts("usb/xhci: enable-slot completion code="); serial_putdec((uint32_t)comp); serial_puts("\n");
            return (comp==0)?0:-1;
        }
        serial_puts("usb/xhci: enable-slot attempt timed out, dumping ER state\n");
        xhci_dump_event_ring();
        /* small pause before retry */
        for(volatile int z=0; z<1000000; z++);
    }
    serial_puts("usb/xhci: enable-slot command failed after retries\n");
    /* Try alternate CRCR sequence: write pointer without flags, small delay, then set flags. */
    serial_puts("usb/xhci: trying alternate CRCR sequence for enable-slot\n");
    uint64_t crcr_ptr = (uint64_t)xhci_command_ring_phys();
    /* write pointer without setting run/CRR flag */
    xhci_op_write32(0x18, (uint32_t)(crcr_ptr & 0xFFFFFFFFu));
    xhci_op_write32(0x1C, (uint32_t)((crcr_ptr>>32)&0xFFFFFFFFu));
    /* small delay */
    for(volatile int z=0; z<100000; z++);
    /* now set the low flags (CRR) by ORing in 1 */
    uint32_t crcr_lo_now = xhci_op_read32(0x18);
    xhci_op_write32(0x18, crcr_lo_now | 1);
    serial_puts("usb/xhci: alternate CRCR written, ringing doorbell\n");
    xhci_ring_doorbell(0, 0);
    /* wait once more for completion */
    int r2 = xhci_wait_for_command_completion(2000, NULL);
    if(r2==0){ serial_puts("usb/xhci: enable-slot completion observed after alt sequence\n"); return 0; }
    serial_puts("usb/xhci: alternate enable-slot attempt failed\n");
    return -1;
}

/* Initialize the device context page with conservative defaults so controllers
 * have a plausible context to read when DCBAAP points to it. This writes zeros
 * to the page and places some safe fields in the first dwords where xHCI may
 * expect context size and endpoint context pointers. This is not a full,
 * spec-correct device context but is enough for early testing on some
 * controllers.
 */
int xhci_init_device_context(void){
    if(!g_dev_ctx) return -1;
    /* zero page */
    for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)g_dev_ctx)[i]=0;

    /*
     * Heuristic / conservative device context initialization
     * Assumptions:
     * - We'll advertise 1 endpoint context (EP0) in the Slot Context so
     *   controllers know to read the endpoint context area.
     * - Many controllers expect the Endpoint TR Dequeue Pointer to appear
     *   at one of several qword offsets inside the endpoint context region
     *   (we populate 0x08, 0x10 and 0x18 to maximize compatibility).
     * - Set a conservative MaxPacketSize for EP0 (64 bytes) in the endpoint
     *   context DW1 low 16-bits. These choices are pragmatic and may need
     *   tuning per-controller.
     */
    /* Slot Context: set "Context Entries" to 1 (means one endpoint context)
     * This is a pragmatic approximation: some controllers look at low bytes
     * of the first dword in the device context to know how many contexts are
     * present. If your controller is strict, a spec-accurate layout may be
     * required instead. */
    ((uint32_t*)g_dev_ctx)[0] = 1u;

    /* Ensure an EP0 ring exists */
    if(!ep0_ring){
        ep0_ring = kmalloc(4096);
        if(ep0_ring){
#ifdef VIRT_TO_PHYS
            ep0_ring_phys = (uintptr_t)VIRT_TO_PHYS(ep0_ring);
#else
            ep0_ring_phys = (uintptr_t)ep0_ring;
#endif
            for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)ep0_ring)[i]=0;
            /* set first TRB cycle bit so some controllers see a valid TRB */
            ((uint32_t*)ep0_ring)[3] = XHCI_TRB_CYCLE_BIT;
        }
    }
    if(!ep0_ring){
        ep0_ring = kmalloc(4096);
        if(ep0_ring){
#ifdef VIRT_TO_PHYS
            ep0_ring_phys = (uintptr_t)VIRT_TO_PHYS(ep0_ring);
#else
            ep0_ring_phys = (uintptr_t)ep0_ring;
#endif
            for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)ep0_ring)[i]=0;
            /* set first TRB cycle bit so some controllers see a valid TRB */
            ((uint32_t*)ep0_ring)[3] = XHCI_TRB_CYCLE_BIT;
        }
    }

    /* Endpoint context start is at offset 0x20 within the device context per xHCI */
    uintptr_t ec_base = (uintptr_t)g_dev_ctx + 0x20;
    if(ep0_ring_phys){
        /* Ensure an RST (Ring Segment Table) exists for EP0 and point it to the ring */
        if(!ep0_rst){
            ep0_rst = kmalloc(4096);
            if(ep0_rst){
#ifdef VIRT_TO_PHYS
                ep0_rst_phys = (uintptr_t)VIRT_TO_PHYS(ep0_rst);
#else
                ep0_rst_phys = (uintptr_t)ep0_rst;
#endif
                /* Zero RST and place one segment entry pointing at ep0_ring_phys */
                for(size_t i=0;i<4096/sizeof(uint32_t);i++) ((uint32_t*)ep0_rst)[i]=0;
                /* RST entry 0: segment base address (qword at offset 0) */
                *((uint64_t*)((uintptr_t)ep0_rst + 0)) = (uint64_t)ep0_ring_phys;
            }
        }

        /* Write RST pointer into a couple of plausible endpoint context locations
         * Many controllers expect the Ring Segment Table pointer in EC DW6/DW7
         * (offsets +0x20/+0x24 relative to EC start) or similar; we conservatively
         * write at ec_base + 0x20 (qword) and at ec_base + 0x28 as an alternate.
         */
        if(ep0_rst_phys){
            *((uint64_t*)(ec_base + 0x20)) = (uint64_t)ep0_rst_phys;
            *((uint64_t*)(ec_base + 0x28)) = (uint64_t)ep0_rst_phys;
            /* RST Size (number of segments) is 1: place at ec_base+0x30 (dword) as a guess */
            *((uint32_t*)(ec_base + 0x30)) = 1u;
        }
        /* Populate several common TR Dequeue Pointer locations (qword) */
        /* EC +0x08 (dwords 2/3) */
        *((uint64_t*)(ec_base + 0x08)) = (uint64_t)ep0_ring_phys;
        /* EC +0x10 (dwords 4/5) - many controllers expect TR Dequeue here */
        *((uint64_t*)(ec_base + 0x10)) = (uint64_t)ep0_ring_phys;
        /* EC +0x18 (dwords 6/7) - alternate placement */
        *((uint64_t*)(ec_base + 0x18)) = (uint64_t)ep0_ring_phys;

        /* Set a conservative Max Packet Size (DW1 low 16 bits) to 64 for EP0 */
        uint32_t *ec_dw1 = (uint32_t*)(ec_base + 0x04);
        *ec_dw1 = (*ec_dw1 & 0xFFFF0000u) | (64u & 0xFFFFu);

        /* Leave Endpoint State/DW0 as zero (default). A full driver should
         * populate proper Endpoint Context fields (Interval, Mult, MaxBurst,
         * Transfer Type etc.) according to the xHCI spec. */
    }

    return 0;
}

/* USB setup packet layout */
struct usb_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* Create and submit a simple control transfer: Setup [+ Data] + Status.
 * This uses the existing TRB builders and submit path. In dry-run mode
 * (xhci_hw_enable==0) this will not talk to hardware but exercises the
 * code paths and returns success/failure accordingly.
 */
int xhci_control_transfer(uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex,
                          void *data, int data_len, int direction_in,
                          void *resp_buf, int resp_len){
    struct usb_setup *s = (struct usb_setup*)kmalloc(sizeof(struct usb_setup));
    if(!s) return -1;
    s->bmRequestType = bmRequestType;
    s->bRequest = bRequest;
    s->wValue = wValue;
    s->wIndex = wIndex;
    s->wLength = (uint16_t)data_len;
#ifdef VIRT_TO_PHYS
    uintptr_t s_phys = (uintptr_t)VIRT_TO_PHYS(s);
#else
    uintptr_t s_phys = (uintptr_t)s;
#endif

    uintptr_t data_phys = 0;
    void *data_v = NULL;
    if(data_len>0){
        data_v = kmalloc(data_len);
        if(!data_v) return -1;
        /* caller provided buffer may be used as initial data for OUT; copy if present */
        if(!direction_in && data) for(int i=0;i<data_len;i++) ((uint8_t*)data_v)[i]=((uint8_t*)data)[i];
#ifdef VIRT_TO_PHYS
        data_phys = (uintptr_t)VIRT_TO_PHYS(data_v);
#else
        data_phys = (uintptr_t)data_v;
#endif
    }

    int trbs = xhci_prepare_control_transfer((const void*)s, s_phys, data_v, data_phys, data_len, direction_in);
    if(trbs<0) return -1;

    int r = xhci_submit_command_ring(trbs, resp_buf, data_v, data_len, direction_in);
    return r;
}

/* Minimal wrappers for common requests */
int xhci_set_address(uint8_t addr){
    /* SET_ADDRESS: bm=0x00, bRequest=5, wValue=addr, wIndex=0, wLength=0 */
    return xhci_control_transfer(0x00, 5, (uint16_t)addr, 0, NULL, 0, 0, NULL, 0);
}

int xhci_get_device_descriptor(void *buf, int len){
    /* GET_DESCRIPTOR device: bm=0x80 (IN, standard, device), bRequest=6, wValue=(1<<8), wIndex=0 */
    return xhci_control_transfer(0x80, 6, (uint16_t)((1<<8)|0), 0, NULL, len, 1, buf, len);
}

/* Small convenience test: attempt to enable slot then read device descriptor */
int xhci_enumerate_once(void){
    if(!g_slot_enabled){
        if(xhci_send_enable_slot()!=0) { printf("usb/xhci: enable slot failed\n"); return -1; }
        if(xhci_init_device_context()!=0){ printf("usb/xhci: device context init failed\n"); return -1; }
        g_slot_enabled = 1;
    }
    uint8_t desc[18];
    for(int i=0;i<18;i++) desc[i]=0;
    int r = xhci_get_device_descriptor(desc, 18);
    if(r==0){
        serial_puts("usb/xhci: device descriptor:\n");
        for(int i=0;i<18;i++){ serial_puts(" "); serial_puthex8((uint8_t)desc[i]); }
        serial_puts("\n");
    } else {
        serial_puts("usb/xhci: get descriptor failed (r="); serial_putdec((uint32_t)r); serial_puts(")\n");
    }
    /* Try to fetch the full configuration descriptor and inspect interfaces
     * to detect potential network (RNDIS/CDC) interfaces. This is a best-effort
     * diagnostic useful for tethering support implementation. */
    if(r==0){
        /* first read 9-byte config header to get total length */
        uint8_t confhdr[9];
        for(int i=0;i<9;i++) confhdr[i]=0;
        int h = xhci_get_config_descriptor(confhdr, 9);
        if(h==0){
            /* total length is little-endian word at offset 2 */
            int totlen = confhdr[2] | (confhdr[3]<<8);
            if(totlen > 0 && totlen <= 4096){
                uint8_t *full = (uint8_t*)kmalloc(totlen);
                if(full){
                    for(int i=0;i<totlen;i++) full[i]=0;
                    int h2 = xhci_get_config_descriptor(full, totlen);
                    if(h2==0){
                        serial_puts("usb/xhci: fetched full config descriptor, len="); serial_putdec((uint32_t)totlen); serial_puts("\n");
                        /* parse descriptors and log interfaces */
                        int off = 0;
                        int found_net = 0;
                        while(off + 2 <= totlen){
                            uint8_t blen = full[off+0];
                            uint8_t dtype = full[off+1];
                            if(blen < 2) break;
                            if(dtype == 0x04 && blen >= 9){ /* Interface descriptor */
                                uint8_t if_class = full[off+5];
                                uint8_t if_sub = full[off+6];
                                uint8_t if_proto = full[off+7];
                                serial_puts("usb/xhci: Interface found class=0x"); serial_puthex8(if_class);
                                serial_puts(" sub=0x"); serial_puthex8(if_sub);
                                serial_puts(" proto=0x"); serial_puthex8(if_proto); serial_puts("\n");
                                /* heuristics: CDC Communications (0x02) or CDC Data (0x0A)
                                 * or vendor-specific (0xFF) are candidates for tethering */
                                if(if_class == 0x02 || if_class == 0x0A || if_class == 0xFF){
                                    found_net = 1;
                                }
                            }
                            off += blen;
                            if(off >= totlen) break;
                        }
                        if(found_net){
                            serial_puts("usb/xhci: network-capable interface detected (candidate for tethering)\n");
                        } else {
                            serial_puts("usb/xhci: no network-capable interface detected in config descriptor\n");
                        }
                    } else {
                        serial_puts("usb/xhci: failed to fetch full config descriptor second pass\n");
                    }
                    /* keep full descriptor in memory for debugging if desired */
                } else {
                    serial_puts("usb/xhci: failed to allocate buffer for full config descriptor\n");
                }
            } else {
                serial_puts("usb/xhci: config descriptor reported invalid total length\n");
            }
        } else {
            serial_puts("usb/xhci: failed to fetch config header\n");
        }
    }
    return r;
}

/* Helper: fetch configuration descriptor via GET_DESCRIPTOR (Configuration)
 * Returns 0 on success, -1 on failure. Uses existing control transfer path.
 */
int xhci_get_config_descriptor(void *buf, int len){
    if(len <= 0 || buf==NULL) return -1;
    /* bmRequestType: 0x80 (IN, Standard, Device), bRequest=6 GET_DESCRIPTOR
     * wValue = (CONFIGURATION<<8) | index(0)
     */
    return xhci_control_transfer(0x80, 6, (2<<8)|0, 0, NULL, len, 1, buf, len);
}

void xhci_dump_event_ring(void){
    serial_puts("usb/xhci: ER buffer virt=0x"); serial_puthex32((uint32_t)(uintptr_t)er_buffer);
    serial_puts(" phys=0x"); serial_puthex32((uint32_t)er_buffer_phys);
    serial_puts(" ERST=0x"); serial_puthex32((uint32_t)(uintptr_t)erst);
    serial_puts(" phys=0x"); serial_puthex32((uint32_t)erst_phys);
    serial_puts(" size="); serial_putdec((uint32_t)erst_size); serial_puts("\n");
    /* Dump first few Event TRBs for debugging */
    if(er_buffer){
        volatile uint32_t *buf = (volatile uint32_t*)er_buffer;
        int max = 8;
        serial_puts("usb/xhci: first few ER TRBs:\n");
        for(int i=0;i<max;i++){
            uint32_t dw0 = buf[i*4 + 0];
            uint32_t dw1 = buf[i*4 + 1];
            uint32_t dw2 = buf[i*4 + 2];
            uint32_t dw3 = buf[i*4 + 3];
            serial_puts(" usb/xhci: ER["); serial_putdec((uint32_t)i); serial_puts("]=");
            serial_puts(" dw3=0x"); serial_puthex32(dw3); serial_puts(" dw2=0x"); serial_puthex32(dw2);
            serial_puts(" dw1=0x"); serial_puthex32(dw1); serial_puts(" dw0=0x"); serial_puthex32(dw0); serial_puts("\n");
        }
    }
}

static inline uint32_t xhci_cap_read32(uint32_t off){
    if(xhci.cap_is_io){
        return inl((uintptr_t)xhci.phys_base + off);
    }
    return *((volatile uint32_t*)(xhci.cap_base + off));
}
static inline uint32_t xhci_op_read32(uint32_t off){
    if(xhci.cap_is_io){
        return inl((uintptr_t)xhci.phys_base + (uintptr_t)(xhci.caplen) + off);
    }
    return *((volatile uint32_t*)(xhci.op_base + off));
}
static inline void xhci_op_write32(uint32_t off, uint32_t val){
    if(xhci.cap_is_io){
        outl((uintptr_t)xhci.phys_base + (uintptr_t)(xhci.caplen) + off, val);
        return;
    }
    *((volatile uint32_t*)(xhci.op_base + off)) = val;
}

static inline void xhci_op_write64(uint32_t off_lo, uint64_t val){
    if(xhci.cap_is_io){
        outl((uintptr_t)xhci.phys_base + (uintptr_t)(xhci.caplen) + off_lo, (uint32_t)(val & 0xFFFFFFFFu));
        outl((uintptr_t)xhci.phys_base + (uintptr_t)(xhci.caplen) + off_lo + 4, (uint32_t)(((uint64_t)val>>32)&0xFFFFFFFFu));
        return;
    }
    xhci_op_write32(off_lo, (uint32_t)(val & 0xFFFFFFFFu));
    xhci_op_write32(off_lo+4, (uint32_t)(((uint64_t)val>>32)&0xFFFFFFFFu));
}

/* Program ERST/ERDP to point at the previously allocated ER buffer and ERST.
 * This function is conservative: it programs minimal ERST with one segment and
 * leaves ERDP zeroed until we explicitly set it. Returns 0 on success.
 */
int xhci_program_event_ring(void){
    if(!er_buffer || !erst) return -1;
    /* ERSTSZ is at operational offset 0x28 (ERST Size) per some implementations.
     * Many controllers provide EVENT_RING registers at offsets near 0x20..0x30.
     * We'll write ERST (ERSTADDR) at offset 0x20/0x24 (lower/upper) and ERSTSZ
     * at 0x28, then set ERDP (Event Ring Dequeue Pointer) at 0x30/0x34.
     * These offsets are controller-dependent. Keep writes gated and logged.
     */
    serial_puts("usb/xhci: programming ERST -> erst_phys=0x"); serial_puthex32((uint32_t)erst_phys);
    serial_puts(" erbuf_phys=0x"); serial_puthex32((uint32_t)er_buffer_phys); serial_puts("\n");
    /* Diagnostic: dump first 0x200 bytes of operational registers (32-bit words)
     * This helps discover where ERST/ERDP-like registers live on this controller.
     */
    serial_puts("usb/xhci: dumping op regs (0x00..0x3FC) - non-zero only\n");
    /* Scan a larger range of operational registers and print only non-zero
     * values so we can more easily locate ERST/ERDP/doorbell/DD regs on
     * controllers that expose them at non-standard offsets. */
    for(uint32_t off=0; off<0x400; off+=4){
        uint32_t v = xhci_op_read32(off);
        if(v != 0){
            serial_puts("op[0x"); serial_puthex32(off); serial_puts("]=0x"); serial_puthex32(v); serial_puts("\n");
        }
    }
    /* Try multiple candidate offsets for ERST/ERSTSZ/ERDP because virtual
     * controllers sometimes expose these registers at different bases.
     * We'll try bases 0x20, 0x30 and 0x40. For each base B we assume layout:
     *  B + 0x00: ERST base addr low
     *  B + 0x04: ERST base addr high
     *  B + 0x08: ERST size (dword)
     *  B + 0x10: ERDP low
     *  B + 0x14: ERDP high
     * Accept the first base where readbacks reflect our writes.
     */
    uint64_t erdp = er_buffer_phys + ((uint64_t)er_dequeue_idx * 16ULL);
    uint32_t erdp_ptr_low = (uint32_t)(erdp & 0xFFFFFFFFu);
    uint32_t erdp_ptr_hi  = (uint32_t)(((uint64_t)erdp>>32)&0xFFFFFFFFu);

    int chosen_base = -1;
    /* Sweep a wide range of candidate bases across the operational register
     * area to discover where the controller expects ERST/ERDP/IMAN/IMOD to live.
     * We step by 8 to stay dword-aligned and stop early if we find a base that
     * reflects our writes. This is noisy but necessary for interoperability with
     * different virtual controller implementations. */
    for(uint32_t B = 0x20u; B < 0x3F0u; B += 8u){
        serial_puts("usb/xhci: attempting ER programming at base 0x"); serial_puthex32(B); serial_puts("\n");
        /* write ERST addr low/high and ERST size */
        xhci_op_write32(B + 0x00, (uint32_t)(erst_phys & 0xFFFFFFFFu));
        xhci_op_write32(B + 0x04, (uint32_t)(((uint64_t)erst_phys>>32)&0xFFFFFFFFu));
        xhci_op_write32(B + 0x08, (uint32_t)erst_size);
        /* small delay */
        for(volatile int z=0; z<1000; z++);
        /* Step 1: write ERDP pointer without cycle bit to probe readback */
        xhci_op_write32(B + 0x10, erdp_ptr_low & ~0x1u);
        xhci_op_write32(B + 0x14, erdp_ptr_hi);
        for(volatile int z=0; z<2000; z++);
        /* read back to see if the controller reflects the written pointers */
        uint32_t erst_lo_rb = xhci_op_read32(B + 0x00);
        uint32_t erst_hi_rb = xhci_op_read32(B + 0x04);
        uint32_t erst_sz_rb = xhci_op_read32(B + 0x08);
        uint32_t erdp_lo_rb = xhci_op_read32(B + 0x10);
        uint32_t erdp_hi_rb = xhci_op_read32(B + 0x14);
        uint32_t usbsts_rb  = xhci_op_read32(0x04);
        serial_puts("usb/xhci: RB[base=0x"); serial_puthex32(B); serial_puts("] ERST_lo=0x"); serial_puthex32(erst_lo_rb);
        serial_puts(" ERST_hi=0x"); serial_puthex32(erst_hi_rb);
        serial_puts(" ERST_sz=0x"); serial_puthex32(erst_sz_rb);
        serial_puts(" ERDP_lo=0x"); serial_puthex32(erdp_lo_rb);
        serial_puts(" ERDP_hi=0x"); serial_puthex32(erdp_hi_rb);
        serial_puts(" USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
        /* If readback shows ERST or ERDP low matching our pointer or non-zero, treat as promising */
        if(erst_lo_rb == (uint32_t)(erst_phys & 0xFFFFFFFFu) || erdp_lo_rb == erdp_ptr_low || erdp_lo_rb != 0){
            chosen_base = (int)B;
            serial_puts("usb/xhci: selected ER base 0x"); serial_puthex32(B); serial_puts(" based on readback\n");
            /* Try writing IMOD (moderation) then enabling IMAN at this base. IMOD often
             * resides at base+0x04 and IMAN at base+0x00; toggle them to encourage events. */
            serial_puts("usb/xhci: setting IMOD then enabling IMAN at base 0x"); serial_puthex32(B); serial_puts("\n");
            /* Conservative IMOD value: a small value to avoid flooding */
            xhci_op_write32(B + 0x04, 0x100u);
            for(volatile int z=0; z<500; z++);
            xhci_op_write32(B + 0x00, 0x1u);
            uint32_t iman_rb = xhci_op_read32(B + 0x00);
            uint32_t imod_rb = xhci_op_read32(B + 0x04);
            serial_puts("usb/xhci: IMAN rb=0x"); serial_puthex32(iman_rb); serial_puts(" IMOD rb=0x"); serial_puthex32(imod_rb); serial_puts("\n");
            /* Step 2: set cycle bit in ERDP low at chosen base */
            uint32_t erdp_low_with_cycle = erdp_ptr_low | (er_expected_cycle & 0x1u);
            xhci_op_write32(B + 0x10, erdp_low_with_cycle);
            /* final readback */
            erdp_lo_rb = xhci_op_read32(B + 0x10);
            erdp_hi_rb = xhci_op_read32(B + 0x14);
            usbsts_rb  = xhci_op_read32(0x04);
            serial_puts("usb/xhci: ERDP after cycle set rb_low=0x"); serial_puthex32(erdp_lo_rb);
            serial_puts(" rb_hi=0x"); serial_puthex32(erdp_hi_rb);
            serial_puts(" USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
            break;
        }
        /* otherwise try next candidate */
    }
    if(chosen_base < 0){
        serial_puts("usb/xhci: no ER base candidate appeared to reflect writes; falling back to 0x20\n");
        /* fallback: original behavior at 0x20 */
        xhci_op_write32(0x20, (uint32_t)(erst_phys & 0xFFFFFFFFu));
        xhci_op_write32(0x24, (uint32_t)(((uint64_t)erst_phys>>32)&0xFFFFFFFFu));
        xhci_op_write32(0x28, (uint32_t)erst_size);
        for(volatile int z=0; z<10000; z++);
        xhci_op_write32(0x30, erdp_ptr_low & ~0x1u);
        xhci_op_write32(0x34, erdp_ptr_hi);
        for(volatile int z=0; z<20000; z++);
        uint32_t erdp_lo_rb = xhci_op_read32(0x30);
        uint32_t erdp_hi_rb = xhci_op_read32(0x34);
        uint32_t usbsts_rb  = xhci_op_read32(0x04);
        serial_puts("usb/xhci: fallback ERDP_rb_low=0x"); serial_puthex32(erdp_lo_rb);
        serial_puts(" ERDP_rb_hi=0x"); serial_puthex32(erdp_hi_rb);
        serial_puts(" USBSTS=0x"); serial_puthex32(usbsts_rb); serial_puts("\n");
        uint32_t erdp_low_with_cycle = erdp_ptr_low | (er_expected_cycle & 0x1u);
        xhci_op_write32(0x30, erdp_low_with_cycle);
    }
    /* Additional probes: try writing ERDP at other likely offsets (0x30 and 0x38)
     * Some virtual controllers expose event-ring dequeue at alternate op offsets.
     * We only write the ERDP pointer (two-step) for diagnostic purposes and
     * log readbacks; do not clobber ERST entries here. */
    serial_puts("usb/xhci: probing additional ERDP locations 0x30 and 0x38\n");
    /* Try 0x30/0x34 (already used in many controllers) */
    xhci_op_write32(0x30, erdp_ptr_low & ~0x1u);
    xhci_op_write32(0x34, erdp_ptr_hi);
    for(volatile int z=0; z<15000; z++);
    {
        uint32_t rb30 = xhci_op_read32(0x30);
        uint32_t rb34 = xhci_op_read32(0x34);
        serial_puts("usb/xhci: probe ERDP[0x30] rb_low=0x"); serial_puthex32(rb30);
        serial_puts(" rb_hi=0x"); serial_puthex32(rb34); serial_puts("\n");
    }
    xhci_op_write32(0x30, (erdp_ptr_low | (er_expected_cycle & 0x1u)));
    for(volatile int z=0; z<5000; z++);
    {
        uint32_t rb30 = xhci_op_read32(0x30);
        uint32_t rb34 = xhci_op_read32(0x34);
        serial_puts("usb/xhci: probe ERDP[0x30] after cycle rb_low=0x"); serial_puthex32(rb30);
        serial_puts(" rb_hi=0x"); serial_puthex32(rb34); serial_puts("\n");
    }
    /* Try 0x38/0x3C as another candidate */
    xhci_op_write32(0x38, erdp_ptr_low & ~0x1u);
    xhci_op_write32(0x3C, erdp_ptr_hi);
    for(volatile int z=0; z<15000; z++);
    {
        uint32_t rb38 = xhci_op_read32(0x38);
        uint32_t rb3c = xhci_op_read32(0x3C);
        serial_puts("usb/xhci: probe ERDP[0x38] rb_low=0x"); serial_puthex32(rb38);
        serial_puts(" rb_hi=0x"); serial_puthex32(rb3c); serial_puts("\n");
    }
    xhci_op_write32(0x38, (erdp_ptr_low | (er_expected_cycle & 0x1u)));
    for(volatile int z=0; z<5000; z++);
    {
        uint32_t rb38 = xhci_op_read32(0x38);
        uint32_t rb3c = xhci_op_read32(0x3C);
        serial_puts("usb/xhci: probe ERDP[0x38] after cycle rb_low=0x"); serial_puthex32(rb38);
        serial_puts(" rb_hi=0x"); serial_puthex32(rb3c); serial_puts("\n");
    }
    return 0;
}

/* Doorbell: write to doorbell registers. Many controllers place doorbells at
 * op_base + 0x1000 + 4*DBIndex. We'll attempt a conservative write at 0x1000 + 4*db_index.
 */
void xhci_ring_doorbell(uint32_t db_index, uint32_t value){
    uint32_t off = 0x1000 + (db_index * 4);
    serial_puts("usb/xhci: ringing doorbell index="); serial_putdec(db_index);
    serial_puts(" value=0x"); serial_puthex32(value); serial_puts(" off=0x"); serial_puthex32(off); serial_puts("\n");
    xhci_op_write32(off, value);
}

/* Poll the event ring buffer for completion TRBs. This is a naive parser that
 * inspects the first few dwords of the ER buffer for any non-zero entry and
 * treats that as a completion. Returns 0 if a completion was observed, -1 on timeout.
 */
int xhci_poll_event_ring(int timeout_ms, void *data_v, void *user_buf, int data_len, int direction_in){
    if(!er_buffer) return -1;

    volatile uint32_t *buf = (volatile uint32_t*)er_buffer;
    const int max_trbs = 4096 / 16;
    int loops = timeout_ms * 1000 / 10; /* 10us sleep granularity */

    for(int iter=0; iter<loops; ++iter){
        /* Check the dequeue position for a valid TRB with matching cycle bit */
        uint32_t idx = er_dequeue_idx;
        uint32_t dw3 = buf[idx*4 + 3];

        /* TRB empty if type/flags word is zero OR cycle bit doesn't match expected */
        uint32_t trb_cycle = dw3 & 0x1u;
        if(dw3 != 0 && trb_cycle == (er_expected_cycle & 0x1u)){
            uint32_t dw2 = buf[idx*4 + 2];
            uint32_t dw1 = buf[idx*4 + 1];
            uint32_t dw0 = buf[idx*4 + 0];
            uint32_t trb_type = (dw3 >> 10) & 0x3f;
            serial_puts("usb/xhci: ER TRB[deq="); serial_putdec((uint32_t)idx);
            serial_puts("] dw3=0x"); serial_puthex32(dw3); serial_puts(" type="); serial_putdec((uint32_t)trb_type);
            serial_puts(" dw2=0x"); serial_puthex32(dw2); serial_puts(" dw1=0x"); serial_puthex32(dw1);
            serial_puts(" dw0=0x"); serial_puthex32(dw0); serial_puts("\n");

            /* For IN transfers, copy payload from DMA buffer into user buffer using length in dw2 (low 16 bits) */
            if(data_v && user_buf && data_len>0 && direction_in){
                uint32_t actual = dw2 & 0xFFFFu;
                if(actual==0) actual = (uint32_t)data_len;
                if(actual > (uint32_t)data_len) actual = (uint32_t)data_len;
                uint8_t *src = (uint8_t*)data_v;
                uint8_t *dst = (uint8_t*)user_buf;
                for(uint32_t z=0; z<actual; ++z) dst[z] = src[z];
                serial_puts("usb/xhci: copied "); serial_putdec((uint32_t)actual); serial_puts(" bytes from DMA buffer to user buffer\n");
            }

            /* Consume this TRB: zero it so we don't reprocess, advance dequeue index */
            for(int k=0;k<4;k++) ((uint32_t*)er_buffer)[idx*4 + k] = 0;

            er_dequeue_idx++;
            if(er_dequeue_idx >= (uint32_t)max_trbs){
                er_dequeue_idx = 0;
                /* wrap: toggle expected cycle bit */
                er_expected_cycle ^= 1u;
            }

            /* Update ERDP to let controller know we've dequeued. Use same offsets as before. */
            uint64_t erdp = er_buffer_phys + ((uint64_t)er_dequeue_idx * 16ULL);
            uint32_t erdp_low = (uint32_t)(erdp & 0xFFFFFFFFu) | (er_expected_cycle & 0x1u);
            xhci_op_write32(0x30, erdp_low);
            xhci_op_write32(0x34, (uint32_t)(((uint64_t)erdp>>32)&0xFFFFFFFFu));

            return 0;
        }

        /* no valid TRB at dequeue; small delay */
        for(volatile int z=0;z<1000;z++);
    }

    return -1;
}

/* Wait for a Command Completion Event. This is a heuristic implementation:
 * - scans the event ring for an event TRB whose type matches the common
 *   Command Completion Event value (0x21 / 33). Many controllers use that
 *   value; if your controller differs this may need adjustment.
 * - extracts a completion code heuristically from dw2 >> 24 (high byte).
 * - returns 0 and stores completion code in *out_code on success, -1 on timeout.
 */
int xhci_wait_for_command_completion(int timeout_ms, int *out_code){
    if(!er_buffer) return -1;
    volatile uint32_t *buf = (volatile uint32_t*)er_buffer;
    const int max_trbs = 4096 / 16;
    int loops = timeout_ms * 1000 / 10;

    for(int iter=0; iter<loops; ++iter){
        uint32_t idx = er_dequeue_idx;
        uint32_t dw3 = buf[idx*4 + 3];
        uint32_t trb_cycle = dw3 & 0x1u;
        if(dw3 != 0 && trb_cycle == (er_expected_cycle & 0x1u)){
            uint32_t dw2 = buf[idx*4 + 2];
            uint32_t dw1 = buf[idx*4 + 1];
            uint32_t dw0 = buf[idx*4 + 0];
            uint32_t trb_type = (dw3 >> 10) & 0x3f;
            /* 0x21 is the common Command Completion Event type */
            if(trb_type == 0x21u || trb_type == 0x20u){
                uint32_t comp_code = (dw2 >> 24) & 0xFFu; /* heuristic */
                if(out_code) *out_code = (int)comp_code;

                /* Attempt to correlate the completion to a command TRB by
                 * interpreting DW0/DW1 as a 64-bit pointer to the command TRB
                 * (some controllers include that). If so, find matching index.
                 */
                uint64_t cmd_ptr = ((uint64_t)dw1 << 32) | (uint64_t)dw0;
                int matched = -1;
                if(cmd_ptr != 0 && cmd_ring_phys!=0){
                    /* If cmd_ptr lies within cmd_ring_phys .. cmd_ring_phys + size, compute index */
                    uint64_t start = (uint64_t)cmd_ring_phys;
                    uint64_t end = start + (uint64_t)(XHCI_CMD_RING_TRBS * sizeof(struct xhci_trb));
                    if(cmd_ptr >= start && cmd_ptr < end){
                        uint64_t off = cmd_ptr - start;
                        matched = (int)(off / sizeof(struct xhci_trb));
                    }
                }
                if(matched>=0){
                    serial_puts("usb/xhci: command completion matched cmd_ring index="); serial_putdec((uint32_t)matched);
                    serial_puts(" code="); serial_putdec((uint32_t)comp_code); serial_puts("\n");
                    if(matched < XHCI_CMD_RING_TRBS) cmd_ring_pending[matched]=0;
                } else {
                    /* try token-based match: many controllers won't include the TRB
                     * pointer but may echo back dw0 contents we placed earlier. We
                     * search cmd_ring_token for a match. */
                    uint32_t token_guess = (uint32_t)dw0;
                    int found_by_token = -1;
                    if(token_guess!=0){
                        for(int ti=0; ti<XHCI_CMD_RING_TRBS; ++ti){
                            if(cmd_ring_token[ti] == token_guess){ found_by_token = ti; break; }
                        }
                    }
                    if(found_by_token>=0){
                        serial_puts("usb/xhci: command completion matched by token idx="); serial_putdec((uint32_t)found_by_token);
                        serial_puts(" token=0x"); serial_puthex32((uint32_t)token_guess);
                        serial_puts(" code="); serial_putdec((uint32_t)comp_code); serial_puts("\n");
                        cmd_ring_pending[found_by_token]=0;
                        /* clear token */
                        cmd_ring_token[found_by_token]=0;
                    } else {
                        serial_puts("usb/xhci: command completion (no-match) code="); serial_putdec((uint32_t)comp_code);
                        serial_puts(" dw0=0x"); serial_puthex32((uint32_t)dw0); serial_puts(" dw1=0x"); serial_puthex32((uint32_t)dw1);
                        serial_puts(" token_guess=0x"); serial_puthex32((uint32_t)token_guess); serial_puts("\n");
                    }
                }

                /* consume TRB and advance dequeue */
                for(int k=0;k<4;k++) ((uint32_t*)er_buffer)[idx*4 + k] = 0;
                er_dequeue_idx++;
                if(er_dequeue_idx >= (uint32_t)max_trbs){
                    er_dequeue_idx = 0;
                    er_expected_cycle ^= 1u;
                }
                uint64_t erdp = er_buffer_phys + ((uint64_t)er_dequeue_idx * 16ULL);
                uint32_t erdp_low = (uint32_t)(erdp & 0xFFFFFFFFu) | (er_expected_cycle & 0x1u);
                xhci_op_write32(0x30, erdp_low);
                xhci_op_write32(0x34, (uint32_t)(((uint64_t)erdp>>32)&0xFFFFFFFFu));
                return 0;
            }
            /* Not a command completion, consume as before (transfer event etc.) */
            for(int k=0;k<4;k++) ((uint32_t*)er_buffer)[idx*4 + k] = 0;
            er_dequeue_idx++;
            if(er_dequeue_idx >= (uint32_t)max_trbs){
                er_dequeue_idx = 0;
                er_expected_cycle ^= 1u;
            }
            uint64_t erdp2 = er_buffer_phys + ((uint64_t)er_dequeue_idx * 16ULL);
            uint32_t erdp2_low = (uint32_t)(erdp2 & 0xFFFFFFFFu) | (er_expected_cycle & 0x1u);
            xhci_op_write32(0x30, erdp2_low);
            xhci_op_write32(0x34, (uint32_t)(((uint64_t)erdp2>>32)&0xFFFFFFFFu));
        }
        for(volatile int z=0;z<1000;z++);
    }
    return -1;
}

int xhci_probe(void){
    /* scan PCI for class/subclass match (simple brute-force as in pci.c) */
    for(uint8_t bus=0; bus<256; ++bus){
        for(uint8_t slot=0; slot<32; ++slot){
            for(uint8_t func=0; func<8; ++func){
                uint16_t ven = pci_config_read16(bus,slot,func,0x00);
                if(ven==0xFFFF) continue;
                uint8_t class = pci_config_read8(bus,slot,func,0x0B);
                uint8_t subclass = pci_config_read8(bus,slot,func,0x0A);
                if(class==XHCI_CLASS && subclass==XHCI_SUBCLASS){
                    printf("usb/xhci: found host controller at %u:%u.%u vendor=0x%04x\n", bus,slot,func, ven);
                    xhci.found = 1;
                    xhci.bus = bus; xhci.slot = slot; xhci.func = func;

                    /* Dump PCI config space to help diagnose BARs/CAP length issues */
                    {
                        int off;
                        serial_puts("usb/xhci: dumping PCI config header for ");
                        char tbuf[16];
                        tbuf[0] = '0' + (bus/10)%10; tbuf[1] = '0' + (bus%10); tbuf[2]=':';
                        tbuf[3] = '0' + (slot/10)%10; tbuf[4] = '0' + (slot%10); tbuf[5]='.';
                        tbuf[6] = '0' + (func%10); tbuf[7]=0;
                        serial_puts(tbuf);
                        serial_puts("\n");
                        for (off = 0; off < 64; off += 4) {
                            uint32_t val = pci_config_read32(bus, slot, func, off);
                            serial_puts("usb/pci: cfg[");
                            serial_puthex8((uint8_t)off);
                            serial_puts("]=0x");
                            serial_puthex32(val);
                            serial_puts("\n");
                        }
                    }

                    /* enable mem + bus master */
                    uint32_t cmd = pci_config_read32(bus,slot,func,0x04);
                    cmd |= 0x0006; /* Memory + Bus Master */
                    pci_config_write32(bus,slot,func,0x04,cmd);
                    /* read back BAR0 and command after program to help debugging */
                    {
                        uint32_t new_cmd = pci_config_read32(bus,slot,func,0x04);
                        uint32_t new_bar0 = pci_config_read32(bus,slot,func,0x10);
                        serial_puts("usb/xhci: after enable CMD="); serial_puthex32(new_cmd); serial_puts(" BAR0="); serial_puthex32(new_bar0); serial_puts("\n");
                    }

                    uint32_t bar0 = pci_config_read32(bus,slot,func,0x10);
                    /* Detect IO vs Memory BAR: bit0==1 => IO BAR per PCI spec */
                    int is_io = (bar0 & 0x1u) ? 1 : 0;
                    uintptr_t base;
                    if(is_io) base = (uintptr_t)(bar0 & ~0x3u);
                    else base = (uintptr_t)(bar0 & ~0xFULL);
                    xhci.phys_base = base;
                    xhci.cap_is_io = is_io;
                    printf("usb/xhci: PCI BAR0=0x%08x base=0x%08lx %s\n", bar0, (unsigned long)base, is_io?"(IO)":"(MMIO)");
                    serial_puts("usb/xhci: PCI BAR0=0x"); serial_puthex32(bar0);
                    serial_puts(" base=0x"); serial_puthex32((uint32_t)base);
                    serial_puts(is_io?" (IO)\n":" (MMIO)\n");
                    /* For simple kernel we assume identity mapping for MMIO; for IO BARs we'll use port IO helpers. */
                    xhci.cap_base = (volatile uint8_t*)(uintptr_t)base;

                    /* Capability registers: CAPLENGTH at offset 0 */
                    uint8_t caplen = *((volatile uint8_t*)(xhci.cap_base + 0x00));
                    xhci.caplen = caplen;
                    printf("usb/xhci: CAPLENGTH read=0x%02x\n", (unsigned)caplen);
                    serial_puts("usb/xhci: CAPLENGTH read=0x"); serial_puthex8(caplen); serial_puts("\n");
                    xhci.op_base = xhci.cap_base + caplen;

                    uint32_t hcsparams1 = xhci_cap_read32(0x04);
                                    /* parse some useful fields from HCSPARAMS1
                                     * - Bits [31:24] typically contain number of ports on the controller (per xHCI spec)
                                     * - Other fields present but not parsed here
                                     */
                                    xhci.num_ports = (uint8_t)((hcsparams1 >> 24) & 0xff);
                                    printf("usb/xhci: CAPLENGTH=%u OP_BASE=0x%p HCSPARAMS1=0x%08x NUM_PORTS=%u\n",
                                        (unsigned)caplen, (void*)xhci.op_base, hcsparams1, (unsigned)xhci.num_ports);
                                    serial_puts("usb/xhci: CAPLENGTH="); serial_puthex8(caplen);
                                    serial_puts(" OP_BASE=0x"); serial_puthex32((uint32_t)(uintptr_t)xhci.op_base);
                                    serial_puts(" HCSPARAMS1=0x"); serial_puthex32(hcsparams1);
                                    serial_puts(" NUM_PORTS="); serial_putdec((uint32_t)xhci.num_ports); serial_puts("\n");

                    /* Small sanity read of an operational register (USBCMD offset 0x00)
                     * Note: real driver must follow xHCI spec for enabling the controller.
                     */
                    uint32_t usbcmd = xhci_op_read32(0x00);
                    printf("usb/xhci: USBCMD=0x%08x\n", usbcmd);
                    serial_puts("usb/xhci: USBCMD=0x"); serial_puthex32(usbcmd); serial_puts("\n");

                    return 0;
                }
                uint8_t ht = pci_config_read8(bus,slot,0,0x0E);
                if((ht & 0x80)==0 && func==0) break;
            }
        }
    }
    printf("usb/xhci: no controller found\n");
    return -1;
}

/* Return number of ports discovered from HCSPARAMS1 (or 0 if unknown) */
int xhci_num_ports(void){
    return xhci.num_ports;
}

/* Read PORTSC for a 1-based port index. Assumes OP regs are mapped and ports start at offset 0x400.
 * Note: this uses a common xHCI layout where port registers begin at op_base + 0x400.
 * This is an assumption used as a safe, common-case heuristic for initial probing.
 */
uint32_t xhci_read_portsc(int port){
    if(port<=0 || xhci.op_base==NULL) return 0;
    uint32_t off = 0x400 + (uint32_t)(port-1)*4;
    return xhci_op_read32(off);
}

/* Dump port status registers for debugging */
void xhci_dump_ports(void){
    int n = xhci_num_ports();
    if(n<=0){
        printf("usb/xhci: no port information available\n");
        return;
    }
    for(int i=1;i<=n;i++){
        uint32_t p = xhci_read_portsc(i);
        printf("usb/xhci: PORT %d: PORTSC=0x%08x\n", i, p);
    }
}
