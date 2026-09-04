/* ============================================================================
 *  T I T A N   K E R N E L   v3.0  --  "Glass Edition"
 *  A single-file, from-scratch, protected-mode x86 kernel with its own GUI.
 *
 *  What changed from v2 -> v3:
 *    - PERFORMANCE FIX: v2 drew every shape directly to video memory, one
 *      pixel at a time, through a bounds-checked function call. On a
 *      1920x1080 framebuffer that is ~1,000,000 checked function calls per
 *      full redraw, and a redraw fired on *every single mouse packet* --
 *      that's what made the cursor feel like it was sticking/lagging.
 *      v3 draws into an off-screen backbuffer using tight `rep stosl`
 *      (fill) and `rep movsl` (blit) loops, and only presents the finished
 *      frame to video memory once per redraw. This is a completely
 *      different order of magnitude of speed, and also removes the
 *      flicker/tearing that made the UI feel rough.
 *    - VISUAL REFRESH: rounded-corner translucent ("glass") windows that
 *      genuinely blend with the desktop gradient behind them, soft layered
 *      drop shadows, a glowing accent taskbar, hover/press glow on buttons,
 *      a blinking text cursor in the terminal, and a smooth vertical
 *      gradient desktop instead of flat color bands.
 *
 *  Still true: this is one real file, it really boots, it really draws its
 *  own GUI, and it is still a hobby-scale kernel (no paging, no fs, no
 *  multitasking). I'm not going to call it "bug-free" or "the best kernel
 *  ever" -- that'd be marketing, not engineering. What's here is real,
 *  tested in QEMU, and now meaningfully faster and nicer to look at.
 *
 *  Build: see build.sh in the same folder.
 * ==========================================================================*/

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                bool_t;
#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#include "timeanddate.h"
#include "lockscreen.h"
#include "ossystemsafety.h"
#include "persist_storage.h"
#include "calculator.h"
#include "paint.h"
#include "cliphist.h"
#include "applauncher.h"
#include "toast.h"
#include "taskmgr.h"
#include "clockview.h"
#include "network/net.h"

/* ============================================================================
 *  MULTIBOOT 1 HEADER -- request a linear framebuffer graphics mode
 * ==========================================================================*/
#define MB_MAGIC   0x1BADB002
#define MB_FLAGS   0x00000007
#define MB_CHECKSUM (-(MB_MAGIC + MB_FLAGS))
#define REQ_WIDTH  1024
#define REQ_HEIGHT 768
#define REQ_DEPTH  32

__attribute__((section(".multiboot"), used, aligned(4)))
static const u32 multiboot_header[7] = {
    MB_MAGIC, MB_FLAGS, MB_CHECKSUM, 0, REQ_WIDTH, REQ_HEIGHT, REQ_DEPTH
};

struct multiboot_info {
    u32 flags;
    u32 mem_lower, mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count, mods_addr;
    u32 syms[4];
    u32 mmap_length, mmap_addr;
    u32 drives_length, drives_addr;
    u32 config_table;
    u32 boot_loader_name;
    u32 apm_table;
    u32 vbe_control_info, vbe_mode_info;
    u16 vbe_mode;
    u16 vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8  framebuffer_bpp;
    u8  framebuffer_type;
    u8  color_info[6];
} __attribute__((packed));

u32 mb_magic;
u32 mb_info_ptr __attribute__((used));

/* ============================================================================
 *  PORT I/O
 * ==========================================================================*/
static inline void outb(u16 port, u8 val) { __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port)); }
static inline void outw(u16 port, u16 val) { __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline void io_wait(void) { outb(0x80, 0); }

/* ============================================================================
 *  STRING / MEM helpers
 * ==========================================================================*/
static u32 k_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void k_memset(void *dst, u8 val, u32 len) { u8 *d = dst; for (u32 i=0;i<len;i++) d[i]=val; }
static bool_t k_strcmp(const char *a, const char *b) { while (*a && *b) { if (*a!=*b) return FALSE; a++; b++; } return *a==*b; }
static bool_t k_strncmp_prefix(const char *s, const char *p) { while (*p) { if (*s!=*p) return FALSE; s++; p++; } return TRUE; }

/* ============================================================================
 *  SERIAL (COM1) -- debug/verification channel, mirrors terminal output
 * ==========================================================================*/
#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1+1,0x00); outb(COM1+3,0x80); outb(COM1+0,0x03); outb(COM1+1,0x00);
    outb(COM1+3,0x03); outb(COM1+2,0xC7); outb(COM1+4,0x0B);
}
static void serial_putc(char c) { while ((inb(COM1+5)&0x20)==0){} outb(COM1,(u8)c); }

typedef void (*putc_fn)(char);
static void gp_uint(putc_fn pc, u32 val, u32 base, bool_t upper) {
    char buf[32]; int i=0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (val==0) { pc('0'); return; }
    while (val>0) { buf[i++] = digits[val%base]; val/=base; }
    while (i>0) pc(buf[--i]);
}
static void gp_int(putc_fn pc, int val) {
    if (val<0) { pc('-'); gp_uint(pc,(u32)(-val),10,FALSE); } else gp_uint(pc,(u32)val,10,FALSE);
}
static void generic_vprintf(putc_fn pc, const char *fmt, __builtin_va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { pc(*p); continue; }
        p++;
        switch (*p) {
            case 's': { const char *s = __builtin_va_arg(args, const char*); while (*s) pc(*s++); break; }
            case 'd': gp_int(pc, __builtin_va_arg(args, int)); break;
            case 'u': gp_uint(pc, __builtin_va_arg(args, u32), 10, FALSE); break;
            case 'x': pc('0'); pc('x'); gp_uint(pc, __builtin_va_arg(args, u32), 16, FALSE); break;
            case 'c': pc((char)__builtin_va_arg(args, int)); break;
            case '%': pc('%'); break;
            default:  pc('%'); pc(*p); break;
        }
    }
}
static void debug_printf(const char *fmt, ...) {
    __builtin_va_list a; __builtin_va_start(a, fmt);
    generic_vprintf(serial_putc, fmt, a);
    __builtin_va_end(a);
}

/* ============================================================================
 *  ORIGINAL 5x7 BITMAP FONT (hand-designed for Titan)
 * ==========================================================================*/
struct font_glyph { char ch; u8 rows[7]; };
static const struct font_glyph font5x7[] = {
    {' ', {0,0,0,0,0,0,0}},       {'!', {4,4,4,4,4,0,4}},
    {'"', {10,10,0,0,0,0,0}},     {'#', {10,31,10,10,31,10,0}},
    {'%', {17,2,4,4,8,17,0}},     {'&', {12,18,20,8,21,18,13}},
    {'\'', {4,4,0,0,0,0,0}},      {'(', {2,4,8,8,8,4,2}},
    {')', {8,4,2,2,2,4,8}},       {'*', {0,21,14,31,14,21,0}},
    {'+', {0,4,4,31,4,4,0}},      {',', {0,0,0,0,4,4,8}},
    {'-', {0,0,0,31,0,0,0}},      {'.', {0,0,0,0,0,4,4}},
    {'/', {1,2,4,4,8,16,0}},      {'0', {14,17,19,21,25,17,14}},
    {'1', {4,12,4,4,4,4,31}},     {'2', {14,17,1,2,4,8,31}},
    {'3', {30,1,2,12,1,1,30}},    {'4', {2,6,10,18,31,2,2}},
    {'5', {31,16,30,1,1,17,14}},  {'6', {6,8,16,30,17,17,14}},
    {'7', {31,1,2,4,4,4,4}},      {'8', {14,17,17,14,17,17,14}},
    {'9', {14,17,17,15,1,2,12}},  {':', {0,4,0,0,4,0,0}},
    {';', {0,4,0,0,4,4,8}},       {'<', {1,2,4,8,4,2,1}},
    {'=', {0,0,31,0,31,0,0}},     {'>', {16,8,4,2,4,8,16}},
    {'?', {14,17,1,2,4,0,4}},     {'@', {14,17,22,23,16,0,14}},
    {'A', {4,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}},
    {'C', {15,16,16,16,16,16,15}},{'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}},{'F', {31,16,16,30,16,16,16}},
    {'G', {15,16,16,23,17,17,15}},{'H', {17,17,17,31,17,17,17}},
    {'I', {31,4,4,4,4,4,31}},     {'J', {7,2,2,2,2,18,12}},
    {'K', {17,18,20,24,20,18,17}},{'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,17,17,17,17}},{'N', {17,25,21,21,19,17,17}},
    {'O', {14,17,17,17,17,17,14}},{'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}},{'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}},  {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}},{'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,27,17}},{'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}},    {'Z', {31,1,2,4,8,16,31}},
    {'\\', {16,8,4,4,2,1,0}},     {'_', {0,0,0,0,0,0,31}},
    {'|', {4,4,4,4,4,4,4}},       {'~', {0,0,8,21,2,0,0}},
    /* lowercase — slightly smaller / distinct shapes */
    {'a', {0,0,14,1,15,17,15}},   {'b', {16,16,30,17,17,17,30}},
    {'c', {0,0,15,16,16,16,15}},  {'d', {1,1,15,17,17,17,15}},
    {'e', {0,0,14,17,31,16,14}},  {'f', {6,8,8,28,8,8,8}},
    {'g', {0,0,15,17,17,15,1}},   {'h', {16,16,30,17,17,17,17}},
    {'i', {4,0,12,4,4,4,14}},     {'j', {2,0,6,2,2,2,12}},
    {'k', {16,16,18,20,24,20,18}},{'l', {12,4,4,4,4,4,14}},
    {'m', {0,0,26,21,21,21,21}},  {'n', {0,0,30,17,17,17,17}},
    {'o', {0,0,14,17,17,17,14}},  {'p', {0,0,30,17,17,30,16}},
    {'q', {0,0,15,17,17,15,1}},   {'r', {0,0,22,24,16,16,16}},
    {'s', {0,0,15,16,14,1,30}},   {'t', {8,8,28,8,8,8,6}},
    {'u', {0,0,17,17,17,17,15}},  {'v', {0,0,17,17,17,10,4}},
    {'w', {0,0,17,17,21,21,10}},  {'x', {0,0,17,10,4,10,17}},
    {'y', {0,0,17,17,17,15,1}},   {'z', {0,0,31,2,4,8,31}},
};
#define FONT_GLYPH_COUNT (sizeof(font5x7)/sizeof(font5x7[0]))
static const u8 *font_lookup(char c) {
    for (u32 i = 0; i < FONT_GLYPH_COUNT; i++) if (font5x7[i].ch == c) return font5x7[i].rows;
    /* fallback: if lowercase missing, try uppercase */
    if (c >= 'a' && c <= 'z') {
        char up = (char)(c - 'a' + 'A');
        for (u32 i = 0; i < FONT_GLYPH_COUNT; i++) if (font5x7[i].ch == up) return font5x7[i].rows;
    }
    return font5x7[0].rows;
}

/* ============================================================================
 *  GRAPHICS ENGINE -- off-screen backbuffer + fast fill/blit + alpha blend
 * ==========================================================================*/
static u8  *fb_addr;
static u32  fb_pitch, fb_width, fb_height;
static u8   fb_bpp;
static bool_t fb_ready = FALSE;
static u32 *backbuffer = NULL; /* one u32 per pixel, tightly packed (no pitch gaps) */

#define RGB(r,g,b) (((u32)(r)<<16) | ((u32)(g)<<8) | (u32)(b))
#define RGB_R(c) (((c)>>16)&0xFF)
#define RGB_G(c) (((c)>>8)&0xFF)
#define RGB_B(c) ((c)&0xFF)

/* ---- Modern "glass" theme palette ---- */
#define COL_SKY_TOP     RGB(20,22,46)
#define COL_SKY_BOT     RGB(38,74,110)
#define COL_ACCENT      RGB(90,200,255)
#define COL_ACCENT2     RGB(170,110,255)
#define COL_GLASS_TINT  RGB(18,22,34)
#define COL_GLASS_LIGHT RGB(235,240,250)
#define COL_TASKBAR     RGB(12,14,24)
#define COL_TASKTEXT    RGB(225,230,240)
#define COL_TITLETEXT   RGB(240,244,250)
#define COL_BODYTEXT    RGB(60,64,74)
#define COL_TERMBG      RGB(8,12,16)
#define COL_TERMTEXT    RGB(120,255,170)
#define COL_TERMDIM     RGB(70,150,110)
#define COL_CLOSE       RGB(235,90,90)
#define COL_CLOSEHOV    RGB(255,120,120)
#define COL_BLACK       RGB(0,0,0)
#define COL_WHITE       RGB(255,255,255)
#define COL_BTN         RGB(40,46,64)
#define COL_BTNHOV       RGB(60,110,150)

/* --- low-level pixel ops on the backbuffer (bounds-checked once) --- */
static inline void bb_set(int x, int y, u32 color) {
    if ((u32)x >= fb_width || (u32)y >= fb_height) return;
    backbuffer[(u32)y * fb_width + (u32)x] = color;
}
static inline u32 bb_get(int x, int y) {
    if ((u32)x >= fb_width || (u32)y >= fb_height) return 0;
    return backbuffer[(u32)y * fb_width + (u32)x];
}
static inline void bb_blend(int x, int y, u32 color, u8 alpha) {
    if ((u32)x >= fb_width || (u32)y >= fb_height) return;
    u32 *p = &backbuffer[(u32)y * fb_width + (u32)x];
    u32 bg = *p;
    u8 rr = (u8)(((u32)RGB_R(color)*alpha + (u32)RGB_R(bg)*(255-alpha)) / 255);
    u8 rg = (u8)(((u32)RGB_G(color)*alpha + (u32)RGB_G(bg)*(255-alpha)) / 255);
    u8 rb = (u8)(((u32)RGB_B(color)*alpha + (u32)RGB_B(bg)*(255-alpha)) / 255);
    *p = RGB(rr,rg,rb);
}

/* fast solid rectangle fill using rep stosl (one asm loop per row) */
static void bb_fill_rect(int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_width)  w = (int)fb_width  - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *dst = backbuffer + (u32)(y+j) * fb_width + (u32)x;
        int count = w;
        __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(count) : "a"(color) : "memory");
    }
}

static void bb_hline(int x, int y, int w, u32 color) { bb_fill_rect(x, y, w, 1, color); }
static void bb_vline(int x, int y, int h, u32 color) { bb_fill_rect(x, y, 1, h, color); }

/* rounded-corner translucent rectangle -- the core of the "glass" look */
static void bb_fill_rounded_glass(int x, int y, int w, int h, int radius, u32 color, u8 alpha) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            bool_t inside = TRUE;
            int cx = -1, cy = -1;
            if (i < radius && j < radius)                     { cx = radius;       cy = radius; }
            else if (i >= w - radius && j < radius)            { cx = w - radius-1; cy = radius; }
            else if (i < radius && j >= h - radius)            { cx = radius;       cy = h - radius-1; }
            else if (i >= w - radius && j >= h - radius)       { cx = w - radius-1; cy = h - radius-1; }
            if (cx >= 0) {
                int dx = i - cx, dy = j - cy;
                if (dx*dx + dy*dy > radius*radius) inside = FALSE;
            }
            if (inside) bb_blend(x+i, y+j, color, alpha);
        }
    }
}

static void bb_rounded_border(int x, int y, int w, int h, int radius, u32 color) {
    for (int i = radius; i < w-radius; i++) { bb_set(x+i,y,color); bb_set(x+i,y+h-1,color); }
    for (int j = radius; j < h-radius; j++) { bb_set(x,y+j,color); bb_set(x+w-1,y+j,color); }
    /* approximate the four corner arcs by sampling a quarter circle */
    for (int t = 0; t <= radius; t++) {
        int dx = radius - t;
        int dyv = radius*radius - dx*dx;
        if (dyv < 0) continue;
        int dy = 0;
        while (dy*dy <= dyv) dy++;
        dy--;
        bb_set(x + radius - dx, y + radius - dy, color);
        bb_set(x + w - radius - 1 + dx, y + radius - dy, color);
        bb_set(x + radius - dx, y + h - radius - 1 + dy, color);
        bb_set(x + w - radius - 1 + dx, y + h - radius - 1 + dy, color);
    }
}

#define FONT_SCALE 2
#define CHAR_W (6*FONT_SCALE)
#define CHAR_H (8*FONT_SCALE)

static void bb_draw_char(int x, int y, char c, u32 fg) {
    const u8 *rows = font_lookup(c);
    for (int r = 0; r < 7; r++) {
        u8 bits = rows[r];
        if (!bits) continue;
        for (int col = 0; col < 5; col++)
            if (bits & (1 << (4 - col)))
                bb_fill_rect(x + col*FONT_SCALE, y + r*FONT_SCALE, FONT_SCALE, FONT_SCALE, fg);
    }
}
static void bb_draw_string(int x, int y, const char *s, u32 fg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += CHAR_H; s++; continue; }
        bb_draw_char(cx, y, *s, fg);
        cx += CHAR_W; s++;
    }
}
static char g_fmtbuf[512];
static u32 g_fmtpos;
static void fmtbuf_putc(char c) { if (g_fmtpos < sizeof(g_fmtbuf)-1) g_fmtbuf[g_fmtpos++] = c; }
static void bb_printf(int x, int y, u32 fg, const char *fmt, ...) {
    g_fmtpos = 0;
    __builtin_va_list a; __builtin_va_start(a, fmt);
    generic_vprintf(fmtbuf_putc, fmt, a);
    __builtin_va_end(a);
    g_fmtbuf[g_fmtpos] = '\0';
    bb_draw_string(x, y, g_fmtbuf, fg);
}

/* present: fast blit of the finished backbuffer to real video memory */
static void fb_present(void) {
    for (u32 y = 0; y < fb_height; y++) {
        u8  *dst = fb_addr + (u32)y * fb_pitch;
        u32 *src = backbuffer + (u32)y * fb_width;
        int count = (int)fb_width;
        __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
    }
}

/* ============================================================================
 *  GDT
 * ==========================================================================*/
struct gdt_entry { u16 limit_low; u16 base_low; u8 base_mid; u8 access; u8 gran; u8 base_high; } __attribute__((packed));
struct gdt_ptr { u16 limit; u32 base; } __attribute__((packed));
static struct gdt_entry gdt[5];
static struct gdt_ptr gdtp;
static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[i].base_low=base&0xFFFF; gdt[i].base_mid=(base>>16)&0xFF; gdt[i].base_high=(base>>24)&0xFF;
    gdt[i].limit_low=limit&0xFFFF; gdt[i].gran=((limit>>16)&0x0F)|(gran&0xF0); gdt[i].access=access;
}
extern void gdt_flush(u32);
static void gdt_init(void) {
    gdtp.limit = sizeof(gdt)-1; gdtp.base = (u32)&gdt;
    gdt_set(0,0,0,0,0);
    gdt_set(1,0,0xFFFFFFFF,0x9A,0xCF);
    gdt_set(2,0,0xFFFFFFFF,0x92,0xCF);
    gdt_set(3,0,0xFFFFFFFF,0xFA,0xCF);
    gdt_set(4,0,0xFFFFFFFF,0xF2,0xCF);
    gdt_flush((u32)&gdtp);
}
__asm__ (
    ".global gdt_flush\n gdt_flush:\n"
    "    mov 4(%esp), %eax\n    lgdt (%eax)\n"
    "    mov $0x10, %ax\n    mov %ax, %ds\n    mov %ax, %es\n"
    "    mov %ax, %fs\n    mov %ax, %gs\n    mov %ax, %ss\n"
    "    ljmp $0x08, $gdt_flush_cs\n"
    "gdt_flush_cs:\n    ret\n"
);

/* ============================================================================
 *  IDT + ISR/IRQ stubs
 * ==========================================================================*/
struct idt_entry { u16 base_low; u16 sel; u8 always0; u8 flags; u16 base_high; } __attribute__((packed));
struct idt_ptr { u16 limit; u32 base; } __attribute__((packed));
static struct idt_entry idt[256];
static struct idt_ptr idtp;
extern void idt_flush(u32);
__asm__ (".global idt_flush\n idt_flush:\n    mov 4(%esp), %eax\n    lidt (%eax)\n    ret\n");
static void idt_set(int i, u32 base, u16 sel, u8 flags) {
    idt[i].base_low=base&0xFFFF; idt[i].base_high=(base>>16)&0xFFFF;
    idt[i].sel=sel; idt[i].always0=0; idt[i].flags=flags;
}
#define ISR_LIST(X) X(0)X(1)X(2)X(3)X(4)X(5)X(6)X(7)X(8)X(9)X(10)X(11)X(12)X(13)X(14)X(15)X(16)X(17)X(18)X(19)X(20)X(21)X(22)X(23)X(24)X(25)X(26)X(27)X(28)X(29)X(30)X(31)
#define IRQ_LIST(X) X(32)X(33)X(34)X(35)X(36)X(37)X(38)X(39)X(40)X(41)X(42)X(43)X(44)X(45)X(46)X(47)
#define DECLARE_EXTERN(n) extern void isr##n(void);
ISR_LIST(DECLARE_EXTERN) IRQ_LIST(DECLARE_EXTERN)
#undef DECLARE_EXTERN

struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
};
static const char *exception_names[32] = {
    "Divide-by-zero","Debug","NMI","Breakpoint","Overflow","Bound Range Exceeded",
    "Invalid Opcode","Device Not Available","Double Fault","Coprocessor Segment Overrun",
    "Invalid TSS","Segment Not Present","Stack-Segment Fault","General Protection Fault",
    "Page Fault","Reserved","x87 Floating-Point","Alignment Check","Machine Check",
    "SIMD Floating-Point","Virtualization","Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved","Reserved","Security","Reserved"
};
static void panic(const char *msg) {
    __asm__ volatile ("cli");
    debug_printf("\n[PANIC] %s\n", msg);
    if (fb_ready && backbuffer) {
        bb_fill_rect(0, 0, (int)fb_width, (int)fb_height, RGB(120,15,15));
        bb_printf(40, 40, COL_WHITE, "*** TITAN KERNEL PANIC ***");
        bb_printf(40, 40 + CHAR_H*2, COL_WHITE, "%s", msg);
        bb_printf(40, 40 + CHAR_H*4, COL_WHITE, "System halted.");
        fb_present();
    }
    for (;;) { __asm__ volatile ("hlt"); }
}
void isr_handler(struct regs *r) {
    if (r->int_no < 32) {
        debug_printf("\n[EXCEPTION] %s (int=%u err=%x)\n", exception_names[r->int_no], r->int_no, r->err_code);
        panic("Unhandled CPU exception");
    }
}
#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1
static void pic_remap(void) {
    outb(PIC1,0x11); io_wait(); outb(PIC2,0x11); io_wait();
    outb(PIC1_DATA,0x20); io_wait(); outb(PIC2_DATA,0x28); io_wait();
    outb(PIC1_DATA,0x04); io_wait(); outb(PIC2_DATA,0x02); io_wait();
    outb(PIC1_DATA,0x01); io_wait(); outb(PIC2_DATA,0x01); io_wait();
    outb(PIC1_DATA,0x00); io_wait(); outb(PIC2_DATA,0x00); io_wait();
}
static void pic_send_eoi(u8 irq) { if (irq>=8) outb(PIC2,0x20); outb(PIC1,0x20); }
typedef void (*irq_handler_t)(struct regs*);
static irq_handler_t irq_handlers[16];
void irq_handler(struct regs *r) {
    u32 irq = r->int_no - 32;
    if (irq < 16 && irq_handlers[irq]) irq_handlers[irq](r);
    pic_send_eoi((u8)irq);
}
static void irq_install_handler(u32 irq, irq_handler_t h) { if (irq < 16) irq_handlers[irq] = h; }
static void idt_init(void) {
    idtp.limit = sizeof(idt)-1; idtp.base = (u32)&idt;
    k_memset(&idt, 0, sizeof(idt));
    #define SET_ISR(n) idt_set(n, (u32)isr##n, 0x08, 0x8E);
    ISR_LIST(SET_ISR) IRQ_LIST(SET_ISR)
    #undef SET_ISR
    idt_flush((u32)&idtp);
}
#define ISR_NOERR(n) ".global isr" #n "\nisr" #n ":\n    cli\n    push $0\n    push $" #n "\n    jmp isr_common_stub\n"
#define ISR_ERR(n)   ".global isr" #n "\nisr" #n ":\n    cli\n    push $" #n "\n    jmp isr_common_stub\n"
#define IRQ_STUB(n)  ".global isr" #n "\nisr" #n ":\n    cli\n    push $0\n    push $" #n "\n    jmp irq_common_stub\n"
__asm__ (
    ISR_NOERR(0) ISR_NOERR(1) ISR_NOERR(2) ISR_NOERR(3) ISR_NOERR(4) ISR_NOERR(5)
    ISR_NOERR(6) ISR_NOERR(7) ISR_ERR(8) ISR_NOERR(9) ISR_ERR(10) ISR_ERR(11)
    ISR_ERR(12) ISR_ERR(13) ISR_ERR(14) ISR_NOERR(15) ISR_NOERR(16) ISR_ERR(17)
    ISR_NOERR(18) ISR_NOERR(19) ISR_NOERR(20) ISR_NOERR(21) ISR_NOERR(22) ISR_NOERR(23)
    ISR_NOERR(24) ISR_NOERR(25) ISR_NOERR(26) ISR_NOERR(27) ISR_NOERR(28) ISR_NOERR(29)
    ISR_ERR(30) ISR_NOERR(31)
    IRQ_STUB(32) IRQ_STUB(33) IRQ_STUB(34) IRQ_STUB(35) IRQ_STUB(36) IRQ_STUB(37)
    IRQ_STUB(38) IRQ_STUB(39) IRQ_STUB(40) IRQ_STUB(41) IRQ_STUB(42) IRQ_STUB(43)
    IRQ_STUB(44) IRQ_STUB(45) IRQ_STUB(46) IRQ_STUB(47)
    "isr_common_stub:\n"
    "    pusha\n    push %ds\n    push %es\n    push %fs\n    push %gs\n"
    "    mov $0x10, %ax\n    mov %ax, %ds\n    mov %ax, %es\n    mov %ax, %fs\n    mov %ax, %gs\n"
    "    push %esp\n    call isr_handler\n    add $4, %esp\n"
    "    pop %gs\n    pop %fs\n    pop %es\n    pop %ds\n    popa\n    add $8, %esp\n    sti\n    iret\n"
    "irq_common_stub:\n"
    "    pusha\n    push %ds\n    push %es\n    push %fs\n    push %gs\n"
    "    mov $0x10, %ax\n    mov %ax, %ds\n    mov %ax, %es\n    mov %ax, %fs\n    mov %ax, %gs\n"
    "    push %esp\n    call irq_handler\n    add $4, %esp\n"
    "    pop %gs\n    pop %fs\n    pop %es\n    pop %ds\n    popa\n    add $8, %esp\n    sti\n    iret\n"
);

/* ============================================================================
 *  PIT timer
 * ==========================================================================*/
static volatile u32 timer_ticks = 0;
#define PIT_HZ 100
static void timer_callback(struct regs *r) { (void)r; timer_ticks++; }
static void timer_init(u32 freq) {
    irq_install_handler(0, timer_callback);
    u32 divisor = 1193180 / freq;
    outb(0x43,0x36); outb(0x40,(u8)(divisor&0xFF)); outb(0x40,(u8)((divisor>>8)&0xFF));
}

/* ============================================================================
 *  REAL-TIME CLOCK -- wall clock time/date shown Windows-taskbar style,
 *  implemented in timeanddate.c against the motherboard CMOS RTC chip.
 * ==========================================================================*/
static td_u8  g_rtc_hour, g_rtc_min, g_rtc_sec, g_rtc_day, g_rtc_month;
static td_u16 g_rtc_year;
static char   g_rtc_str[24];
static u32    g_rtc_last_tick = 0;
static bool_t g_rtc_have_reading = FALSE;

static void rtc_refresh_if_due(void) {
    /* CMOS reads block briefly on the "update in progress" flag, so only
       do this once per second of ticks instead of every redraw. */
    if (g_rtc_have_reading && (timer_ticks - g_rtc_last_tick) < PIT_HZ) return;
    g_rtc_last_tick = timer_ticks;
    g_rtc_have_reading = TRUE;
    rtc_read_datetime(&g_rtc_hour, &g_rtc_min, &g_rtc_sec, &g_rtc_day, &g_rtc_month, &g_rtc_year);
    format_datetime_windows(g_rtc_str, g_rtc_hour, g_rtc_min, g_rtc_sec, g_rtc_day, g_rtc_month, g_rtc_year);
}

/* ============================================================================
 *  KEYBOARD DRIVER (IRQ1)
 * ==========================================================================*/
#define KBD_BUF_SIZE 256
static char kbd_buf[KBD_BUF_SIZE];
static volatile u32 kbd_head=0, kbd_tail=0;
static bool_t shift_down = FALSE;
static bool_t ctrl_down = FALSE;
static bool_t alt_down = FALSE;
static volatile bool_t request_launcher = FALSE;
static volatile bool_t request_taskmgr = FALSE;
static const char scancode_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
};
static const char scancode_ascii_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
};
static void kbd_buf_push(char c) {
    u32 next = (kbd_head+1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return;
    kbd_buf[kbd_head] = c; kbd_head = next;
}
static bool_t kbd_buf_pop(char *out) {
    if (kbd_head == kbd_tail) return FALSE;
    *out = kbd_buf[kbd_tail]; kbd_tail = (kbd_tail+1) % KBD_BUF_SIZE;
    return TRUE;
}
static bool_t kbd_e0 = FALSE;
static void keyboard_callback(struct regs *r) {
    (void)r;
    u8 sc = inb(0x60);
    if (sc == 0xE0) { kbd_e0 = TRUE; return; }
    /* modifiers make / break */
    if (sc == 0x2A || sc == 0x36) { shift_down = TRUE; kbd_e0 = FALSE; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = FALSE; kbd_e0 = FALSE; return; }
    if (sc == 0x1D) { ctrl_down = TRUE; kbd_e0 = FALSE; return; }
    if (sc == 0x9D) { ctrl_down = FALSE; kbd_e0 = FALSE; return; }
    if (sc == 0x38) { alt_down = TRUE; kbd_e0 = FALSE; return; }
    if (sc == 0xB8) { alt_down = FALSE; kbd_e0 = FALSE; return; }

    /* Extended keys (arrows, home/end, pgup/pgdn, delete) */
    if (kbd_e0) {
        kbd_e0 = FALSE;
        if (sc & 0x80) return; /* break */
        if (sc == 0x48) { kbd_buf_push(0x01); return; } /* Up */
        if (sc == 0x50) { kbd_buf_push(0x02); return; } /* Down */
        if (sc == 0x4B) { kbd_buf_push(0x03); return; } /* Left */
        if (sc == 0x4D) { kbd_buf_push(0x07); return; } /* Right */
        if (sc == 0x47) { kbd_buf_push(0x0B); return; } /* Home */
        if (sc == 0x4F) { kbd_buf_push(0x0E); return; } /* End */
        if (sc == 0x49) { kbd_buf_push(0x0F); return; } /* PgUp */
        if (sc == 0x51) { kbd_buf_push(0x10); return; } /* PgDn */
        if (sc == 0x53) { kbd_buf_push(0x7F); return; } /* Delete */
        return;
    }

    if (sc & 0x80) return;
    /* Ctrl+Alt+A = App Launcher */
    if (ctrl_down && alt_down && sc == 0x1E) { request_launcher = TRUE; return; }
    /* Ctrl+Alt+T = Task Manager */
    if (ctrl_down && alt_down && sc == 0x14) { request_taskmgr = TRUE; return; }
    /* Alt+F4 = close focused window */
    if (alt_down && sc == 0x3E) {
        kbd_buf_push(0x04);
        return;
    }
    /* Alt+E = File Manager, Alt+T = Terminal */
    if (alt_down && !ctrl_down && sc == 0x12) { kbd_buf_push(0x05); return; }
    if (alt_down && !ctrl_down && sc == 0x14) { kbd_buf_push(0x06); return; }
    /* Ctrl+L = clear terminal */
    if (ctrl_down && !alt_down && sc == 0x26) { kbd_buf_push(0x0C); return; }
    /* Ctrl+C — cancel current input line */
    if (ctrl_down && !alt_down && sc == 0x2E) {
        kbd_buf_push(0x18); /* CAN — terminal clears the line */
        return;
    }
    if (sc < 128) {
        char c = shift_down ? scancode_ascii_shift[sc] : scancode_ascii[sc];
        if (c) kbd_buf_push(c);
    }
}
static void keyboard_init(void) { irq_install_handler(1, keyboard_callback); }

/* ============================================================================
 *  PS/2 MOUSE DRIVER (IRQ12)
 * ==========================================================================*/
static int mouse_x = 400, mouse_y = 300;
static bool_t mouse_left = FALSE, mouse_right = FALSE;
static volatile bool_t mouse_dirty = TRUE;
static void mouse_wait_input(void)  { u32 t=100000; while (t-- && (inb(0x64)&2)) {} }
static void mouse_wait_output(void) { u32 t=100000; while (t-- && !(inb(0x64)&1)) {} }
static void mouse_write(u8 val) { mouse_wait_input(); outb(0x64,0xD4); mouse_wait_input(); outb(0x60,val); }
static u8 mouse_read(void) { mouse_wait_output(); return inb(0x60); }
static u8 mouse_cycle = 0;
static u8 mouse_packet[3];
static void mouse_callback(struct regs *r) {
    (void)r;
    u8 data = inb(0x60);
    switch (mouse_cycle) {
        case 0: if (!(data & 0x08)) return; mouse_packet[0]=data; mouse_cycle=1; break;
        case 1: mouse_packet[1]=data; mouse_cycle=2; break;
        case 2: {
            mouse_packet[2] = data; mouse_cycle = 0;
            u8 flags = mouse_packet[0];
            int dx = mouse_packet[1], dy = mouse_packet[2];
            if (flags & 0x10) dx -= 256;
            if (flags & 0x20) dy -= 256;
            mouse_x += dx; mouse_y -= dy;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if ((u32)mouse_x >= fb_width)  mouse_x = (int)fb_width - 1;
            if ((u32)mouse_y >= fb_height) mouse_y = (int)fb_height - 1;
            mouse_left  = (flags & 0x01) != 0;
            mouse_right = (flags & 0x02) != 0;
            mouse_dirty = TRUE;
            break;
        }
    }
}
static void mouse_init(void) {
    mouse_wait_input(); outb(0x64, 0xA8);
    mouse_wait_input(); outb(0x64, 0x20);
    mouse_wait_output(); u8 status = inb(0x60);
    status |= 0x02; status &= (u8)~0x20;
    mouse_wait_input(); outb(0x64, 0x60);
    mouse_wait_input(); outb(0x60, status);
    mouse_write(0xF6); mouse_read();
    mouse_write(0xF4); mouse_read();
    irq_install_handler(12, mouse_callback);
}

/* ============================================================================
 *  HEAP (bump allocator) -- sized to hold the backbuffer (up to ~1920x1080x4)
 * ==========================================================================*/
#define HEAP_SIZE (10*1024*1024)
static u8 heap_arena[HEAP_SIZE] __attribute__((aligned(16)));
static u32 heap_used = 0;
static void *kmalloc(u32 size) {
    if (size == 0) return NULL;
    u32 need = safety_align16(size);
    if (need == 0) return NULL; /* overflow on align */
    if (!safety_heap_fit(heap_used, need, HEAP_SIZE)) return NULL;
    void *p = &heap_arena[heap_used];
    heap_used += need;
    /* scrub small objects only — full FB wipe is multi-MB and freezes boot */
    if (need <= 4096u)
        safety_wipe(p, need);
    else {
        /* touch first page so mapping faults show early */
        safety_wipe(p, 64);
    }
    return p;
}

/* ============================================================================
 *  RAMFS -- hierarchical in-memory filesystem (files + folders)
 *
 *  Fixed slot table, no dynamic allocation. Each node has a parent index
 *  (-1 = root). Directories have is_dir=TRUE and empty content.
 *  cwd tracks the shell/File-Manager current directory (-1 = root).
 * ==========================================================================*/
#define FS_MAX_FILES     48
#define FS_NAME_MAX      27
#define FS_CONTENT_MAX   2048

typedef struct {
    char   name[FS_NAME_MAX + 1];
    char   content[FS_CONTENT_MAX];
    u32    size;
    bool_t used;
    bool_t is_dir;
    int    parent;   /* -1 = root */
} fs_file_t;

static fs_file_t fs_files[FS_MAX_FILES];
static u32       fs_file_count = 0;
static int       fs_cwd = -1;   /* current directory index, -1 = root */

typedef enum {
    FS_OK = 0,
    FS_ERR_EXISTS,
    FS_ERR_NOTFOUND,
    FS_ERR_FULL,
    FS_ERR_BADNAME,
    FS_ERR_TOOBIG,
    FS_ERR_NOTDIR,
    FS_ERR_NOTEMPTY
} fs_result_t;

static bool_t fs_name_valid(const char *name) {
    u32 len = k_strlen(name);
    if (len == 0 || len > FS_NAME_MAX) return FALSE;
    if (name[0] == '.') return FALSE; /* reserve . and .. */
    for (u32 i = 0; i < len; i++) {
        char c = name[i];
        bool_t ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';
        if (!ok) return FALSE;
    }
    return TRUE;
}

/* Find name inside a specific parent directory (-1 = root). */
static int fs_find_in(const char *name, int parent) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_files[i].used && fs_files[i].parent == parent && k_strcmp(fs_files[i].name, name))
            return i;
    return -1;
}

static int fs_find(const char *name) {
    return fs_find_in(name, fs_cwd);
}

static int fs_find_free(void) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (!fs_files[i].used) return i;
    return -1;
}

static fs_result_t fs_create_ex(const char *name, bool_t is_dir) {
    if (!fs_name_valid(name)) return FS_ERR_BADNAME;
    if (fs_find_in(name, fs_cwd) >= 0) return FS_ERR_EXISTS;
    int idx = fs_find_free();
    if (idx < 0) return FS_ERR_FULL;
    k_memset(&fs_files[idx], 0, sizeof(fs_file_t));
    u32 len = k_strlen(name);
    for (u32 i = 0; i < len; i++) fs_files[idx].name[i] = name[i];
    fs_files[idx].name[len] = '\0';
    fs_files[idx].used = TRUE;
    fs_files[idx].is_dir = is_dir;
    fs_files[idx].parent = fs_cwd;
    fs_files[idx].size = 0;
    fs_file_count++;
    return FS_OK;
}

static fs_result_t fs_create(const char *name) {
    return fs_create_ex(name, FALSE);
}

static fs_result_t fs_mkdir(const char *name) {
    return fs_create_ex(name, TRUE);
}

static fs_result_t fs_write(const char *name, const char *data, u32 len) {
    int idx = fs_find(name);
    if (idx < 0) {
        fs_result_t r = fs_create(name);
        if (r != FS_OK) return r;
        idx = fs_find(name);
    }
    if (fs_files[idx].is_dir) return FS_ERR_NOTDIR;
    if (len > FS_CONTENT_MAX - 1) len = FS_CONTENT_MAX - 1;
    for (u32 i = 0; i < len; i++) fs_files[idx].content[i] = data[i];
    fs_files[idx].content[len] = '\0';
    fs_files[idx].size = len;
    return FS_OK;
}

static fs_result_t fs_append(const char *name, const char *data, u32 len) {
    int idx = fs_find(name);
    if (idx < 0) {
        fs_result_t r = fs_create(name);
        if (r != FS_OK) return r;
        idx = fs_find(name);
    }
    if (fs_files[idx].is_dir) return FS_ERR_NOTDIR;
    u32 space = (FS_CONTENT_MAX - 1) - fs_files[idx].size;
    if (len > space) len = space;
    for (u32 i = 0; i < len; i++) fs_files[idx].content[fs_files[idx].size + i] = data[i];
    fs_files[idx].size += len;
    fs_files[idx].content[fs_files[idx].size] = '\0';
    return FS_OK;
}

static fs_result_t fs_delete(const char *name) {
    int idx = fs_find(name);
    if (idx < 0) return FS_ERR_NOTFOUND;
    if (fs_files[idx].is_dir) {
        /* refuse non-empty dirs */
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (fs_files[i].used && fs_files[i].parent == idx)
                return FS_ERR_NOTEMPTY;
    }
    k_memset(&fs_files[idx], 0, sizeof(fs_file_t));
    fs_files[idx].used = FALSE;
    fs_file_count--;
    return FS_OK;
}

static fs_result_t fs_rename(const char *oldname, const char *newname) {
    int idx = fs_find(oldname);
    if (idx < 0) return FS_ERR_NOTFOUND;
    if (!fs_name_valid(newname)) return FS_ERR_BADNAME;
    if (fs_find_in(newname, fs_files[idx].parent) >= 0) return FS_ERR_EXISTS;
    u32 len = k_strlen(newname);
    for (u32 i = 0; i < len; i++) fs_files[idx].name[i] = newname[i];
    fs_files[idx].name[len] = '\0';
    return FS_OK;
}

static const fs_file_t *fs_read(const char *name) {
    int idx = fs_find(name);
    if (idx < 0 || fs_files[idx].is_dir) return NULL;
    return &fs_files[idx];
}

static fs_result_t fs_copy(const char *src, const char *dst) {
    int s = fs_find(src);
    if (s < 0) return FS_ERR_NOTFOUND;
    if (fs_files[s].is_dir) return FS_ERR_NOTDIR;
    if (!fs_name_valid(dst)) return FS_ERR_BADNAME;
    if (fs_find_in(dst, fs_cwd) >= 0) return FS_ERR_EXISTS;
    fs_result_t r = fs_create(dst);
    if (r != FS_OK) return r;
    int d = fs_find(dst);
    if (d < 0) return FS_ERR_NOTFOUND;
    u32 n = fs_files[s].size;
    if (n > FS_CONTENT_MAX) n = FS_CONTENT_MAX;
    for (u32 i = 0; i < n; i++) fs_files[d].content[i] = fs_files[s].content[i];
    fs_files[d].content[n] = 0;
    fs_files[d].size = n;
    return FS_OK;
}

static fs_result_t fs_cd(const char *name) {
    if (k_strcmp(name, "..")) {
        if (fs_cwd >= 0) fs_cwd = fs_files[fs_cwd].parent;
        return FS_OK;
    }
    if (k_strcmp(name, "/") || k_strcmp(name, "")) {
        fs_cwd = -1;
        return FS_OK;
    }
    int idx = fs_find(name);
    if (idx < 0) return FS_ERR_NOTFOUND;
    if (!fs_files[idx].is_dir) return FS_ERR_NOTDIR;
    fs_cwd = idx;
    return FS_OK;
}

/* Build a simple path string for status display */
static void fs_cwd_path(char *buf, int buflen) {
    if (fs_cwd < 0) { buf[0]='/'; buf[1]=0; return; }
    const char *parts[16];
    int depth = 0;
    int cur = fs_cwd;
    while (cur >= 0 && depth < 16) {
        parts[depth++] = fs_files[cur].name;
        cur = fs_files[cur].parent;
    }
    int pos = 0;
    if (pos < buflen-1) buf[pos++] = '/';
    for (int i = depth-1; i >= 0; i--) {
        int len = 0; while (parts[i][len]) len++;
        for (int j = 0; j < len && pos < buflen-2; j++) buf[pos++] = parts[i][j];
        if (i > 0 && pos < buflen-2) buf[pos++] = '/';
    }
    buf[pos] = 0;
}

/* Seed a few default folders/files at boot */
static void fs_init_defaults(void) {
    fs_cwd = -1;
    fs_mkdir("home");
    fs_mkdir("docs");
    fs_mkdir("apps");
    int home = fs_find("home");
    if (home >= 0) {
        fs_cwd = home;
        fs_write("readme.txt", "Welcome to Titan Glass FS.\nUse mkdir, touch, cd, ls.\n", 52);
        fs_cwd = -1;
    }
    fs_cwd = -1;
}

#define FS_PERSIST_MAGIC 0x46535031u /* FSP1 */
static u32 fs_persist_export(void *buf, u32 cap) {
    /* magic + cwd + count + raw table */
    u32 need = 4u + 4u + 4u + (u32)sizeof(fs_files);
    if (cap < need) return 0;
    u8 *b = (u8*)buf;
    u32 magic = FS_PERSIST_MAGIC;
    for (int i=0;i<4;i++) b[i] = (u8)(magic >> (8*i));
    u32 cwd_u = (u32)fs_cwd;
    for (int i=0;i<4;i++) b[4+i] = (u8)(cwd_u >> (8*i));
    for (int i=0;i<4;i++) b[8+i] = (u8)(fs_file_count >> (8*i));
    u8 *src = (u8*)fs_files;
    for (u32 i = 0; i < (u32)sizeof(fs_files); i++) b[12+i] = src[i];
    return need;
}
static int fs_persist_import(const void *buf, u32 len) {
    u32 need = 4u + 4u + 4u + (u32)sizeof(fs_files);
    if (len < need) return 0;
    const u8 *b = (const u8*)buf;
    u32 magic = (u32)b[0] | ((u32)b[1]<<8) | ((u32)b[2]<<16) | ((u32)b[3]<<24);
    if (magic != FS_PERSIST_MAGIC) return 0;
    fs_cwd = (int)((u32)b[4] | ((u32)b[5]<<8) | ((u32)b[6]<<16) | ((u32)b[7]<<24));
    fs_file_count = (u32)b[8] | ((u32)b[9]<<8) | ((u32)b[10]<<16) | ((u32)b[11]<<24);
    u8 *dst = (u8*)fs_files;
    for (u32 i = 0; i < (u32)sizeof(fs_files); i++) dst[i] = b[12+i];
    if (fs_cwd >= FS_MAX_FILES) fs_cwd = -1;
    if (fs_file_count > FS_MAX_FILES) fs_file_count = FS_MAX_FILES;
    return 1;
}


/* ============================================================================
 *  TERMINAL WINDOW — top-tier: scrollback, history, line edit, colors
 * ==========================================================================*/
#define TERM_SCROLLBACK  256
#define TERM_COLS        72
#define TERM_VIEW_ROWS   20          /* max visible rows (drawn dynamically) */
#define TERM_HIST_MAX    40
#define TERM_ATTR_NORMAL 0
#define TERM_ATTR_DIM    1
#define TERM_ATTR_PROMPT 2
#define TERM_ATTR_ERR    3
#define TERM_ATTR_OK     4
#define TERM_ATTR_INFO   5
#define TERM_ATTR_WARN   6

static char term_sb[TERM_SCROLLBACK][TERM_COLS + 1];
static u8   term_sb_attr[TERM_SCROLLBACK];
static u32  term_sb_len = 0;          /* total lines stored (caps at SCROLLBACK) */
static u32  term_sb_head = 0;         /* next write index (circular) */
static int  term_view = 0;            /* scroll offset from bottom (0 = live) */
static u8   term_cur_attr = TERM_ATTR_NORMAL;
static char term_linebuf[TERM_COLS + 1]; /* current incomplete output line */
static u32  term_linecol = 0;

#define TERM_CMD_MAX 128
/* command history */
static char term_hist[TERM_HIST_MAX][TERM_CMD_MAX];
static u32  term_hist_count = 0;
static int  term_hist_browse = -1;    /* -1 = not browsing; else index */
static char term_hist_stash[TERM_CMD_MAX]; /* save live buffer while browsing */

/* Compatibility aliases used by clear / old code paths */
#define TERM_ROWS TERM_VIEW_ROWS
static u32 term_cur_row = 0, term_cur_col = 0;

/* ============================================================================
 *  NOTEPAD
 * ==========================================================================*/
#define NOTE_ROWS 14
#define NOTE_COLS 38   /* fits inside 520px window with padding */
static char note_lines[NOTE_ROWS][NOTE_COLS+1];
static u32 note_cur_row = 0, note_cur_col = 0;
static char note_filename[FS_NAME_MAX + 1]; /* "" = untitled */
static int  note_file_idx = -1;
static bool_t note_menu_open = FALSE;   /* File dropdown visible */
static bool_t note_saveas = FALSE;      /* Save As name dialog */
static bool_t note_rename = FALSE;      /* Rename dialog */
static char note_namebuf[FS_NAME_MAX + 1];
static u32  note_namelen = 0;
static bool_t fm_pick_mode = FALSE;     /* FM opened to pick file for Notepad */

static void note_putc(char c) {
    if (c == '\n') {
        if (note_cur_row < NOTE_ROWS-1) { note_cur_row++; note_cur_col = 0; }
        return;
    }
    if (c == '\b') {
        if (note_cur_col > 0) {
            note_cur_col--;
            note_lines[note_cur_row][note_cur_col] = '\0';
        } else if (note_cur_row > 0) {
            note_cur_row--;
            note_cur_col = 0;
            while (note_lines[note_cur_row][note_cur_col] && note_cur_col < NOTE_COLS)
                note_cur_col++;
        }
        return;
    }

    /* word-wrap: if this char would go past the edge, move the whole word down first */
    if (note_cur_col >= NOTE_COLS && c != ' ') {
        int word_start = (int)note_cur_col;
        while (word_start > 0 && note_lines[note_cur_row][word_start-1] != ' ')
            word_start--;

        if (word_start > 0 && note_cur_row < NOTE_ROWS-1) {
            /* move the partial word to the next line */
            int i = 0;
            while (note_lines[note_cur_row][word_start + i]) {
                note_lines[note_cur_row+1][i] = note_lines[note_cur_row][word_start + i];
                i++;
            }
            note_lines[note_cur_row+1][i] = '\0';
            note_lines[note_cur_row][word_start] = '\0';
            note_cur_row++;
            note_cur_col = (u32)i;
        } else if (note_cur_row < NOTE_ROWS-1) {
            /* very long unbroken word — hard wrap */
            note_cur_row++;
            note_cur_col = 0;
            note_lines[note_cur_row][0] = '\0';
        } else {
            return; /* no more room */
        }
    } else if (note_cur_col >= NOTE_COLS) {
        /* space at the edge — just go to next line */
        if (note_cur_row < NOTE_ROWS-1) {
            note_cur_row++;
            note_cur_col = 0;
        } else return;
    }

    note_lines[note_cur_row][note_cur_col++] = c;
    note_lines[note_cur_row][note_cur_col] = '\0';
}

static void note_load_file(int idx) {
    k_memset(note_lines, 0, sizeof(note_lines));
    note_cur_row = 0;
    note_cur_col = 0;
    if (idx < 0 || !fs_files[idx].used) return;
    const char *src = fs_files[idx].content;
    u32 len = fs_files[idx].size;
    for (u32 i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\n') {
            if (note_cur_row < NOTE_ROWS - 1) { note_cur_row++; note_cur_col = 0; }
            continue;
        }
        if (note_cur_col >= NOTE_COLS) {
            if (note_cur_row < NOTE_ROWS - 1) { note_cur_row++; note_cur_col = 0; }
            else break;
        }
        note_lines[note_cur_row][note_cur_col++] = c;
        note_lines[note_cur_row][note_cur_col] = '\0';
    }
}



/* Push a finished line into the circular scrollback */
static void term_sb_push(const char *line, u8 attr) {
    u32 idx = term_sb_head % TERM_SCROLLBACK;
    u32 i = 0;
    while (line[i] && i < TERM_COLS) {
        term_sb[idx][i] = line[i];
        i++;
    }
    term_sb[idx][i] = 0;
    term_sb_attr[idx] = attr;
    term_sb_head++;
    if (term_sb_len < TERM_SCROLLBACK) term_sb_len++;
    /* follow live output unless user scrolled up */
    if (term_view == 0) { /* stay pinned to bottom */ }
    else if (term_view > 0) {
        /* keep visual position roughly stable when new lines arrive */
        if (term_view < (int)TERM_SCROLLBACK - 1) term_view++;
    }
    term_cur_row = term_sb_len;
    term_cur_col = 0;
}

static void term_new_line(void) {
    term_linebuf[term_linecol] = 0;
    term_sb_push(term_linebuf, term_cur_attr);
    term_linecol = 0;
    term_linebuf[0] = 0;
    term_cur_attr = TERM_ATTR_NORMAL;
}

static void term_putc(char c) {
    serial_putc(c);
    if (c == '\n') { term_new_line(); return; }
    if (c == '\b') {
        if (term_linecol > 0) {
            term_linecol--;
            term_linebuf[term_linecol] = 0;
        }
        return;
    }
    if (c == '\r') return;
    if (term_linecol >= TERM_COLS) term_new_line();
    term_linebuf[term_linecol++] = c;
    term_linebuf[term_linecol] = 0;
}

static void term_puts(const char *s) { while (*s) term_putc(*s++); }

static void term_printf(const char *fmt, ...) {
    __builtin_va_list a; __builtin_va_start(a, fmt);
    generic_vprintf(term_putc, fmt, a);
    __builtin_va_end(a);
}

/* Colored helpers */
static void term_print_attr(u8 attr, const char *s) {
    u8 prev = term_cur_attr;
    term_cur_attr = attr;
    term_puts(s);
    term_cur_attr = prev;
}

static void term_clear_screen(void) {
    term_sb_len = 0;
    term_sb_head = 0;
    term_view = 0;
    term_linecol = 0;
    term_linebuf[0] = 0;
    term_cur_attr = TERM_ATTR_NORMAL;
    k_memset(term_sb, 0, sizeof(term_sb));
    k_memset(term_sb_attr, 0, sizeof(term_sb_attr));
}

static void term_hist_push(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    /* skip duplicate of last */
    if (term_hist_count > 0) {
        u32 last = (term_hist_count - 1) % TERM_HIST_MAX;
        u32 i = 0;
        while (term_hist[last][i] && term_hist[last][i] == cmd[i]) i++;
        if (term_hist[last][i] == 0 && cmd[i] == 0) return;
    }
    u32 idx = term_hist_count % TERM_HIST_MAX;
    u32 i = 0;
    while (cmd[i] && i < TERM_CMD_MAX - 1) {
        term_hist[idx][i] = cmd[i];
        i++;
    }
    term_hist[idx][i] = 0;
    term_hist_count++;
}

static int term_sb_index(int from_bottom) {
    /* from_bottom 0 = newest completed line */
    if (term_sb_len == 0) return -1;
    if (from_bottom < 0 || from_bottom >= (int)term_sb_len) return -1;
    u32 pos = (term_sb_head - 1 - (u32)from_bottom + TERM_SCROLLBACK * 4) % TERM_SCROLLBACK;
    return (int)pos;
}

static void term_scroll_by(int delta) {
    int max_scroll = (int)term_sb_len - 2;
    if (max_scroll < 0) max_scroll = 0;
    term_view += delta;
    if (term_view < 0) term_view = 0;
    if (term_view > max_scroll) term_view = max_scroll;
}

/* ============================================================================
 *  WINDOW MANAGER
 * ==========================================================================*/
typedef struct {
    char title[32];
    int x, y, w, h;
    bool_t visible, has_close, is_terminal, is_about, is_notepad, is_files, is_settings, is_calc, is_paint, is_launcher, is_taskmgr;
    bool_t maximized;
    int rest_x, rest_y, rest_w, rest_h;
} gui_window_t;

#define MAX_WINDOWS 9
static gui_window_t windows[MAX_WINDOWS];
static int win_order[MAX_WINDOWS];
#define WIN_TERMINAL 0
#define WIN_ABOUT    1
#define WIN_NOTEPAD  2
#define WIN_FILES    3
#define WIN_SETTINGS 4
#define WIN_CALC     5
#define WIN_PAINT    6
#define WIN_LAUNCHER 7
#define WIN_TASKMGR  8
static int focused_win;
static void bring_to_front(int idx);
static int dragging = -1;
static int drag_dx = 0, drag_dy = 0;
static bool_t prev_left = FALSE;
static bool_t prev_right = FALSE;
static bool_t start_menu_open = FALSE;
static bool_t desk_ctx_open = FALSE;
static int desk_ctx_x = 0, desk_ctx_y = 0;
#define DESK_CTX_N 6
#define DESK_CTX_W 178
#define DESK_CTX_ITEM_H 28
#define DESK_CTX_H (DESK_CTX_N * DESK_CTX_ITEM_H + 12)
static char persist_pw[LOCK_PASS_MAX + 1];
static bool_t persist_pw_set = FALSE;
static void persist_pw_clear(void) {
    safety_wipe(persist_pw, sizeof(persist_pw));
    persist_pw_set = FALSE;
}

/* forward declarations */
static void bring_to_front(int idx);
static void note_clear(void) {
    k_memset(note_lines, 0, sizeof(note_lines));
    note_cur_row = 0;
    note_cur_col = 0;
    note_filename[0] = 0;
    note_file_idx = -1;
}

static void note_save_to_idx(int idx) {
    if (idx < 0 || !fs_files[idx].used || fs_files[idx].is_dir) return;
    /* rebuild content from lines */
    char buf[FS_CONTENT_MAX];
    u32 pos = 0;
    for (u32 r = 0; r < NOTE_ROWS && pos < FS_CONTENT_MAX - 1; r++) {
        u32 llen = k_strlen(note_lines[r]);
        for (u32 c = 0; c < llen && pos < FS_CONTENT_MAX - 1; c++)
            buf[pos++] = note_lines[r][c];
        if (r + 1 < NOTE_ROWS && note_lines[r+1][0] && pos < FS_CONTENT_MAX - 1)
            buf[pos++] = '\n';
    }
    buf[pos] = 0;
    fs_files[idx].size = pos;
    for (u32 i = 0; i < pos; i++) fs_files[idx].content[i] = buf[i];
    fs_files[idx].content[pos] = 0;
}

static void note_open_file(int idx) {
    if (idx < 0 || !fs_files[idx].used || fs_files[idx].is_dir) return;
    note_load_file(idx);
    note_file_idx = idx;
    u32 nlen = k_strlen(fs_files[idx].name);
    for (u32 i = 0; i < nlen; i++) note_filename[i] = fs_files[idx].name[i];
    note_filename[nlen] = 0;
    windows[WIN_NOTEPAD].visible = TRUE;
    bring_to_front(WIN_NOTEPAD);
    focused_win = WIN_NOTEPAD;
    fm_pick_mode = FALSE;
    note_menu_open = FALSE;
}


/* ---- File Manager state (top-tier) ---- */
static int  fm_selected = -1;
static int  fm_last_click_idx = -1;
static u32  fm_last_click_tick = 0;
static bool_t fm_naming = FALSE;
static bool_t fm_renaming = FALSE;
static bool_t fm_mkdir_mode = FALSE;
static char fm_namebuf[FS_NAME_MAX + 1];
static u32  fm_namelen = 0;
static char fm_status[56] = "";
static u32  fm_status_tick = 0;
static int  fm_view = 0;            /* 0=grid 1=list */
static int  fm_sort = 0;            /* 0=name 1=size 2=type */
static int  fm_scroll = 0;
static int  fm_clip_idx = -1;
static bool_t fm_clip_cut = FALSE;
static bool_t fm_props_open = FALSE;
static bool_t fm_ctx_open = FALSE;
static int    fm_ctx_x = 0, fm_ctx_y = 0;
static int    fm_ctx_file = -1;
#define FM_CTX_W  148
#define FM_CTX_ITEMS 7
#define FM_CTX_H  (FM_CTX_ITEMS * 26 + 10)
#define FM_SIDEBAR_W  120
#define FM_TOOLBAR_H  36
#define FM_PATH_H     28
#define FM_STATUS_H   22
#define FM_CARD_W     88
#define FM_CARD_H     92
#define FM_CARD_GAP   12
#define FM_LIST_ROW   28

static void fm_set_status(const char *msg) {
    u32 i = 0;
    while (msg[i] && i < sizeof(fm_status) - 1) { fm_status[i] = msg[i]; i++; }
    fm_status[i] = 0;
    fm_status_tick = timer_ticks;
}

static int fm_collect(int *out, int maxn) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES && n < maxn; i++) {
        if (!fs_files[i].used || fs_files[i].parent != fs_cwd) continue;
        out[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int key = out[a], b = a - 1;
        while (b >= 0) {
            fs_file_t *ka = &fs_files[key], *kb = &fs_files[out[b]];
            int less = 0;
            if (ka->is_dir != kb->is_dir) less = ka->is_dir ? 1 : 0;
            else {
                const char *na = ka->name, *nb = kb->name;
                if (fm_sort == 1) {
                    if (ka->size != kb->size) less = (ka->size < kb->size);
                    else {
                        while (*na && *nb && *na == *nb) { na++; nb++; }
                        less = (u8)*na < (u8)*nb;
                    }
                } else {
                    while (*na && *nb && *na == *nb) { na++; nb++; }
                    less = (u8)*na < (u8)*nb;
                }
            }
            if (!less) break;
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

static int fm_find_named(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (fs_files[i].used && fs_files[i].is_dir && k_strcmp(fs_files[i].name, name))
            return i;
    }
    return -1;
}
static void fm_open_selected(void);
static void fm_do_delete(int idx);
static void fm_do_paste(void);

static void bring_to_front(int idx) {
    int pos = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (win_order[i] == idx) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < MAX_WINDOWS-1; i++) win_order[i] = win_order[i+1];
    win_order[MAX_WINDOWS-1] = idx;
}
static bool_t point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x+w && py >= y && py < y+h;
}

/* ============================================================================
 *  SHELL COMMANDS
 * ==========================================================================*/
#define CMD_BUF_SIZE TERM_CMD_MAX
static char cmd_buf[CMD_BUF_SIZE];
static u32  cmd_len = 0;
static u32  cmd_cursor = 0; /* caret position inside cmd_buf */
static u32  g_theme = 0; /* cycles accent hue */
static u32  g_wallpaper = 0; /* desktop gradient preset */
static u32  g_security_level = 1; /* 0=basic 1=standard 2=high */
static u32  last_input_tick = 0; /* idle auto-lock */
static int  settings_tab = 0; /* 0 lock 1 wall 2 security 3 account 4 system */
static u32  g_idle_timeout_sec = 120; /* default 2 min (standard) */

/* AUDIT LOG + SECURE SESSION (extreme offline security) */
#define AUDIT_MAX 64
#define AUDIT_LINE 48
static char audit_log[AUDIT_MAX][AUDIT_LINE];
static u32  audit_count = 0;
static u32  audit_head = 0;
static u32  g_login_ok_count = 0;
static u32  g_login_fail_count = 0;

static void audit_add(const char *msg) {
    if (!msg) return;
    u32 i = 0;
    char *dst = audit_log[audit_head];
    while (msg[i] && i < AUDIT_LINE - 1) { dst[i] = msg[i]; i++; }
    dst[i] = 0;
    audit_head = (audit_head + 1) % AUDIT_MAX;
    if (audit_count < AUDIT_MAX) audit_count++;
}

/* Clipboard (Notepad <-> shell) */
#define CLIP_MAX 512
static char g_clipboard[CLIP_MAX];
static u32  g_clip_len = 0;

static void clip_set(const char *s, u32 n) {
    if (n >= CLIP_MAX) n = CLIP_MAX - 1;
    for (u32 i = 0; i < n; i++) g_clipboard[i] = s[i];
    g_clipboard[n] = 0;
    g_clip_len = n;
    cliphist_push(s, n);
}

static void session_secure_lock(void) {
    /* Always wipe session key material */
    persist_pw_clear();
    /* High security: wipe editor secrets + clipboard */
    if (g_security_level >= 2) {
        note_clear();
        safety_wipe(g_clipboard, sizeof(g_clipboard));
        g_clip_len = 0;
        safety_wipe(cmd_buf, sizeof(cmd_buf));
        cmd_len = 0;
        cmd_cursor = 0;
        term_hist_browse = -1;
        audit_add("LOCK: high-sec wipe secrets");
    } else {
        audit_add("LOCK: session locked");
    }
    lock_activate();
}





static void on_unlock_success(const char *password) {
    u32 n = 0;
    while (password[n] && n < LOCK_PASS_MAX) { persist_pw[n] = password[n]; n++; }
    persist_pw[n] = 0;
    persist_pw_set = TRUE;
    lock_set_password_memory(password);
    g_login_ok_count++;
    audit_add("AUTH: login OK");
    if (persist_disk_present()) {
        u32 w=0, th=0, sec=0, idle=120;
        if (persist_cred_load(&w, &th, &sec, &idle)) {
            g_wallpaper = w;
            g_theme = th;
            g_security_level = sec;
            g_idle_timeout_sec = idle ? idle : 120;
            lock_set_security_level(g_security_level);
            /* desktop cache invalidated on next redraw via theme key */
        } else {
            /* first-time: write default creds so next reboot keeps password */
            persist_cred_save(password, g_wallpaper, g_theme, g_security_level, g_idle_timeout_sec);
        }
        if (!persist_load(password)) {
            persist_format(password);
        }
    }
}

static void settings_persist_all(const char *password) {
    if (!password || !password[0]) return;
    if (!persist_disk_present()) return;
    persist_cred_save(password, g_wallpaper, g_theme, g_security_level, g_idle_timeout_sec);
    /* also refresh encrypted volume so files survive */
    persist_save(password);
}

static int lock_verify_password(const char *user, const char *password) {
    /* username must be titan */
    const char *u = "titan";
    while (*u && *user && *u == *user) { u++; user++; }
    if (*u || *user) return 0;
    if (!password) return 0;
    int ok = 0;
    if (persist_disk_present() && persist_cred_exists()) {
        ok = persist_cred_verify(password) ? 1 : 0;
    } else {
        /* No encrypted volume yet: never use a hardcoded default secret.
         * First boot accepts empty password only; after unlock, session key is used. */
        if (persist_pw_set) {
            const char *e = persist_pw;
            const char *p = password;
            while (*e && *p && *e == *p) { e++; p++; }
            ok = (*e == 0 && *p == 0) ? 1 : 0;
        } else {
            ok = (password[0] == 0) ? 1 : 0;
        }
    }
    if (!ok) {
        g_login_fail_count++;
        audit_add("AUTH: login FAIL");
    }
    return ok;
}

static char settings_old[LOCK_PASS_MAX+1];
static char settings_new[LOCK_PASS_MAX+1];
static u32  settings_old_len = 0, settings_new_len = 0;
static int  settings_pw_focus = 0; /* 0 old 1 new */
static char settings_status[40] = "";

static void shell_help(void) {
    term_print_attr(TERM_ATTR_INFO, "Titan shell — keys: Up/Down history  Left/Right edit  PgUp/PgDn scroll\n");
    term_printf("  Tab=complete  Ctrl+L=clear  Ctrl+C=cancel  Home/End  Del\n");
    term_printf(" help clear cls history echo TXT date time whoami uname version\n");
    term_printf(" uptime meminfo free sysinfo neofetch status\n");
    term_printf(" ls ll tree pwd cd NAME mkdir NAME rmdir NAME\n");
    term_printf(" touch NAME cat NAME head NAME wc NAME\n");
    term_printf(" write NAME TEXT  rm NAME  cp SRC DST  mv OLD NEW\n");
    term_printf(" savenote NAME  notepad files settings about calc paint applauncher taskmgr\n");
    term_printf(" theme lock sync logout audit\n");
    term_printf(" clip  cliphist  clipclear  (clipboard)\n");
    term_printf(" ifconfig  ping IP  arp  lspci\n");
    term_printf(" panic reboot\n");
}
static void reboot(void) {
    u8 tmp;
    __asm__ volatile ("cli");
    do { tmp = inb(0x64); if (tmp & 1) inb(0x60); } while (tmp & 2);
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); }
}
static void shell_prompt(void) {
    /* Prompt is drawn live in the terminal UI (not pushed to scrollback
     * until the user submits a command). Keep a soft marker for serial. */
    char path[64];
    fs_cwd_path(path, sizeof(path));
    serial_putc('\n');
    /* nothing printed to scrollback — UI paints "titan:path> " itself */
    (void)path;
    term_view = 0; /* jump to live on new prompt */
    term_hist_browse = -1;
}

static void shell_execute(const char *line) {
    if (k_strlen(line) == 0) return;
    if (k_strcmp(line, "help")) { shell_help(); }
    else if (k_strcmp(line, "clear") || k_strcmp(line, "cls")) {
        term_clear_screen();
        term_print_attr(TERM_ATTR_INFO, "Terminal cleared.\n");
    }
    else if (k_strcmp(line, "history")) {
        u32 n = term_hist_count < TERM_HIST_MAX ? term_hist_count : TERM_HIST_MAX;
        if (n == 0) term_print_attr(TERM_ATTR_DIM, "(no history yet)\n");
        else {
            for (u32 i = 0; i < n; i++) {
                u32 idx = (term_hist_count - n + i) % TERM_HIST_MAX;
                term_printf(" %u  %s\n", i + 1, term_hist[idx]);
            }
        }
    }
    else if (k_strncmp_prefix(line, "echo ")) { term_printf("%s\n", line + 5); }
    else if (k_strcmp(line, "uptime")) { term_printf("uptime: %u ticks (~%u sec)\n", timer_ticks, timer_ticks/PIT_HZ); }
    else if (k_strcmp(line, "meminfo")) { term_printf("heap: %u / %u bytes used\n", heap_used, (u32)HEAP_SIZE); }
    else if (k_strcmp(line, "sysinfo") || k_strcmp(line, "neofetch")) {
        term_printf("\n  Titan Kernel v3.0  \"Glass Edition\"\n");
        term_printf("  ---------------------------\n");
        term_printf("  OS:       Titan (hobby x86)\n");
        term_printf("  Kernel:   v3.0 glass\n");
        term_printf("  Arch:     i386 protected mode\n");
        term_printf("  Screen:   %ux%u @ %ubpp\n", fb_width, fb_height, (u32)fb_bpp);
        term_printf("  Memory:   %u / %u KiB used\n", heap_used/1024, (u32)HEAP_SIZE/1024);
        term_printf("  Uptime:   %u sec\n", timer_ticks/PIT_HZ);
        term_printf("  Theme:    #%u\n", g_theme);
        term_printf("  Mouse:    (%d,%d)\n", mouse_x, mouse_y);
        term_printf("  Disk:     %s\n", persist_disk_present()?"present":"none");
        term_printf("  Security: %s\n\n",
            g_security_level>=2?"HIGH":(g_security_level>=1?"STANDARD":"BASIC"));
    }
    else if (k_strcmp(line, "about")) { windows[WIN_ABOUT].visible = TRUE; bring_to_front(WIN_ABOUT); term_printf("Opened About window.\n"); }
    else if (k_strcmp(line, "notepad") || k_strcmp(line, "note")) {
        windows[WIN_NOTEPAD].visible = TRUE;
        bring_to_front(WIN_NOTEPAD);
        focused_win = WIN_NOTEPAD;
        term_printf("Opened Notepad.\n");
    }
    else if (k_strcmp(line, "files")) {
        windows[WIN_FILES].visible = TRUE;
        bring_to_front(WIN_FILES);
        term_printf("Opened File Manager.\n");
    }
    else if (k_strcmp(line, "calc") || k_strcmp(line, "calculator")) {
        calc_open();
        windows[WIN_CALC].visible = TRUE;
        bring_to_front(WIN_CALC);
        focused_win = WIN_CALC;
        term_printf("Opened Calculator.\n");
    }
    else if (k_strcmp(line, "paint")) {
        paint_open();
        windows[WIN_PAINT].visible = TRUE;
        bring_to_front(WIN_PAINT);
        focused_win = WIN_PAINT;
        term_printf("Opened Paint.\n");
    }
    else if (k_strcmp(line, "applauncher") || k_strcmp(line, "apps") || k_strcmp(line, "launcher")) {
        al_open();
        windows[WIN_LAUNCHER].visible = TRUE;
        bring_to_front(WIN_LAUNCHER);
        focused_win = WIN_LAUNCHER;
        term_printf("Opened App Launcher.\n");
    }
    else if (k_strcmp(line, "taskmgr") || k_strcmp(line, "tasks")) {
        request_taskmgr = TRUE;
        term_printf("Opened Task Manager.\n");
    }
    else if (k_strcmp(line, "ls")) {
        int n = 0;
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used || fs_files[i].parent != fs_cwd) continue;
            if (fs_files[i].is_dir)
                term_printf(" [DIR]  %s\n", fs_files[i].name);
            else
                term_printf("        %s  (%u bytes)\n", fs_files[i].name, fs_files[i].size);
            n++;
        }
        if (n == 0) term_printf("(empty)\n");
    }
    else if (k_strcmp(line, "pwd")) {
        char path[96];
        fs_cwd_path(path, sizeof(path));
        term_printf("%s\n", path);
    }
    else if (k_strncmp_prefix(line, "cd ")) {
        fs_result_t r = fs_cd(line + 3);
        if (r == FS_ERR_NOTDIR) term_printf("Not a directory.\n");
        else if (r != FS_OK) term_printf("No such directory.\n");
    }
    else if (k_strcmp(line, "cd")) {
        fs_cwd = -1;
    }
    else if (k_strncmp_prefix(line, "mkdir ")) {
        fs_result_t r = fs_mkdir(line + 6);
        if (r == FS_OK) term_printf("Created folder '%s'.\n", line + 6);
        else if (r == FS_ERR_EXISTS) term_printf("Already exists.\n");
        else if (r == FS_ERR_FULL) term_printf("File table full.\n");
        else term_printf("Invalid name.\n");
    }
    else if (k_strncmp_prefix(line, "touch ")) {
        fs_result_t r = fs_create(line + 6);
        if (r == FS_OK) term_printf("Created '%s'.\n", line + 6);
        else if (r == FS_ERR_EXISTS) term_printf("'%s' already exists.\n", line + 6);
        else if (r == FS_ERR_FULL) term_printf("File table full (max %u files).\n", (u32)FS_MAX_FILES);
        else term_printf("Invalid filename.\n");
    }
    else if (k_strncmp_prefix(line, "cat ")) {
        const fs_file_t *f = fs_read(line + 4);
        if (!f) term_printf("No such file: '%s'\n", line + 4);
        else if (f->size == 0) term_printf("(empty)\n");
        else term_printf("%s\n", f->content);
    }
    else if (k_strncmp_prefix(line, "rm ")) {
        fs_result_t r = fs_delete(line + 3);
        if (r == FS_OK) {
            term_printf("Deleted '%s'.\n", line + 3);
            audit_add("FS: delete");
            if (fm_selected >= 0 && !fs_files[fm_selected].used) fm_selected = -1;
        }
        else if (r == FS_ERR_NOTEMPTY) term_printf("Directory not empty.\n");
        else term_printf("No such file: '%s'\n", line + 3);
    }
    else if (k_strncmp_prefix(line, "savenote ")) {
        /* Saves the current Notepad contents into a file, joining its
         * on-screen lines back into one block of text with newlines. */
        const char *name = line + 9;
        if (!fs_name_valid(name)) { term_printf("Invalid filename.\n"); }
        else {
            fs_delete(name); /* start clean so re-saving doesn't append stale content */
            for (u32 r = 0; r < NOTE_ROWS; r++) {
                u32 llen = k_strlen(note_lines[r]);
                if (llen == 0 && r == NOTE_ROWS-1) break;
                fs_append(name, note_lines[r], llen);
                if (r < NOTE_ROWS-1) fs_append(name, "\n", 1);
            }
            term_printf("Saved Notepad to '%s'.\n", name);
        }
    }
    else if (k_strncmp_prefix(line, "write ")) {
        /* syntax: write NAME rest-of-line-is-content */
        const char *p = line + 6;
        const char *name_start = p;
        while (*p && *p != ' ') p++;
        u32 name_len = (u32)(p - name_start);
        if (name_len == 0 || name_len > FS_NAME_MAX) { term_printf("Usage: write NAME text...\n"); }
        else {
            char nbuf[FS_NAME_MAX + 1];
            for (u32 i = 0; i < name_len; i++) nbuf[i] = name_start[i];
            nbuf[name_len] = '\0';
            if (*p == ' ') p++;
            fs_result_t r = fs_write(nbuf, p, k_strlen(p));
            if (r == FS_OK) term_printf("Wrote %u bytes to '%s'.\n", k_strlen(p), nbuf);
            else term_printf("Invalid filename.\n");
        }
    }
    else if (k_strcmp(line, "theme")) { g_theme = (g_theme+1) % 3; term_printf("accent theme #%u\n", g_theme); }
    else if (k_strcmp(line, "sync")) {
        if (!persist_disk_present()) term_printf("No disk.\n");
        else if (!persist_pw_set) term_printf("Unlock session required.\n");
        else if (persist_save(persist_pw)) term_printf("Synced encrypted volume OK.\n");
        else term_printf("Sync failed: %s\n", persist_last_error());
    }

    else if (k_strcmp(line, "date") || k_strcmp(line, "time")) {
        char tb[16], db[24];
        lock_format_time(tb, sizeof(tb));
        lock_format_date(db, sizeof(db));
        term_printf("%s  %s\n", tb, db);
    }
    else if (k_strcmp(line, "whoami")) { term_printf("titan\n"); }
    else if (k_strcmp(line, "uname") || k_strcmp(line, "version")) {
        term_printf("Titan Kernel v3 Glass Edition\n");
        term_printf("arch: i386  offline-first  encrypted FS\n");
    }
    else if (k_strcmp(line, "free")) {
        term_printf("heap used: %u / %u bytes (%u KiB free)\n",
            heap_used, (u32)HEAP_SIZE, ((u32)HEAP_SIZE - heap_used)/1024);
    }
    else if (k_strcmp(line, "status")) {
        term_printf("security: %s\n",
            g_security_level>=2?"HIGH":(g_security_level>=1?"STANDARD":"BASIC"));
        term_printf("idle lock: %u sec\n", g_idle_timeout_sec);
        term_printf("disk: %s\n", persist_disk_present()?"present":"none");
        term_printf("session: %s\n", persist_pw_set?"unlocked":"locked-key-cleared");
        term_printf("files: %u nodes\n", fs_file_count);
    }
    else if (k_strcmp(line, "tree")) {
        int shown = 0;
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used) continue;
            if (fs_files[i].parent != fs_cwd) continue;
            term_printf("%s %s\n", fs_files[i].is_dir ? "+-" : "  ", fs_files[i].name);
            if (fs_files[i].is_dir) {
                int p = i;
                for (int j = 0; j < FS_MAX_FILES; j++) {
                    if (!fs_files[j].used || fs_files[j].parent != p) continue;
                    term_printf("   %s %s\n", fs_files[j].is_dir ? "+-" : "|-", fs_files[j].name);
                }
            }
            shown++;
        }
        if (!shown) term_printf("(empty)\n");
    }
    else if (k_strncmp_prefix(line, "rmdir ")) {
        const char *name = line + 6;
        int idx = fs_find(name);
        if (idx < 0) term_printf("No such dir: '%s'\n", name);
        else if (!fs_files[idx].is_dir) term_printf("Not a directory.\n");
        else {
            fs_result_t r = fs_delete(name);
            if (r == FS_OK) term_printf("Removed dir '%s'.\n", name);
            else if (r == FS_ERR_NOTEMPTY) term_printf("Directory not empty.\n");
            else term_printf("rmdir failed.\n");
        }
    }
    else if (k_strncmp_prefix(line, "head ")) {
        const fs_file_t *f = fs_read(line + 5);
        if (!f) term_printf("No such file.\n");
        else {
            u32 n = f->size; if (n > 200) n = 200;
            for (u32 i = 0; i < n; i++) {
                char ch[2] = { f->content[i], 0 };
                if (ch[0] == '\n') term_printf("\n");
                else term_printf("%s", ch);
            }
            if (n < f->size) term_printf("\n...(truncated)\n");
            else term_printf("\n");
        }
    }
    else if (k_strncmp_prefix(line, "wc ")) {
        const fs_file_t *f = fs_read(line + 3);
        if (!f) term_printf("No such file.\n");
        else {
            u32 lines=0, words=0, chars=f->size;
            int in_word = 0;
            for (u32 i = 0; i < f->size; i++) {
                char c = f->content[i];
                if (c == '\n') lines++;
                if (c==' '||c=='\n'||c=='\t') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
            term_printf("%u lines, %u words, %u bytes\n", lines, words, chars);
        }
    }
    else if (k_strncmp_prefix(line, "cp ")) {
        const char *p = line + 3;
        while (*p == ' ') p++;
        const char *s1 = p;
        while (*p && *p != ' ') p++;
        char src[FS_NAME_MAX+1], dst[FS_NAME_MAX+1];
        u32 n1 = (u32)(p - s1);
        if (n1 == 0 || n1 > FS_NAME_MAX) { term_printf("Usage: cp SRC DST\n"); }
        else {
            for (u32 i = 0; i < n1; i++) src[i] = s1[i];
            src[n1] = 0;
            while (*p == ' ') p++;
            u32 n2 = 0;
            while (p[n2] && p[n2] != ' ' && n2 < FS_NAME_MAX) { dst[n2] = p[n2]; n2++; }
            dst[n2] = 0;
            if (n2 == 0) term_printf("Usage: cp SRC DST\n");
            else {
                fs_result_t r = fs_copy(src, dst);
                if (r == FS_OK) term_printf("Copied '%s' -> '%s'.\n", src, dst);
                else if (r == FS_ERR_NOTFOUND) term_printf("Source not found.\n");
                else if (r == FS_ERR_EXISTS) term_printf("Destination exists.\n");
                else if (r == FS_ERR_NOTDIR) term_printf("Cannot copy directory.\n");
                else term_printf("cp failed.\n");
            }
        }
    }
    else if (k_strncmp_prefix(line, "mv ")) {
        const char *p = line + 3;
        while (*p == ' ') p++;
        const char *s1 = p;
        while (*p && *p != ' ') p++;
        char src[FS_NAME_MAX+1], dst[FS_NAME_MAX+1];
        u32 n1 = (u32)(p - s1);
        if (n1 == 0 || n1 > FS_NAME_MAX) { term_printf("Usage: mv OLD NEW\n"); }
        else {
            for (u32 i = 0; i < n1; i++) src[i] = s1[i];
            src[n1] = 0;
            while (*p == ' ') p++;
            u32 n2 = 0;
            while (p[n2] && p[n2] != ' ' && n2 < FS_NAME_MAX) { dst[n2] = p[n2]; n2++; }
            dst[n2] = 0;
            if (n2 == 0) term_printf("Usage: mv OLD NEW\n");
            else {
                fs_result_t r = fs_rename(src, dst);
                if (r == FS_OK) term_printf("Renamed '%s' -> '%s'.\n", src, dst);
                else if (r == FS_ERR_NOTFOUND) term_printf("Not found.\n");
                else if (r == FS_ERR_EXISTS) term_printf("Target exists.\n");
                else term_printf("mv failed.\n");
            }
        }
    }
    else if (k_strcmp(line, "settings")) {
        windows[WIN_SETTINGS].visible = TRUE;
        bring_to_front(WIN_SETTINGS);
        focused_win = WIN_SETTINGS;
        term_printf("Opened Settings.\n");
    }
    else if (k_strcmp(line, "lock") || k_strcmp(line, "logout")) {
        if (persist_pw_set) settings_persist_all(persist_pw);
        session_secure_lock();
        term_printf("Locked.\n");
    }
    else if (k_strcmp(line, "ll")) {
        int shown = 0;
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!fs_files[i].used || fs_files[i].parent != fs_cwd) continue;
            if (fs_files[i].is_dir)
                term_printf("d  %-20s\n", fs_files[i].name);
            else
                term_printf("-  %-20s %u\n", fs_files[i].name, fs_files[i].size);
            shown++;
        }
        if (!shown) term_printf("(empty)\n");
    }

        else if (k_strcmp(line, "audit")) {
        term_printf("--- audit log (%u) fails=%u ok=%u ---\n",
            audit_count, g_login_fail_count, g_login_ok_count);
        if (audit_count == 0) term_printf("(empty)\n");
        else {
            u32 start = (audit_head + AUDIT_MAX - audit_count) % AUDIT_MAX;
            for (u32 i = 0; i < audit_count; i++) {
                u32 idx = (start + i) % AUDIT_MAX;
                term_printf("%u: %s\n", i + 1, audit_log[idx]);
            }
        }
    }
    else if (k_strcmp(line, "clip")) {
        if (g_clip_len == 0) term_printf("(clipboard empty)\n");
        else term_printf("%s\n", g_clipboard);
    }
    else if (k_strcmp(line, "clipnote")) {
        /* copy first notepad line to clipboard */
        u32 n = k_strlen(note_lines[0]);
        clip_set(note_lines[0], n);
        term_printf("Copied Notepad line1 to clipboard (%u bytes).\n", n);
        audit_add("CLIP: from notepad");
    }
    else if (k_strcmp(line, "pastenote")) {
        if (g_clip_len == 0) term_printf("Clipboard empty.\n");
        else {
            for (u32 i = 0; i < g_clip_len; i++) note_putc(g_clipboard[i]);
            term_printf("Pasted clipboard into Notepad.\n");
            audit_add("CLIP: to notepad");
        }
    }
    else if (k_strcmp(line, "cliphist")) {
        u32 n = cliphist_count();
        if (n == 0) term_printf("(history empty)\n");
        else {
            for (u32 i = 0; i < n; i++) {
                const char *e = cliphist_get((int)i);
                if (e) term_printf("%u: %s\n", i + 1, e);
            }
        }
    }
    else if (k_strcmp(line, "clipclear")) {
        safety_wipe(g_clipboard, sizeof(g_clipboard));
        g_clip_len = 0;
        cliphist_clear();
        audit_add("CLIP: cleared");
        term_printf("Clipboard wiped.\n");
    }
    else if (k_strcmp(line, "ifconfig") || k_strcmp(line, "ip")) {
        net_cmd_ifconfig(term_printf);
    }
    else if (k_strncmp_prefix(line, "ping ")) {
        net_cmd_ping(line + 5, term_printf);
    }
    else if (k_strcmp(line, "ping")) {
        term_printf("usage: ping <ip>\nexample: ping 10.0.2.2\n");
    }
    else if (k_strcmp(line, "arp")) {
        net_cmd_arp(term_printf);
    }
    else if (k_strcmp(line, "lspci")) {
        net_cmd_lspci(term_printf);
    }
    else if (k_strcmp(line, "panic")) { panic("User-triggered test panic (all systems otherwise nominal)"); }
    else if (k_strcmp(line, "reboot")) { term_printf("Rebooting...\n"); reboot(); }
    else { term_printf("Unknown command: '%s'\n", line); }
}
static void fm_handle_key(char c) {
    if (fm_props_open) {
        if (c == 27 || c == '\n') fm_props_open = FALSE;
        return;
    }
    if (!fm_naming) {
        if (c == 0x01) { if (fm_scroll > 0) fm_scroll--; return; }
        if (c == 0x02) { fm_scroll++; return; }
        if (c == 0x0F) { fm_scroll -= 3; if (fm_scroll < 0) fm_scroll = 0; return; }
        if (c == 0x10) { fm_scroll += 3; return; }
        if (c == '\n' && fm_selected >= 0) { fm_open_selected(); return; }
        if (c == 0x7F && fm_selected >= 0) { fm_do_delete(fm_selected); return; }
        if (c == 27) { fm_ctx_open = FALSE; fm_selected = -1; return; }
        return;
    }
    if (c == '\n') {
        fm_namebuf[fm_namelen] = '\0';
        if (fm_namelen > 0) {
            fs_result_t r;
            const char *msg;
            if (fm_renaming && fm_selected >= 0 && fs_files[fm_selected].used) {
                r = fs_rename(fs_files[fm_selected].name, fm_namebuf);
                if (r == FS_OK) { msg = "Renamed"; fm_selected = fs_find(fm_namebuf); }
                else if (r == FS_ERR_EXISTS) msg = "Name taken";
                else msg = "Invalid name";
            } else if (fm_mkdir_mode) {
                r = fs_mkdir(fm_namebuf);
                if (r == FS_OK) { msg = "Folder created"; fm_selected = fs_find(fm_namebuf); }
                else if (r == FS_ERR_EXISTS) msg = "Already exists";
                else if (r == FS_ERR_FULL) msg = "Table full";
                else msg = "Invalid name";
            } else {
                r = fs_create(fm_namebuf);
                if (r == FS_OK) { msg = "Created"; fm_selected = fs_find(fm_namebuf); }
                else if (r == FS_ERR_EXISTS) msg = "Already exists";
                else if (r == FS_ERR_FULL) msg = "File table full";
                else msg = "Invalid name";
            }
            fm_set_status(msg);
        }
        fm_naming = FALSE; fm_renaming = FALSE; fm_mkdir_mode = FALSE;
        return;
    }
    if (c == 27) { fm_naming = FALSE; fm_renaming = FALSE; fm_mkdir_mode = FALSE; return; }
    if (c == '\b') { if (fm_namelen > 0) { fm_namelen--; fm_namebuf[fm_namelen] = '\0'; } return; }
    if (fm_namelen < FS_NAME_MAX) {
        bool_t ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';
        if (ok) { fm_namebuf[fm_namelen++] = c; fm_namebuf[fm_namelen] = '\0'; }
    }
}

static void term_cmd_redraw_serial(void) {
    /* optional: keep serial in sync roughly */
}

static void term_set_cmd_from(const char *s) {
    u32 i = 0;
    while (s[i] && i < CMD_BUF_SIZE - 1) {
        cmd_buf[i] = s[i];
        i++;
    }
    cmd_buf[i] = 0;
    cmd_len = i;
    cmd_cursor = i;
}

static void terminal_handle_key(char c) {
    /* Special keys (from keyboard_callback) */
    if (c == 0x01) { /* Up — history previous */
        if (term_hist_count == 0) return;
        if (term_hist_browse < 0) {
            /* stash current */
            u32 i = 0;
            while (i < cmd_len && i < CMD_BUF_SIZE - 1) {
                term_hist_stash[i] = cmd_buf[i];
                i++;
            }
            term_hist_stash[i] = 0;
            term_hist_browse = (int)((term_hist_count - 1) % TERM_HIST_MAX);
        } else {
            u32 oldest = term_hist_count > TERM_HIST_MAX
                ? (term_hist_count - TERM_HIST_MAX) % TERM_HIST_MAX
                : 0;
            if ((u32)term_hist_browse == oldest) return; /* top of history */
            term_hist_browse = (term_hist_browse - 1 + TERM_HIST_MAX) % TERM_HIST_MAX;
            /* if we walked past valid entries */
            if (term_hist_count <= TERM_HIST_MAX && term_hist_browse > (int)(term_hist_count - 1))
                return;
        }
        term_set_cmd_from(term_hist[term_hist_browse]);
        return;
    }
    if (c == 0x02) { /* Down — history next */
        if (term_hist_browse < 0) return;
        u32 newest = (term_hist_count - 1) % TERM_HIST_MAX;
        if ((u32)term_hist_browse == newest) {
            term_hist_browse = -1;
            term_set_cmd_from(term_hist_stash);
            return;
        }
        term_hist_browse = (term_hist_browse + 1) % TERM_HIST_MAX;
        term_set_cmd_from(term_hist[term_hist_browse]);
        return;
    }
    if (c == 0x03) { /* Left */
        if (cmd_cursor > 0) cmd_cursor--;
        return;
    }
    if (c == 0x07) { /* Right */
        if (cmd_cursor < cmd_len) cmd_cursor++;
        return;
    }
    if (c == 0x0B) { /* Home */
        cmd_cursor = 0;
        return;
    }
    if (c == 0x0E) { /* End */
        cmd_cursor = cmd_len;
        return;
    }
    if (c == 0x0F) { /* Page Up — scrollback */
        term_scroll_by(8);
        return;
    }
    if (c == 0x10) { /* Page Down */
        term_scroll_by(-8);
        return;
    }
    if (c == 0x0C) { /* Ctrl+L — clear */
        term_clear_screen();
        term_print_attr(TERM_ATTR_INFO, "Terminal cleared.\n");
        return;
    }
    if (c == 0x18) { /* Ctrl+C — cancel line */
        cmd_len = 0;
        cmd_cursor = 0;
        cmd_buf[0] = 0;
        term_hist_browse = -1;
        term_print_attr(TERM_ATTR_WARN, "^C\n");
        return;
    }

    /* Tab — simple command completion */
    if (c == '\t') {
        static const char *cmds[] = {
            "help","clear","cls","echo","date","time","whoami","uname","version",
            "uptime","meminfo","free","sysinfo","neofetch","status","ls","ll","tree",
            "pwd","cd","mkdir","rmdir","touch","cat","head","wc","write","rm","cp","mv",
            "savenote","notepad","files","settings","about","calc","paint","applauncher",
            "taskmgr","theme","lock","sync","logout","audit","clip","cliphist","clipclear",
            "history","ifconfig","ping","arp","lspci","panic","reboot", 0
        };
        if (cmd_len == 0) return;
        const char *match = 0;
        int nmatch = 0;
        for (int i = 0; cmds[i]; i++) {
            u32 j = 0;
            while (j < cmd_len && cmds[i][j] && cmds[i][j] == cmd_buf[j]) j++;
            if (j == cmd_len) {
                nmatch++;
                match = cmds[i];
            }
        }
        if (nmatch == 1 && match) {
            term_set_cmd_from(match);
            if (cmd_len < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_len++] = ' ';
                cmd_buf[cmd_len] = 0;
                cmd_cursor = cmd_len;
            }
        } else if (nmatch > 1) {
            /* list matches into scrollback */
            char path[64];
            fs_cwd_path(path, sizeof(path));
            term_printf("titan:%s> %s\n", path, cmd_buf);
            term_print_attr(TERM_ATTR_DIM, "matches: ");
            for (int i = 0; cmds[i]; i++) {
                u32 j = 0;
                while (j < cmd_len && cmds[i][j] && cmds[i][j] == cmd_buf[j]) j++;
                if (j == cmd_len) {
                    term_printf("%s ", cmds[i]);
                }
            }
            term_putc('\n');
        }
        return;
    }

    if (c == '\n') {
        cmd_buf[cmd_len] = 0;
        /* echo the submitted line into scrollback with prompt */
        {
            char path[64];
            fs_cwd_path(path, sizeof(path));
            term_cur_attr = TERM_ATTR_PROMPT;
            term_printf("titan:%s> ", path);
            term_cur_attr = TERM_ATTR_NORMAL;
            term_printf("%s\n", cmd_buf);
        }
        if (cmd_len > 0) term_hist_push(cmd_buf);
        shell_execute(cmd_buf);
        cmd_len = 0;
        cmd_cursor = 0;
        cmd_buf[0] = 0;
        term_hist_browse = -1;
        shell_prompt();
        return;
    }

    if (c == '\b') {
        if (cmd_cursor == 0) return;
        /* delete char before cursor */
        for (u32 i = cmd_cursor - 1; i < cmd_len; i++)
            cmd_buf[i] = cmd_buf[i + 1];
        if (cmd_len > 0) cmd_len--;
        cmd_cursor--;
        cmd_buf[cmd_len] = 0;
        term_hist_browse = -1;
        return;
    }

    /* Delete key (scancode mapped to 0x7F) */
    if (c == 0x7F) {
        if (cmd_cursor >= cmd_len) return;
        for (u32 i = cmd_cursor; i < cmd_len; i++)
            cmd_buf[i] = cmd_buf[i + 1];
        if (cmd_len > 0) cmd_len--;
        cmd_buf[cmd_len] = 0;
        term_hist_browse = -1;
        return;
    }

    /* printable */
    if (c >= 32 && c < 127) {
        if (cmd_len >= CMD_BUF_SIZE - 1) return;
        /* insert at cursor */
        for (u32 i = cmd_len; i > cmd_cursor; i--)
            cmd_buf[i] = cmd_buf[i - 1];
        cmd_buf[cmd_cursor] = c;
        cmd_len++;
        cmd_cursor++;
        cmd_buf[cmd_len] = 0;
        term_hist_browse = -1;
        term_view = 0;
        (void)term_cmd_redraw_serial;
    }
}

/* ============================================================================
 *  GUI RENDERING -- glass theme
 * ==========================================================================*/
static u32 accent_color(void) {
    static const u32 accents[3] = { COL_ACCENT, COL_ACCENT2, RGB(120,255,170) };
    return accents[g_theme % 3];
}

static void desktop_render_raw(void) {
    static const u32 walls[][2] = {
        { RGB(20,22,46), RGB(38,74,110) },   /* midnight blue */
        { RGB(18,28,24), RGB(40,90,70) },    /* forest */
        { RGB(40,20,30), RGB(90,40,60) },    /* dusk rose */
        { RGB(15,15,20), RGB(45,45,55) },    /* graphite */
        { RGB(25,30,50), RGB(70,50,120) },   /* violet */
    };
    u32 top = walls[g_wallpaper % 5][0];
    u32 bot = walls[g_wallpaper % 5][1];
    for (u32 y = 0; y < fb_height; y++) {
        u32 t = (y * 255) / (fb_height ? fb_height : 1);
        u8 r = (u8)((RGB_R(top)*(255-t) + RGB_R(bot)*t) / 255);
        u8 g = (u8)((RGB_G(top)*(255-t) + RGB_G(bot)*t) / 255);
        u8 b = (u8)((RGB_B(top)*(255-t) + RGB_B(bot)*t) / 255);
        bb_hline(0, (int)y, (int)fb_width, RGB(r,g,b));
    }
    /* soft accent glow blob, top-right, purely decorative */
    u32 ac = accent_color();
    for (int r = 220; r > 0; r -= 10) {
        u8 alpha = (u8)(6 + (220-r)/12);
        bb_fill_rounded_glass((int)fb_width-260-r/2, -r/2, r, r, r/2, ac, alpha);
    }
}

/* The gradient + glow above is expensive (well over a million blended pixels)
 * and never changes frame-to-frame unless the accent theme changes -- so we
 * render it once into a cached buffer and just memcpy it back every redraw
 * instead of recomputing it. This is the single biggest performance win:
 * it turns "redo all that math every frame" into "one fast block copy". */
static u32   *desktop_cache = NULL;
static bool_t desktop_cache_valid = FALSE;
static u32    desktop_cache_theme = 0xFFFFFFFF;

static void draw_gradient_desktop(void) {
    if (!desktop_cache) desktop_cache = (u32*)kmalloc(fb_width * fb_height * 4);

    if (!desktop_cache) { desktop_render_raw(); return; } /* heap alloc failed: fall back */

    if (!desktop_cache_valid || desktop_cache_theme != (g_theme + g_wallpaper * 10)) {
        desktop_render_raw();                       /* pay the cost once ... */
        u32 *dst = desktop_cache, *src = backbuffer; /* ... then snapshot it */
        int count = (int)(fb_width * fb_height);
        __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
        desktop_cache_valid = TRUE;
        desktop_cache_theme = g_theme + g_wallpaper * 10;
        return; /* backbuffer already holds the freshly rendered desktop */
    }

    /* fast path: every other frame, just blit the cached desktop */
    u32 *dst = backbuffer, *src = desktop_cache;
    int count = (int)(fb_width * fb_height);
    __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}


static void system_shutdown(void) {
    debug_printf("[power] shutting down...\n");
    __asm__ volatile ("cli");

    /* Try every common emulator power-off port. NO reboot path. */
    outw(0x604,  0x2000); /* QEMU ACPI PM1a_CNT */
    outw(0xB004, 0x2000); /* Bochs / old QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */
    outw(0x1004, 0x2000); /* alternate ACPI */

    /* QEMU isa-debug-exit (if present) — exits emulator with code */
    outb(0x501, 0x00);

    /* If still alive: freeze forever (never reset / never reboot) */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static void draw_taskbar(void) {
    int th = 46;
    int ty = (int)fb_height - th;
    /* translucent dark glass bar over the desktop gradient already drawn */
    bb_fill_rect(0, ty, (int)fb_width, th, COL_TASKBAR);
    for (int i = 0; i < (int)fb_width; i++) bb_blend(i, ty, COL_TASKBAR, 235);
    bb_hline(0, ty, (int)fb_width, accent_color());       /* glowing top edge */
    for (int i = 1; i <= 3; i++) {
        u8 a = (u8)(60 - i*15);
        bb_hline(0, ty+i, (int)fb_width, accent_color()); /* fake glow falloff */
        (void)a;
    }

    /* TITAN start button */
    bool_t titan_hov = point_in(mouse_x, mouse_y, 4, ty + 4, 88, th - 8);
    if (titan_hov || start_menu_open)
        bb_fill_rounded_glass(4, ty + 4, 88, th - 8, 6, start_menu_open ? COL_BTNHOV : COL_BTN, 220);
    bb_printf(18, ty + th/2 - CHAR_H/2, accent_color(), "TITAN");

    /* Start menu — apps + power */
    if (start_menu_open) {
        static const char *sm_items[] = {
            "Apps", "Task Mgr", "Terminal", "Files", "Calc", "Paint", "Lock", "Shutdown"
        };
        const int sm_n = 8;
        int mw = 160, bh = 28, pad = 8;
        int mh = pad * 2 + sm_n * (bh + 4);
        int mx = 8, my = ty - mh - 8;
        bb_fill_rounded_glass(mx-2, my+4, mw+4, mh+4, 12, COL_BLACK, 70);
        bb_fill_rounded_glass(mx, my, mw, mh, 12, RGB(22,26,38), 250);
        bb_rounded_border(mx, my, mw, mh, 12, RGB(70,80,110));
        for (int i = 0; i < sm_n; i++) {
            int iy = my + pad + i * (bh + 4);
            bool_t hov = point_in(mouse_x, mouse_y, mx + pad, iy, mw - pad*2, bh);
            u32 bg = (i == sm_n - 1) ? (hov ? COL_CLOSEHOV : COL_CLOSE) : (hov ? COL_BTNHOV : COL_BTN);
            bb_fill_rounded_glass(mx + pad, iy, mw - pad*2, bh, 6, bg, 240);
            if (hov && i < sm_n - 1) bb_rounded_border(mx + pad, iy, mw - pad*2, bh, 6, accent_color());
            int tw = (int)k_strlen(sm_items[i]) * CHAR_W;
            bb_draw_string(mx + (mw - tw)/2, iy + (bh - CHAR_H)/2, sm_items[i], COL_WHITE);
        }
    }

    /* clock only */
    rtc_refresh_if_due();
    int clock_x = (int)fb_width - 128;
    bb_draw_string(clock_x, ty + 4, g_rtc_str, COL_TASKTEXT);
    bb_vline(clock_x - 12, ty + 6, th - 12, RGB(60,66,84));
}

/* ---- classic arrow-pointer cursor: anti-aliased via integer supersampling.
 *      (Freestanding kernel -- deliberately zero floating point, just
 *      integer subpixel sampling on a scaled-up polygon grid.) ---- */
typedef struct { int x, y; } pt2_t;
static const pt2_t cursor_poly[] = {
    {0,0},{0,25},{6,20},{11,30},{14,28},{9,18},{20,18}
};
#define CURSOR_POLY_N (int)(sizeof(cursor_poly)/sizeof(cursor_poly[0]))
#define CURSOR_W   20
#define CURSOR_H   30
#define CURSOR_SS  3                         /* 3x3 = 9 subsamples per pixel */
#define CURSOR_FULL (CURSOR_SS*CURSOR_SS)

static bool_t point_in_cursor_poly_ss(int px_ss, int py_ss) {
    bool_t inside = FALSE;
    int j = CURSOR_POLY_N - 1;
    for (int i = 0; i < CURSOR_POLY_N; i++) {
        int xi = cursor_poly[i].x * CURSOR_SS, yi = cursor_poly[i].y * CURSOR_SS;
        int xj = cursor_poly[j].x * CURSOR_SS, yj = cursor_poly[j].y * CURSOR_SS;
        if (((yi > py_ss) != (yj > py_ss)) &&
            (px_ss < (xj - xi) * (py_ss - yi) / (yj - yi) + xi))
            inside = !inside;
        j = i;
    }
    return inside;
}

static int cursor_cov[CURSOR_H][CURSOR_W];

static void cursor_rasterize(void) {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int hits = 0;
            for (int sy = 0; sy < CURSOR_SS; sy++)
                for (int sx = 0; sx < CURSOR_SS; sx++)
                    if (point_in_cursor_poly_ss(x*CURSOR_SS+sx, y*CURSOR_SS+sy)) hits++;
            cursor_cov[y][x] = hits;
        }
    }
}
static int cursor_cov_at(int x, int y) {
    if (x < 0 || y < 0 || x >= CURSOR_W || y >= CURSOR_H) return 0;
    return cursor_cov[y][x];
}

static void draw_cursor(int cx, int cy) {
    cursor_rasterize();

    /* soft drop shadow -- same silhouette, offset down-right, low alpha */
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int hits = cursor_cov[y][x];
            if (!hits) continue;
            u8 a = (u8)(hits * 90 / CURSOR_FULL);
            bb_blend(cx+x+2, cy+y+3, COL_BLACK, a);
        }
    }

    /* body: anti-aliased outer edge (partial coverage blends into the desktop),
       crisp white fill in the core, thin dark outline ring in between */
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int hits = cursor_cov[y][x];
            if (!hits) continue;
            if (hits < CURSOR_FULL) {
                u8 a = (u8)(hits * 255 / CURSOR_FULL);
                bb_blend(cx+x, cy+y, RGB(15,15,20), a);
                continue;
            }
            bool_t ring = cursor_cov_at(x-1,y) < CURSOR_FULL || cursor_cov_at(x+1,y) < CURSOR_FULL ||
                          cursor_cov_at(x,y-1) < CURSOR_FULL || cursor_cov_at(x,y+1) < CURSOR_FULL;
            bb_set(cx+x, cy+y, ring ? RGB(15,15,20) : COL_WHITE);
        }
    }

    /* tiny accent-colored highlight right at the hotspot tip */
    bb_blend(cx+2, cy+2, accent_color(), 200);
}

/* ============================================================================
 *  FILE MANAGER v2 -- modern sidebar + toolbar + grid/list + vector icons
 * ==========================================================================*/

static void fm_icon_folder(int x, int y, bool_t sel) {
    u32 tab = sel ? RGB(255, 210, 100) : RGB(240, 180, 60);
    u32 body = sel ? RGB(255, 230, 140) : RGB(230, 170, 50);
    u32 dark = RGB(120, 80, 20);
    bb_fill_rounded_glass(x + 2, y + 6, 18, 8, 3, tab, 250);
    bb_fill_rounded_glass(x, y + 12, 40, 28, 5, body, 250);
    bb_hline(x + 4, y + 16, 32, RGB(255, 245, 200));
    bb_hline(x + 2, y + 38, 36, dark);
}

static void fm_icon_file(int x, int y, bool_t sel) {
    u32 body = sel ? RGB(250, 252, 255) : RGB(220, 228, 240);
    u32 fold = sel ? RGB(180, 190, 210) : RGB(160, 170, 190);
    u32 line = sel ? RGB(80, 100, 140) : RGB(100, 120, 150);
    bb_fill_rounded_glass(x + 4, y + 4, 32, 40, 4, body, 250);
    for (int i = 0; i < 10; i++) {
        bb_hline(x + 26 + i, y + 4 + i, 10 - i, fold);
        bb_blend(x + 26 + i, y + 4 + i, RGB(20, 24, 32), 100);
    }
    for (int li = 0; li < 4; li++)
        bb_hline(x + 9, y + 16 + li * 6, li == 3 ? 14 : 22, line);
}

static void fm_icon_text(int x, int y, bool_t sel) {
    u32 body = sel ? RGB(255, 255, 255) : RGB(245, 248, 252);
    u32 accent = sel ? RGB(70, 140, 255) : RGB(90, 150, 230);
    bb_fill_rounded_glass(x + 4, y + 4, 32, 40, 4, body, 250);
    bb_fill_rect(x + 4, y + 4, 4, 40, accent);
    for (int li = 0; li < 5; li++)
        bb_hline(x + 12, y + 12 + li * 6, 18, RGB(140, 150, 170));
}

static void fm_icon_for(const fs_file_t *f, int x, int y, bool_t sel) {
    if (f->is_dir) { fm_icon_folder(x, y, sel); return; }
    const char *n = f->name;
    int len = 0; while (n[len]) len++;
    bool_t is_txt = FALSE;
    if (len > 2) {
        const char *e = n + len - 2;
        if ((e[0]=='.' && (e[1]=='c' || e[1]=='h' || e[1]=='C' || e[1]=='H')) ||
            (len > 4 && n[len-4]=='.' && n[len-3]=='t' && n[len-2]=='x' && n[len-1]=='t') ||
            (len > 3 && n[len-3]=='.' && n[len-2]=='m' && n[len-1]=='d'))
            is_txt = TRUE;
    }
    if (is_txt || f->size > 0) fm_icon_text(x, y, sel);
    else fm_icon_file(x, y, sel);
}

static void fm_draw_toolbar_btn(int x, int y, int w, int h, const char *label, bool_t hov, bool_t danger, bool_t enabled) {
    u32 bg;
    if (!enabled) bg = RGB(32, 36, 46);
    else if (danger) bg = hov ? COL_CLOSEHOV : COL_CLOSE;
    else bg = hov ? COL_BTNHOV : COL_BTN;
    bb_fill_rounded_glass(x, y, w, h, 6, bg, enabled ? 245 : 180);
    int tw = (int)k_strlen(label) * CHAR_W;
    bb_draw_string(x + (w - tw) / 2, y + (h - CHAR_H) / 2, label, enabled ? COL_WHITE : RGB(100, 105, 120));
}

static void draw_file_manager(gui_window_t *win, int x, int y, int w, int h, int tb) {
    (void)win;
    int cx0 = x + 6, cy0 = y + tb + 6;
    int cw = w - 12, ch = h - tb - 12;
    bb_fill_rounded_glass(cx0, cy0, cw, ch, 10, RGB(12, 14, 22), 250);

    int sb = FM_SIDEBAR_W;
    bb_fill_rounded_glass(cx0 + 4, cy0 + 4, sb - 4, ch - 8, 8, RGB(18, 22, 34), 250);
    bb_draw_string(cx0 + 14, cy0 + 14, "PLACES", RGB(120, 130, 150));
    static const char *places[] = { "Root", "home", "docs", "apps" };
    for (int pi = 0; pi < 4; pi++) {
        int iy = cy0 + 36 + pi * 32;
        int idx = (pi == 0) ? -1 : fm_find_named(places[pi]);
        bool_t active = (pi == 0 && fs_cwd < 0) || (idx >= 0 && fs_cwd == idx);
        bool_t hov = point_in(mouse_x, mouse_y, cx0 + 10, iy, sb - 20, 28);
        if (active) bb_fill_rounded_glass(cx0 + 10, iy, sb - 20, 28, 6, accent_color(), 200);
        else if (hov) bb_fill_rounded_glass(cx0 + 10, iy, sb - 20, 28, 6, RGB(36, 44, 64), 220);
        if (pi == 0) bb_fill_rect(cx0 + 18, iy + 8, 12, 12, active ? COL_WHITE : RGB(160, 170, 190));
        else bb_fill_rounded_glass(cx0 + 16, iy + 8, 14, 12, 2, active ? COL_WHITE : RGB(230, 170, 50), 240);
        bb_draw_string(cx0 + 36, iy + 8, places[pi], active ? COL_WHITE : RGB(200, 208, 220));
    }
    if (fm_clip_idx >= 0 && fs_files[fm_clip_idx].used) {
        bb_draw_string(cx0 + 14, cy0 + ch - 40, fm_clip_cut ? "Cut:" : "Copy:", RGB(120, 140, 160));
        bb_draw_string(cx0 + 14, cy0 + ch - 26, fs_files[fm_clip_idx].name, accent_color());
    }

    int mx0 = cx0 + sb + 4;
    int mw = cw - sb - 10;
    int my0 = cy0 + 4;
    int mh = ch - 8;
    int tbh = FM_TOOLBAR_H;
    bb_fill_rounded_glass(mx0, my0, mw, tbh, 7, RGB(22, 26, 38), 250);
    {
        const char *labs[] = { "Up", "New", "Folder", "Del", "Grid", "List", "Sort" };
        int bw = 54, gap = 6, bx = mx0 + 8;
        for (int bi = 0; bi < 7; bi++) {
            bool_t hov = point_in(mouse_x, mouse_y, bx, my0 + 5, bw, tbh - 10);
            bool_t en = TRUE;
            if (bi == 0) en = (fs_cwd >= 0);
            if (bi == 3) en = (fm_selected >= 0);
            if (bi == 4 && fm_view == 0) hov = TRUE;
            if (bi == 5 && fm_view == 1) hov = TRUE;
            fm_draw_toolbar_btn(bx, my0 + 5, bw, tbh - 10, labs[bi], hov, bi == 3, en);
            bx += bw + gap;
        }
    }

    int path_y = my0 + tbh + 4;
    bb_fill_rounded_glass(mx0, path_y, mw, FM_PATH_H, 6, RGB(16, 20, 30), 250);
    {
        char path[72];
        fs_cwd_path(path, sizeof(path));
        bb_printf(mx0 + 12, path_y + 7, accent_color(), "%s", path);
        int tmp[FS_MAX_FILES];
        int cnt = fm_collect(tmp, FS_MAX_FILES);
        bb_printf(mx0 + mw - 90, path_y + 7, RGB(120, 130, 150), "%d items", cnt);
    }

    int grid_y = path_y + FM_PATH_H + 4;
    int grid_h = mh - tbh - FM_PATH_H - FM_STATUS_H - 12;
    bb_fill_rounded_glass(mx0, grid_y, mw, grid_h, 8, RGB(14, 16, 24), 250);

    int items[FS_MAX_FILES];
    int nitems = fm_collect(items, FS_MAX_FILES);

    if (nitems == 0) {
        bb_draw_string(mx0 + mw / 2 - 80, grid_y + grid_h / 2 - 8, "This folder is empty", RGB(100, 110, 130));
        bb_draw_string(mx0 + mw / 2 - 100, grid_y + grid_h / 2 + 10, "Use New or Folder to create", RGB(80, 90, 110));
    } else if (fm_view == 0) {
        int cols = mw / (FM_CARD_W + FM_CARD_GAP);
        if (cols < 1) cols = 1;
        int rows_vis = grid_h / (FM_CARD_H + FM_CARD_GAP);
        if (rows_vis < 1) rows_vis = 1;
        int max_scroll = (nitems + cols - 1) / cols - rows_vis;
        if (max_scroll < 0) max_scroll = 0;
        if (fm_scroll > max_scroll) fm_scroll = max_scroll;
        if (fm_scroll < 0) fm_scroll = 0;
        for (int s = 0; s < nitems; s++) {
            int i = items[s];
            int col = s % cols, row = s / cols - fm_scroll;
            if (row < 0 || row >= rows_vis) continue;
            int cardx = mx0 + FM_CARD_GAP + col * (FM_CARD_W + FM_CARD_GAP);
            int cardy = grid_y + FM_CARD_GAP + row * (FM_CARD_H + FM_CARD_GAP);
            bool_t sel = (fm_selected == i);
            bool_t hov = point_in(mouse_x, mouse_y, cardx, cardy, FM_CARD_W, FM_CARD_H);
            if (sel) bb_fill_rounded_glass(cardx - 3, cardy - 3, FM_CARD_W + 6, FM_CARD_H + 6, 10, accent_color(), 100);
            else if (hov) bb_fill_rounded_glass(cardx - 2, cardy - 2, FM_CARD_W + 4, FM_CARD_H + 4, 9, RGB(40, 50, 70), 160);
            bb_fill_rounded_glass(cardx, cardy, FM_CARD_W, FM_CARD_H - 20, 8, RGB(24, 28, 40), 240);
            fm_icon_for(&fs_files[i], cardx + FM_CARD_W / 2 - 20, cardy + 8, sel);
            char label[16];
            u32 ln = k_strlen(fs_files[i].name);
            u32 maxc = 10;
            u32 c = 0;
            while (c < ln && c < maxc) { label[c] = fs_files[i].name[c]; c++; }
            if (ln > maxc) { if (maxc >= 3) { label[maxc-3]='.'; label[maxc-2]='.'; label[maxc-1]='.'; } c = maxc; }
            label[c] = 0;
            int lx = cardx + FM_CARD_W / 2 - ((int)k_strlen(label) * CHAR_W) / 2;
            bb_draw_string(lx, cardy + FM_CARD_H - 16, label, sel ? COL_WHITE : RGB(190, 198, 215));
        }
        if (max_scroll > 0) {
            int sh = grid_h - 8;
            int thumb = sh * rows_vis / ((nitems + cols - 1) / cols + 1);
            if (thumb < 16) thumb = 16;
            int ty = grid_y + 4 + (sh - thumb) * fm_scroll / (max_scroll ? max_scroll : 1);
            bb_fill_rounded_glass(mx0 + mw - 8, grid_y + 4, 5, sh, 2, RGB(30, 34, 48), 200);
            bb_fill_rounded_glass(mx0 + mw - 8, ty, 5, thumb, 2, accent_color(), 220);
        }
    } else {
        bb_fill_rect(mx0 + 4, grid_y + 2, mw - 8, 22, RGB(20, 24, 36));
        bb_draw_string(mx0 + 44, grid_y + 6, "Name", RGB(140, 150, 170));
        bb_draw_string(mx0 + mw - 160, grid_y + 6, "Size", RGB(140, 150, 170));
        bb_draw_string(mx0 + mw - 80, grid_y + 6, "Type", RGB(140, 150, 170));
        int rows_vis = (grid_h - 26) / FM_LIST_ROW;
        if (rows_vis < 1) rows_vis = 1;
        int max_scroll = nitems - rows_vis;
        if (max_scroll < 0) max_scroll = 0;
        if (fm_scroll > max_scroll) fm_scroll = max_scroll;
        if (fm_scroll < 0) fm_scroll = 0;
        for (int s = fm_scroll; s < nitems && s < fm_scroll + rows_vis; s++) {
            int i = items[s];
            int ry = grid_y + 26 + (s - fm_scroll) * FM_LIST_ROW;
            bool_t sel = (fm_selected == i);
            bool_t hov = point_in(mouse_x, mouse_y, mx0 + 4, ry, mw - 12, FM_LIST_ROW - 2);
            if (sel) bb_fill_rounded_glass(mx0 + 4, ry, mw - 12, FM_LIST_ROW - 2, 5, accent_color(), 120);
            else if (hov) bb_fill_rounded_glass(mx0 + 4, ry, mw - 12, FM_LIST_ROW - 2, 5, RGB(36, 44, 60), 180);
            if (fs_files[i].is_dir) bb_fill_rounded_glass(mx0 + 12, ry + 6, 16, 12, 2, RGB(230, 170, 50), 240);
            else bb_fill_rounded_glass(mx0 + 14, ry + 4, 12, 16, 2, RGB(220, 228, 240), 240);
            bb_draw_string(mx0 + 44, ry + 6, fs_files[i].name, sel ? COL_WHITE : RGB(200, 208, 220));
            if (fs_files[i].is_dir) bb_draw_string(mx0 + mw - 160, ry + 6, "-", RGB(120, 130, 150));
            else {
                char sz[16]; char tmp[16]; int ti = 0; u32 ssz = fs_files[i].size;
                if (ssz == 0) tmp[ti++] = '0';
                else while (ssz && ti < 14) { tmp[ti++] = (char)('0' + (ssz % 10)); ssz /= 10; }
                int k = 0; while (ti > 0) sz[k++] = tmp[--ti]; sz[k] = 0;
                bb_draw_string(mx0 + mw - 160, ry + 6, sz, RGB(150, 160, 180));
            }
            bb_draw_string(mx0 + mw - 80, ry + 6, fs_files[i].is_dir ? "Folder" : "File", RGB(140, 150, 170));
        }
        if (max_scroll > 0) {
            int sh = grid_h - 30;
            int thumb = sh * rows_vis / (nitems ? nitems : 1);
            if (thumb < 12) thumb = 12;
            int ty = grid_y + 26 + (sh - thumb) * fm_scroll / max_scroll;
            bb_fill_rounded_glass(mx0 + mw - 8, grid_y + 26, 5, sh, 2, RGB(30, 34, 48), 200);
            bb_fill_rounded_glass(mx0 + mw - 8, ty, 5, thumb, 2, accent_color(), 220);
        }
    }

    int sty = my0 + mh - FM_STATUS_H;
    bb_fill_rounded_glass(mx0, sty, mw, FM_STATUS_H, 5, RGB(18, 22, 32), 250);
    if (fm_status[0] && (timer_ticks - fm_status_tick) < PIT_HZ * 4)
        bb_draw_string(mx0 + 10, sty + 4, fm_status, RGB(140, 220, 160));
    else if (fm_selected >= 0 && fs_files[fm_selected].used) {
        if (fs_files[fm_selected].is_dir)
            bb_printf(mx0 + 10, sty + 4, RGB(150, 160, 180), "Folder: %s", fs_files[fm_selected].name);
        else
            bb_printf(mx0 + 10, sty + 4, RGB(150, 160, 180), "%s — %u bytes", fs_files[fm_selected].name, fs_files[fm_selected].size);
    } else bb_draw_string(mx0 + 10, sty + 4, "Ready", RGB(100, 110, 130));

    if (fm_naming) {
        int nw = 300, nh = 90;
        int nx = mx0 + (mw - nw) / 2, ny = grid_y + 40;
        bb_fill_rounded_glass(nx - 4, ny - 4, nw + 8, nh + 8, 10, COL_BLACK, 100);
        bb_fill_rounded_glass(nx, ny, nw, nh, 10, RGB(28, 34, 50), 250);
        const char *title = fm_renaming ? "Rename" : (fm_mkdir_mode ? "New Folder" : "New File");
        bb_draw_string(nx + 16, ny + 12, title, COL_TITLETEXT);
        bb_fill_rounded_glass(nx + 16, ny + 36, nw - 32, 28, 6, RGB(14, 18, 28), 250);
        bb_draw_string(nx + 24, ny + 42, fm_namebuf, COL_WHITE);
        if ((timer_ticks / 35) % 2 == 0)
            bb_fill_rect(nx + 24 + (int)fm_namelen * CHAR_W, ny + 42, 2, CHAR_H - 1, accent_color());
        bb_draw_string(nx + 16, ny + 70, "Enter=OK  Esc=Cancel", RGB(120, 130, 150));
    }

    if (fm_props_open && fm_selected >= 0 && fs_files[fm_selected].used) {
        int nw = 280, nh = 140;
        int nx = mx0 + (mw - nw) / 2, ny = grid_y + 30;
        bb_fill_rounded_glass(nx - 4, ny - 4, nw + 8, nh + 8, 10, COL_BLACK, 120);
        bb_fill_rounded_glass(nx, ny, nw, nh, 10, RGB(28, 34, 50), 250);
        bb_draw_string(nx + 16, ny + 12, "Properties", COL_TITLETEXT);
        bb_printf(nx + 16, ny + 40, RGB(200, 208, 220), "Name:  %s", fs_files[fm_selected].name);
        bb_printf(nx + 16, ny + 58, RGB(200, 208, 220), "Type:  %s", fs_files[fm_selected].is_dir ? "Folder" : "File");
        bb_printf(nx + 16, ny + 76, RGB(200, 208, 220), "Size:  %u bytes", fs_files[fm_selected].size);
        bb_printf(nx + 16, ny + 94, RGB(200, 208, 220), "Index: %d", fm_selected);
        bb_draw_string(nx + 16, ny + 118, "Click outside or Esc to close", RGB(120, 130, 150));
    }

    if (fm_ctx_open) {
        int menu_x = fm_ctx_x, menu_y = fm_ctx_y;
        int mw2 = FM_CTX_W, mh2 = FM_CTX_H, item_h = 26;
        if (menu_x + mw2 > (int)fb_width) menu_x = (int)fb_width - mw2 - 4;
        if (menu_y + mh2 > (int)fb_height) menu_y = (int)fb_height - mh2 - 4;
        bb_fill_rounded_glass(menu_x - 2, menu_y - 2, mw2 + 4, mh2 + 4, 8, COL_BLACK, 80);
        bb_fill_rounded_glass(menu_x, menu_y, mw2, mh2, 8, RGB(30, 36, 52), 250);
        static const char *citems[] = { "Open", "Copy", "Cut", "Paste", "Rename", "Delete", "Properties" };
        for (int ii = 0; ii < FM_CTX_ITEMS; ii++) {
            int iy = menu_y + 5 + ii * item_h;
            bool_t hov = point_in(mouse_x, mouse_y, menu_x + 4, iy, mw2 - 8, item_h - 2);
            bool_t en = TRUE;
            if (ii == 3 && fm_clip_idx < 0) en = FALSE;
            if (fm_ctx_file < 0 && ii != 3) en = FALSE;
            if (hov && en) bb_fill_rounded_glass(menu_x + 4, iy, mw2 - 8, item_h - 2, 5, accent_color(), 180);
            bb_draw_string(menu_x + 14, iy + 5, citems[ii], en ? COL_WHITE : RGB(90, 95, 110));
        }
    }
}

static void fm_open_selected(void) {
    if (fm_selected < 0 || !fs_files[fm_selected].used) return;
    if (fs_files[fm_selected].is_dir) {
        fs_cwd = fm_selected; fm_selected = -1; fm_scroll = 0; fm_set_status("Opened folder");
    } else {
        note_open_file(fm_selected); fm_set_status("Opened in Notepad");
    }
}

static void fm_do_delete(int idx) {
    if (idx < 0 || !fs_files[idx].used) return;
    char nbuf[FS_NAME_MAX + 1];
    u32 nlen = k_strlen(fs_files[idx].name);
    for (u32 c = 0; c < nlen; c++) nbuf[c] = fs_files[idx].name[c];
    nbuf[nlen] = 0;
    fs_result_t r = fs_delete(nbuf);
    if (r == FS_OK) {
        if (fm_selected == idx) fm_selected = -1;
        if (fm_clip_idx == idx) fm_clip_idx = -1;
        fm_set_status("Deleted");
    } else if (r == FS_ERR_NOTEMPTY) fm_set_status("Folder not empty");
    else fm_set_status("Delete failed");
}

static void fm_do_paste(void) {
    if (fm_clip_idx < 0 || !fs_files[fm_clip_idx].used) { fm_set_status("Clipboard empty"); return; }
    if (fs_files[fm_clip_idx].is_dir) { fm_set_status("Cannot paste folders"); return; }
    const char *src = fs_files[fm_clip_idx].name;
    char dst[FS_NAME_MAX + 1];
    u32 n = k_strlen(src);
    for (u32 i = 0; i < n && i < FS_NAME_MAX; i++) dst[i] = src[i];
    dst[n] = 0;
    if (fs_find_in(dst, fs_cwd) >= 0) {
        if (n + 5 < FS_NAME_MAX) {
            dst[n]='_'; dst[n+1]='c'; dst[n+2]='o'; dst[n+3]='p'; dst[n+4]='y'; dst[n+5]=0;
        }
    }
    fs_result_t r = fs_copy(src, dst);
    if (r == FS_OK) {
        if (fm_clip_cut) {
            char nbuf[FS_NAME_MAX + 1];
            for (u32 i = 0; i <= n; i++) nbuf[i] = src[i];
            fs_delete(nbuf);
            fm_clip_idx = -1; fm_clip_cut = FALSE; fm_set_status("Moved");
        } else fm_set_status("Copied");
        fm_selected = fs_find(dst);
    } else if (r == FS_ERR_EXISTS) fm_set_status("Already exists");
    else fm_set_status("Paste failed");
}

static bool_t fm_handle_click(gui_window_t *win, int mx, int my) {
    int x = win->x, y = win->y, w = win->w, h = win->h, tb = 26;
    int cx0 = x + 6, cy0 = y + tb + 6;
    int cw = w - 12, ch = h - tb - 12;
    int sb = FM_SIDEBAR_W;
    int mx0 = cx0 + sb + 4;
    int mw = cw - sb - 10;
    int my0 = cy0 + 4;
    int mh = ch - 8;
    int tbh = FM_TOOLBAR_H;
    int path_y = my0 + tbh + 4;
    int grid_y = path_y + FM_PATH_H + 4;
    int grid_h = mh - tbh - FM_PATH_H - FM_STATUS_H - 12;

    if (fm_props_open) { fm_props_open = FALSE; return TRUE; }

    if (fm_ctx_open) {
        int menu_x = fm_ctx_x, menu_y = fm_ctx_y;
        int mw2 = FM_CTX_W, mh2 = FM_CTX_H, item_h = 26;
        if (menu_x + mw2 > (int)fb_width) menu_x = (int)fb_width - mw2 - 4;
        if (menu_y + mh2 > (int)fb_height) menu_y = (int)fb_height - mh2 - 4;
        if (point_in(mx, my, menu_x, menu_y, mw2, mh2)) {
            int item = (my - menu_y - 5) / item_h;
            if (item < 0) item = 0;
            if (item >= FM_CTX_ITEMS) item = FM_CTX_ITEMS - 1;
            int fidx = fm_ctx_file;
            fm_ctx_open = FALSE;
            if (item == 0 && fidx >= 0) { fm_selected = fidx; fm_open_selected(); }
            else if (item == 1 && fidx >= 0) { fm_clip_idx = fidx; fm_clip_cut = FALSE; fm_set_status("Copied to clipboard"); }
            else if (item == 2 && fidx >= 0) { fm_clip_idx = fidx; fm_clip_cut = TRUE; fm_set_status("Cut to clipboard"); }
            else if (item == 3) fm_do_paste();
            else if (item == 4 && fidx >= 0) {
                fm_selected = fidx; fm_naming = TRUE; fm_renaming = TRUE; fm_mkdir_mode = FALSE;
                fm_namelen = k_strlen(fs_files[fidx].name);
                for (u32 c = 0; c < fm_namelen; c++) fm_namebuf[c] = fs_files[fidx].name[c];
                fm_namebuf[fm_namelen] = 0;
            } else if (item == 5 && fidx >= 0) fm_do_delete(fidx);
            else if (item == 6 && fidx >= 0) { fm_selected = fidx; fm_props_open = TRUE; }
            return TRUE;
        }
        fm_ctx_open = FALSE;
        return TRUE;
    }

    if (fm_naming) return TRUE;

    static const char *places[] = { "Root", "home", "docs", "apps" };
    for (int pi = 0; pi < 4; pi++) {
        int iy = cy0 + 36 + pi * 32;
        if (point_in(mx, my, cx0 + 10, iy, sb - 20, 28)) {
            if (pi == 0) fs_cwd = -1;
            else {
                int idx = fm_find_named(places[pi]);
                if (idx >= 0) fs_cwd = idx;
                else fm_set_status("Place not found");
            }
            fm_selected = -1; fm_scroll = 0;
            return TRUE;
        }
    }

    {
        int bw = 54, gap = 6, bx = mx0 + 8;
        for (int bi = 0; bi < 7; bi++) {
            if (point_in(mx, my, bx, my0 + 5, bw, tbh - 10)) {
                if (bi == 0 && fs_cwd >= 0) { fs_cwd = fs_files[fs_cwd].parent; fm_selected = -1; fm_scroll = 0; }
                else if (bi == 1) { fm_naming = TRUE; fm_renaming = FALSE; fm_mkdir_mode = FALSE; fm_namelen = 0; fm_namebuf[0] = 0; }
                else if (bi == 2) { fm_naming = TRUE; fm_renaming = FALSE; fm_mkdir_mode = TRUE; fm_namelen = 0; fm_namebuf[0] = 0; }
                else if (bi == 3 && fm_selected >= 0) fm_do_delete(fm_selected);
                else if (bi == 4) { fm_view = 0; fm_scroll = 0; }
                else if (bi == 5) { fm_view = 1; fm_scroll = 0; }
                else if (bi == 6) {
                    fm_sort = (fm_sort + 1) % 3;
                    fm_set_status(fm_sort == 0 ? "Sort: Name" : fm_sort == 1 ? "Sort: Size" : "Sort: Type");
                }
                return TRUE;
            }
            bx += bw + gap;
        }
    }

    int items[FS_MAX_FILES];
    int nitems = fm_collect(items, FS_MAX_FILES);
    if (fm_view == 0) {
        int cols = mw / (FM_CARD_W + FM_CARD_GAP);
        if (cols < 1) cols = 1;
        int rows_vis = grid_h / (FM_CARD_H + FM_CARD_GAP);
        if (rows_vis < 1) rows_vis = 1;
        for (int s = 0; s < nitems; s++) {
            int i = items[s];
            int col = s % cols, row = s / cols - fm_scroll;
            if (row < 0 || row >= rows_vis) continue;
            int cardx = mx0 + FM_CARD_GAP + col * (FM_CARD_W + FM_CARD_GAP);
            int cardy = grid_y + FM_CARD_GAP + row * (FM_CARD_H + FM_CARD_GAP);
            if (point_in(mx, my, cardx, cardy, FM_CARD_W, FM_CARD_H)) {
                bool_t is_dbl = (i == fm_last_click_idx && (timer_ticks - fm_last_click_tick) < 35);
                fm_last_click_idx = i; fm_last_click_tick = timer_ticks; fm_selected = i;
                if (is_dbl) fm_open_selected();
                return TRUE;
            }
        }
    } else {
        int rows_vis = (grid_h - 26) / FM_LIST_ROW;
        if (rows_vis < 1) rows_vis = 1;
        for (int s = fm_scroll; s < nitems && s < fm_scroll + rows_vis; s++) {
            int i = items[s];
            int ry = grid_y + 26 + (s - fm_scroll) * FM_LIST_ROW;
            if (point_in(mx, my, mx0 + 4, ry, mw - 12, FM_LIST_ROW - 2)) {
                bool_t is_dbl = (i == fm_last_click_idx && (timer_ticks - fm_last_click_tick) < 35);
                fm_last_click_idx = i; fm_last_click_tick = timer_ticks; fm_selected = i;
                if (is_dbl) fm_open_selected();
                return TRUE;
            }
        }
    }
    if (point_in(mx, my, mx0, grid_y, mw, grid_h)) { fm_selected = -1; return TRUE; }
    return FALSE;
}

static bool_t fm_handle_right_click(gui_window_t *win, int mx, int my) {
    int x = win->x, y = win->y, w = win->w, h = win->h, tb = 26;
    int cx0 = x + 6, cy0 = y + tb + 6;
    int cw = w - 12, ch = h - tb - 12;
    int sb = FM_SIDEBAR_W;
    int mx0 = cx0 + sb + 4;
    int mw = cw - sb - 10;
    int my0 = cy0 + 4;
    int mh = ch - 8;
    int tbh = FM_TOOLBAR_H;
    int path_y = my0 + tbh + 4;
    int grid_y = path_y + FM_PATH_H + 4;
    int grid_h = mh - tbh - FM_PATH_H - FM_STATUS_H - 12;
    if (fm_naming || fm_props_open) return TRUE;
    int items[FS_MAX_FILES];
    int nitems = fm_collect(items, FS_MAX_FILES);
    int hit = -1;
    if (fm_view == 0) {
        int cols = mw / (FM_CARD_W + FM_CARD_GAP);
        if (cols < 1) cols = 1;
        int rows_vis = grid_h / (FM_CARD_H + FM_CARD_GAP);
        if (rows_vis < 1) rows_vis = 1;
        for (int s = 0; s < nitems; s++) {
            int i = items[s];
            int col = s % cols, row = s / cols - fm_scroll;
            if (row < 0 || row >= rows_vis) continue;
            int cardx = mx0 + FM_CARD_GAP + col * (FM_CARD_W + FM_CARD_GAP);
            int cardy = grid_y + FM_CARD_GAP + row * (FM_CARD_H + FM_CARD_GAP);
            if (point_in(mx, my, cardx, cardy, FM_CARD_W, FM_CARD_H)) { hit = i; break; }
        }
    } else {
        int rows_vis = (grid_h - 26) / FM_LIST_ROW;
        if (rows_vis < 1) rows_vis = 1;
        for (int s = fm_scroll; s < nitems && s < fm_scroll + rows_vis; s++) {
            int i = items[s];
            int ry = grid_y + 26 + (s - fm_scroll) * FM_LIST_ROW;
            if (point_in(mx, my, mx0 + 4, ry, mw - 12, FM_LIST_ROW - 2)) { hit = i; break; }
        }
    }
    if (hit >= 0) fm_selected = hit;
    fm_ctx_file = hit; fm_ctx_x = mx; fm_ctx_y = my; fm_ctx_open = TRUE;
    return TRUE;
}



static void draw_window(gui_window_t *win) {
    int x=win->x, y=win->y, w=win->w, h=win->h, tb=26, radius=7;

    /* soft shadow (single blurred layer -- two overlapping layers cost twice
       the per-pixel blending for a difference too subtle to be worth it) */
    bb_fill_rounded_glass(x-3, y+8, w+6, h+8, radius+3, COL_BLACK, 55);

    /* frosted glass body: blends with whatever desktop is already drawn beneath */
    bb_fill_rounded_glass(x, y, w, h, radius, COL_GLASS_TINT, 190);

    /* title bar: brighter glass strip with accent-tinted bottom edge */
    bb_fill_rounded_glass(x, y, w, tb+radius, radius, RGB(30,34,52), 215);
    bb_hline(x+radius, y+tb, w-2*radius, accent_color());

    bb_rounded_border(x, y, w, h, radius, RGB(255,255,255));
    for (int i = x+radius; i < x+w-radius; i++) bb_blend(i, y, COL_WHITE, 40); /* top highlight */

    {
        /* title clipped so it never runs into close button */
        char tbuf[28];
        int maxc = (w - 78) / CHAR_W;
        if (maxc < 4) maxc = 4;
        if (maxc > 27) maxc = 27;
        int n = 0;
        while (win->title[n] && n < maxc) { tbuf[n] = win->title[n]; n++; }
        tbuf[n] = 0;
        bb_printf(x+14, y+tb/2-CHAR_H/2, COL_TITLETEXT, "%s", tbuf);
    }

    if (win->has_close) {
        int cby=y+6, cbs=16;
        /* maximize / restore — left of close */
        int mbx=x+w-52;
        bool_t mhov = point_in(mouse_x, mouse_y, mbx-3, cby-3, cbs+6, cbs+6);
        bb_fill_rounded_glass(mbx, cby, cbs, cbs, 5, mhov ? COL_BTNHOV : COL_BTN, 235);
        if (win->maximized) {
            /* restore: small overlapping squares */
            bb_rounded_border(mbx+4, cby+4, 7, 7, 1, COL_WHITE);
            bb_rounded_border(mbx+6, cby+6, 7, 7, 1, COL_WHITE);
        } else {
            /* maximize: empty square */
            bb_rounded_border(mbx+3, cby+3, 10, 10, 2, COL_WHITE);
        }
        /* close */
        int cbx=x+w-30;
        bool_t hov = point_in(mouse_x, mouse_y, cbx-3, cby-3, cbs+6, cbs+6);
        bb_fill_rounded_glass(cbx, cby, cbs, cbs, 5, hov ? COL_CLOSEHOV : COL_CLOSE, 235);
        bb_draw_string(cbx+3, cby, "X", COL_WHITE);
    }

    if (win->is_terminal) {
        int cx0 = x + 8, cy0 = y + tb + 10, cw = w - 16, ch = h - tb - 18;
        bb_fill_rounded_glass(cx0, cy0, cw, ch, 8, COL_TERMBG, 235);

        /* how many text rows fit */
        int pad = 8;
        int footer_h = CHAR_H + 6;
        int body_h = ch - pad * 2 - footer_h - 4;
        int rows_fit = body_h / CHAR_H;
        if (rows_fit < 3) rows_fit = 3;
        if (rows_fit > TERM_VIEW_ROWS) rows_fit = TERM_VIEW_ROWS;
        int cols_fit = (cw - pad * 2) / CHAR_W;
        if (cols_fit > TERM_COLS) cols_fit = TERM_COLS;
        if (cols_fit < 20) cols_fit = 20;

        /* color for attr */
        /* draw scrollback: bottom of view is newest (minus term_view) */
        int incomplete = (term_linecol > 0) ? 1 : 0;
        int start_from_bottom = term_view + rows_fit - 1;
        /* start_from_bottom is offset of top line of viewport from bottom */

        for (int row = 0; row < rows_fit; row++) {
            int from_bottom = start_from_bottom - row;
            int ty = cy0 + pad + row * CHAR_H;
            if (from_bottom < 0) continue;

            if (from_bottom == 0 && incomplete) {
                /* current incomplete output line (rare — mid printf) */
                u32 col = COL_TERMTEXT;
                bb_draw_string(cx0 + pad, ty, term_linebuf, col);
                continue;
            }
            int adj = incomplete ? from_bottom - 1 : from_bottom;
            if (adj < 0 || adj >= (int)term_sb_len) continue;
            int idx = term_sb_index(adj);
            if (idx < 0) continue;
            u8 attr = term_sb_attr[idx];
            u32 col = COL_TERMTEXT;
            if (attr == TERM_ATTR_DIM) col = COL_TERMDIM;
            else if (attr == TERM_ATTR_PROMPT) col = accent_color();
            else if (attr == TERM_ATTR_ERR) col = RGB(255, 100, 100);
            else if (attr == TERM_ATTR_OK) col = RGB(100, 230, 140);
            else if (attr == TERM_ATTR_INFO) col = RGB(120, 180, 255);
            else if (attr == TERM_ATTR_WARN) col = RGB(255, 200, 80);
            else if (adj > 0) col = COL_TERMDIM; /* older lines slightly dim */
            /* clip to cols_fit */
            char clip[TERM_COLS + 1];
            u32 n = 0;
            while (term_sb[idx][n] && n < (u32)cols_fit) {
                clip[n] = term_sb[idx][n];
                n++;
            }
            clip[n] = 0;
            bb_draw_string(cx0 + pad, ty, clip, col);
        }

        /* Input line (prompt + cmd) pinned to bottom of terminal body */
        int input_y = cy0 + ch - footer_h - 2;
        /* separator */
        bb_fill_rect(cx0 + 4, input_y - 4, cw - 8, 1, RGB(30, 40, 50));

        char path[64];
        fs_cwd_path(path, sizeof(path));
        char prompt[80];
        /* build "titan:path> " */
        {
            const char *pfx = "titan:";
            u32 pi = 0;
            while (pfx[pi] && pi < 70) { prompt[pi] = pfx[pi]; pi++; }
            u32 j = 0;
            while (path[j] && pi < 70) { prompt[pi++] = path[j++]; }
            if (pi < 70) prompt[pi++] = '>';
            if (pi < 70) prompt[pi++] = ' ';
            prompt[pi] = 0;
        }
        u32 plen = k_strlen(prompt);
        bb_draw_string(cx0 + pad, input_y, prompt, accent_color());

        /* command text + blinking caret at cmd_cursor */
        int cmd_x = cx0 + pad + (int)plen * CHAR_W;
        bb_draw_string(cmd_x, input_y, cmd_buf, COL_TERMTEXT);
        if ((timer_ticks / 35) % 2 == 0 && focused_win == WIN_TERMINAL) {
            int caret_x = cmd_x + (int)cmd_cursor * CHAR_W;
            bb_fill_rect(caret_x, input_y, 2, CHAR_H - 1, accent_color());
        }

        /* scroll indicator */
        if (term_view > 0)
            bb_draw_string(cx0 + cw - 10 * CHAR_W, cy0 + 4, "[scroll]", RGB(90, 110, 130));
    } else if (win->is_about) {
        int cx0=x+16, cy0=y+tb+16;
        bb_draw_string(cx0, cy0,             "TITAN KERNEL v3", COL_TITLETEXT);
        bb_draw_string(cx0, cy0+CHAR_H*2,     "GLASS EDITION", accent_color());
        bb_draw_string(cx0, cy0+CHAR_H*4,     "OWN GDT / IDT / PIC / PIT", RGB(200,205,215));
        bb_draw_string(cx0, cy0+CHAR_H*5,     "OWN PS2 MOUSE + KEYBOARD", RGB(200,205,215));
        bb_draw_string(cx0, cy0+CHAR_H*6,     "DOUBLE-BUFFERED RENDERER", RGB(200,205,215));
        bb_draw_string(cx0, cy0+CHAR_H*8,     "A HOBBY KERNEL, NOT A", RGB(255,150,150));
        bb_draw_string(cx0, cy0+CHAR_H*9,     "PRODUCTION OS -- BUT REAL", RGB(255,150,150));
    } else if (win->is_notepad) {
        int menu_h = 24;
        int cx0=x+8, cy0=y+tb+8, cw=w-16, ch=h-tb-16;
        /* menu bar */
        bb_fill_rect(cx0, cy0, cw, menu_h, RGB(236,236,240));
        bb_hline(cx0, cy0+menu_h-1, cw, RGB(180,180,190));
        bool_t file_hov = point_in(mouse_x, mouse_y, cx0+4, cy0+2, 48, 20);
        if (file_hov || note_menu_open)
            bb_fill_rect(cx0+4, cy0+2, 48, 20, RGB(200,210,230));
        bb_draw_string(cx0+10, cy0+5, "File", RGB(30,32,40));
        /* title shows filename */
        if (note_filename[0])
            bb_printf(cx0 + 70, cy0+5, RGB(80,84,100), "- %s", note_filename);
        else
            bb_draw_string(cx0 + 70, cy0+5, "- Untitled", RGB(120,124,140));

        /* text area */
        int tx0 = cx0, ty0 = cy0 + menu_h, th = ch - menu_h;
        bb_fill_rect(tx0, ty0, cw, th, RGB(252,252,254));
        for (u32 i = 0; i < NOTE_ROWS; i++) {
            bb_draw_string(tx0+10, ty0+6 + (int)i*CHAR_H, note_lines[i], RGB(30,32,40));
        }
        if (focused_win == WIN_NOTEPAD && !note_menu_open && !note_saveas && !note_rename &&
            (timer_ticks / 40) % 2 == 0) {
            int blink_x = tx0+10 + (int)note_cur_col * CHAR_W;
            int blink_y = ty0+6 + (int)note_cur_row * CHAR_H;
            bb_fill_rect(blink_x, blink_y, 2, CHAR_H-2, RGB(40,100,200));
        }

        /* File dropdown */
        if (note_menu_open) {
            int dw = 120, dh = 120, dx = cx0+4, dy = cy0+menu_h;
            bb_fill_rounded_glass(dx-2, dy+2, dw+4, dh+4, 4, COL_BLACK, 70);
            bb_fill_rect(dx, dy, dw, dh, RGB(248,248,250));
            bb_rounded_border(dx, dy, dw, dh, 4, RGB(160,160,170));
            const char *items[] = { "New", "Open...", "Save", "Save As...", "Rename" };
            for (int ii = 0; ii < 5; ii++) {
                int iy = dy + 4 + ii * 22;
                bool_t hov = point_in(mouse_x, mouse_y, dx+2, iy, dw-4, 20);
                if (hov) bb_fill_rect(dx+2, iy, dw-4, 20, RGB(200,210,235));
                bb_draw_string(dx+12, iy+3, items[ii], RGB(30,32,40));
            }
        }

        /* Save As / Rename dialog */
        if (note_saveas || note_rename) {
            int dw = 280, dh = 70;
            int dx = x + w/2 - dw/2, dy = y + h/2 - dh/2;
            bb_fill_rounded_glass(dx-3, dy+4, dw+6, dh+6, 8, COL_BLACK, 90);
            bb_fill_rounded_glass(dx, dy, dw, dh, 8, RGB(40,44,58), 250);
            bb_rounded_border(dx, dy, dw, dh, 8, accent_color());
            bb_draw_string(dx+14, dy+10, note_rename ? "Rename file:" : "Save as filename:", COL_TITLETEXT);
            bb_fill_rect(dx+14, dy+32, dw-28, 22, RGB(12,14,20));
            bb_draw_string(dx+18, dy+36, note_namebuf, COL_WHITE);
            if ((timer_ticks/30)%2==0)
                bb_fill_rect(dx+18+(int)note_namelen*CHAR_W, dy+36, 2, 14, accent_color());
        }

    } else if (win->is_settings) {
        int cx0 = x + 8, cy0 = y + tb + 8, cw = w - 16, ch = h - tb - 16;
        int side = 140;
        bb_fill_rounded_glass(cx0, cy0, cw, ch, 8, RGB(16,18,28), 245);
        /* sidebar */
        bb_fill_rect(cx0, cy0, side, ch, RGB(22,26,40));
        static const char *tabs[] = { "Lock", "Wallpaper", "Security", "Account", "System" };
        for (int i = 0; i < 5; i++) {
            int ty = cy0 + 16 + i * 40;
            bool_t on = (settings_tab == i);
            bool_t hov = point_in(mouse_x, mouse_y, cx0 + 8, ty, side - 16, 32);
            if (on || hov)
                bb_fill_rounded_glass(cx0 + 8, ty, side - 16, 32, 8,
                    on ? accent_color() : RGB(40,48,70), 230);
            bb_draw_string(cx0 + 20, ty + 8, tabs[i], on ? COL_WHITE : COL_TASKTEXT);
        }
        /* content */
        int ox = cx0 + side + 20, oy = cy0 + 20;
        (void)cw; /* width available for content */
        if (settings_tab == 0) {
            bb_draw_string(ox, oy, "Lock screen", COL_TITLETEXT);
            bb_draw_string(ox, oy + 24, "Lock now or set idle timeout.", RGB(160,170,190));
            bool_t hov = point_in(mouse_x, mouse_y, ox, oy + 52, 140, 32);
            bb_fill_rounded_glass(ox, oy + 52, 140, 32, 8, hov ? COL_BTNHOV : COL_BTN, 240);
            bb_draw_string(ox + 28, oy + 60, "Lock now", COL_WHITE);
            bb_draw_string(ox, oy + 100, "Idle timeout:", RGB(160,170,190));
            static const u32 tsec[] = { 30, 60, 120, 300 };
            static const char *tl[] = { "30s", "1m", "2m", "5m" };
            for (int i = 0; i < 4; i++) {
                int bx = ox + i * 90;
                int by = oy + 128;
                bool_t on = (g_idle_timeout_sec == tsec[i]);
                (void)point_in(mouse_x, mouse_y, bx, by, 80, 28); /* hover reserved */
                bb_fill_rounded_glass(bx, by, 80, 28, 6, on ? accent_color() : RGB(36,42,60), 235);
                bb_draw_string(bx + 22, by + 6, tl[i], COL_WHITE);
            }
            bb_draw_string(ox, oy + 170, "Shorter = safer if you walk away", RGB(120,130,150));
        } else if (settings_tab == 1) {
            bb_draw_string(ox, oy, "Wallpaper", COL_TITLETEXT);
            bb_draw_string(ox, oy + 28, "Choose a desktop gradient.", RGB(160,170,190));
            static const char *wn[] = { "Midnight", "Forest", "Dusk", "Graphite", "Violet" };
            for (int i = 0; i < 5; i++) {
                int bx = ox + (i % 3) * 140;
                int by = oy + 60 + (i / 3) * 70;
                bool_t on = (g_wallpaper % 5 == (u32)i);
                bool_t hov = point_in(mouse_x, mouse_y, bx, by, 120, 56);
                bb_fill_rounded_glass(bx, by, 120, 56, 10, on ? accent_color() : RGB(36,42,60), 235);
                if (hov || on) bb_rounded_border(bx, by, 120, 56, 10, COL_WHITE);
                bb_draw_string(bx + 16, by + 20, wn[i], COL_WHITE);
            }
        } else if (settings_tab == 2) {
            bb_draw_string(ox, oy, "Security", COL_TITLETEXT);
            bb_draw_string(ox, oy + 28, "Offline by design. Raise local safeguards.", RGB(160,170,190));
            static const char *lv[] = { "Basic", "Standard", "High" };
            for (int i = 0; i < 3; i++) {
                int bx = ox + i * 130;
                int by = oy + 70;
                bool_t on = (g_security_level == (u32)i);
                bool_t hov = point_in(mouse_x, mouse_y, bx, by, 110, 40);
                bb_fill_rounded_glass(bx, by, 110, 40, 10, on ? RGB(40,120,80) : RGB(36,42,60), 235);
                if (hov || on) bb_rounded_border(bx, by, 110, 40, 10, accent_color());
                bb_draw_string(bx + 20, by + 12, lv[i], COL_WHITE);
            }
            bb_draw_string(ox, oy + 140, "Disk: AES-128-CTR + AES auth tag", RGB(120,130,150));
            bb_draw_string(ox, oy + 158, "KDF: 65536 AES rounds + salt", RGB(120,130,150));
            bb_draw_string(ox, oy + 176, "High: 3 fails -> 15s+ lockout", RGB(120,130,150));
            bb_draw_string(ox, oy + 194, "No network stack (online surface=0)", RGB(120,130,150));
            if (g_security_level >= 2)
                bb_draw_string(ox, oy + 220, "Status: HARDENED", RGB(100,220,140));
            else
                bb_draw_string(ox, oy + 220, "Status: ACTIVE", RGB(200,200,120));
        } else if (settings_tab == 3) {
            /* Account / password change */
            bb_draw_string(ox, oy, "Account", COL_TITLETEXT);
            bb_draw_string(ox, oy + 24, "User: titan", RGB(160,170,190));
            bb_draw_string(ox, oy + 48, "Old password", RGB(160,170,190));
            bb_fill_rounded_glass(ox, oy + 68, 220, 28, 6, settings_pw_focus==0 ? RGB(40,48,70) : RGB(18,20,28), 240);
            {
                char b[32];
                u32 n = settings_old_len;
                if (n > 20) n = 20;
                for (u32 i = 0; i < n; i++) b[i] = '*';
                b[n] = 0;
                bb_draw_string(ox + 10, oy + 74, b, COL_WHITE);
            }
            bb_draw_string(ox, oy + 108, "New password", RGB(160,170,190));
            bb_fill_rounded_glass(ox, oy + 128, 220, 28, 6, settings_pw_focus==1 ? RGB(40,48,70) : RGB(18,20,28), 240);
            {
                char b[32];
                u32 n = settings_new_len;
                if (n > 20) n = 20;
                for (u32 i = 0; i < n; i++) b[i] = '*';
                b[n] = 0;
                bb_draw_string(ox + 10, oy + 134, b, COL_WHITE);
            }
            bool_t hov = point_in(mouse_x, mouse_y, ox, oy + 172, 160, 32);
            bb_fill_rounded_glass(ox, oy + 172, 160, 32, 8, hov ? COL_BTNHOV : COL_BTN, 240);
            bb_draw_string(ox + 20, oy + 180, "Change password", COL_WHITE);
            if (settings_status[0])
                bb_draw_string(ox, oy + 220, settings_status, RGB(100,220,140));
            bb_draw_string(ox, oy + 250, "Tab=switch  Enter=apply", RGB(120,130,150));
        } else {
            bb_draw_string(ox, oy, "System", COL_TITLETEXT);
            bb_draw_string(ox, oy + 36, "Titan Kernel v3 Glass", COL_TASKTEXT);
            bb_draw_string(ox, oy + 56, "Offline-first · AES volume · Lock screen", RGB(140,150,170));
            bool_t hov = point_in(mouse_x, mouse_y, ox, oy + 100, 180, 36);
            bb_fill_rounded_glass(ox, oy + 100, 180, 36, 10, hov ? COL_BTNHOV : COL_BTN, 240);
            bb_draw_string(ox + 24, oy + 110, "Cycle accent", COL_WHITE);
        }
    } else if (win->is_files) {
        draw_file_manager(win, x, y, w, h, tb);
    } else if (win->is_calc) {
        int cx0 = x + 6, cy0 = y + tb + 6, cw = w - 12, ch = h - tb - 12;
        calc_draw(cx0, cy0, cw, ch, mouse_x, mouse_y,
                  (void (*)(int,int,int,int,calc_u32))bb_fill_rect,
                  (void (*)(int,int,const char*,calc_u32))bb_draw_string,
                  RGB(20,22,32), RGB(40,46,64), COL_WHITE,
                  accent_color(), RGB(12,14,22));
    } else if (win->is_paint) {
        int cx0 = x + 6, cy0 = y + tb + 6, cw = w - 12, ch = h - tb - 12;
        paint_draw(cx0, cy0, cw, ch, mouse_x, mouse_y, mouse_left,
                   (void (*)(int,int,int,int,paint_u32))bb_fill_rect,
                   (void (*)(int,int,const char*,paint_u32))bb_draw_string,
                   RGB(20,22,32), RGB(40,46,64), COL_WHITE, accent_color());
    } else if (win->is_launcher) {
        int cx0 = x + 6, cy0 = y + tb + 6, cw = w - 12, ch = h - tb - 12;
        al_draw(cx0, cy0, cw, ch, mouse_x, mouse_y,
                (void (*)(int,int,int,int,al_u32))bb_fill_rect,
                (void (*)(int,int,const char*,al_u32))bb_draw_string,
                RGB(18,20,30), RGB(32,38,54), RGB(50,70,120),
                COL_WHITE, accent_color());
    } else if (win->is_taskmgr) {
        int cx0 = x + 6, cy0 = y + tb + 6, cw = w - 12, ch = h - tb - 12;
        tm_draw(cx0, cy0, cw, ch, mouse_x, mouse_y,
                (void (*)(int,int,int,int,tm_u32))bb_fill_rect,
                (void (*)(int,int,const char*,tm_u32))bb_draw_string,
                RGB(18,20,30), RGB(32,38,54), RGB(50,70,120),
                COL_WHITE, accent_color(), RGB(180,50,50));
    }
}


/* ---- Lock screen (full-screen, no taskbar) ---- */
static void bb_draw_string_scaled(int x, int y, const char *s, u32 fg, int scale) {
    for (int i = 0; s[i]; i++) {
        const u8 *g = font_lookup(s[i]);
        for (int row = 0; row < 7; row++) {
            u8 bits = g[row];
            for (int col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col))) {
                    bb_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
        x += 6 * scale;
    }
}

static void draw_lock_screen(void) {
    /* full gradient background only */
    for (u32 y = 0; y < fb_height; y++) {
        u32 t = (y * 255) / (fb_height ? fb_height : 1);
        u8 r = (u8)((RGB_R(COL_SKY_TOP)*(255-t) + RGB_R(COL_SKY_BOT)*t) / 255);
        u8 g = (u8)((RGB_G(COL_SKY_TOP)*(255-t) + RGB_G(COL_SKY_BOT)*t) / 255);
        u8 b = (u8)((RGB_B(COL_SKY_TOP)*(255-t) + RGB_B(COL_SKY_BOT)*t) / 255);
        bb_hline(0, (int)y, (int)fb_width, RGB(r,g,b));
    }
    /* soft center glow */
    u32 ac = accent_color();
    for (int rad = 180; rad > 0; rad -= 12) {
        u8 a = (u8)(4 + (180 - rad) / 20);
        bb_fill_rounded_glass((int)fb_width/2 - rad, (int)fb_height/2 - rad - 40,
                              rad*2, rad*2, rad, ac, a);
    }

    char tbuf[8], dbuf[24];
    lock_format_time(tbuf, sizeof(tbuf));
    lock_format_date(dbuf, sizeof(dbuf));

    /* large centered time */
    int scale = 8;
    int tw = (int)k_strlen(tbuf) * 6 * scale;
    int th = 7 * scale;
    int tx = ((int)fb_width - tw) / 2;
    int ty = (int)fb_height / 2 - 120;
    bb_draw_string_scaled(tx, ty, tbuf, COL_WHITE, scale);

    /* date under time */
    int dw = (int)k_strlen(dbuf) * CHAR_W;
    bb_draw_string(((int)fb_width - dw) / 2, ty + th + 24, dbuf, RGB(220,225,235));

    if (!lock_login_visible()) {
        const char *hint = "Click anywhere to unlock";
        int hw = (int)k_strlen(hint) * CHAR_W;
        bb_draw_string(((int)fb_width - hw) / 2, (int)fb_height - 80, hint, RGB(160,170,190));
    } else {
        /* fixed centered login card — not movable */
        int cw = 360, ch = 200;
        int cx = ((int)fb_width - cw) / 2;
        int cy = (int)fb_height / 2 + 40;
        bb_fill_rounded_glass(cx-3, cy+4, cw+6, ch+6, 12, COL_BLACK, 90);
        bb_fill_rounded_glass(cx, cy, cw, ch, 12, RGB(28,32,44), 250);
        bb_rounded_border(cx, cy, cw, ch, 12, accent_color());

        bb_draw_string(cx + 24, cy + 18, "Sign in", COL_TITLETEXT);

        /* username */
        bb_draw_string(cx + 24, cy + 50, "Username", RGB(160,170,190));
        u32 ubg = (lock_focus() == 0) ? RGB(40,48,70) : RGB(18,20,28);
        bb_fill_rounded_glass(cx + 24, cy + 68, cw - 48, 28, 6, ubg, 240);
        if (lock_focus() == 0) bb_rounded_border(cx + 24, cy + 68, cw - 48, 28, 6, accent_color());
        bb_draw_string(cx + 34, cy + 74, lock_username(), COL_WHITE);
        if (lock_focus() == 0 && (timer_ticks / 30) % 2 == 0)
            bb_fill_rect(cx + 34 + (int)lock_user_len() * CHAR_W, cy + 74, 2, CHAR_H, accent_color());

        /* password */
        bb_draw_string(cx + 24, cy + 108, "Password", RGB(160,170,190));
        u32 pbg = (lock_focus() == 1) ? RGB(40,48,70) : RGB(18,20,28);
        bb_fill_rounded_glass(cx + 24, cy + 126, cw - 48, 28, 6, pbg, 240);
        if (lock_focus() == 1) bb_rounded_border(cx + 24, cy + 126, cw - 48, 28, 6, accent_color());
        /* bullets */
        char bullets[LOCK_PASS_MAX + 1];
        u32 pl = lock_pass_len();
        for (u32 i = 0; i < pl; i++) bullets[i] = '*';
        bullets[pl] = 0;
        bb_draw_string(cx + 34, cy + 132, bullets, COL_WHITE);
        if (lock_focus() == 1 && (timer_ticks / 30) % 2 == 0)
            bb_fill_rect(cx + 34 + (int)pl * CHAR_W, cy + 132, 2, CHAR_H, accent_color());

        if (lock_is_locked_out()) {
            char lo[40];
            u32 sec = lock_lockout_seconds_left();
            /* LOCKED OUT Ns */
            lo[0]='L';lo[1]='O';lo[2]='C';lo[3]='K';lo[4]='E';lo[5]='D';lo[6]=' ';
            lo[7]='O';lo[8]='U';lo[9]='T';lo[10]=' ';
            if (sec > 99) sec = 99;
            lo[11] = (char)('0' + (sec / 10));
            lo[12] = (char)('0' + (sec % 10));
            lo[13] = 's'; lo[14] = 0;
            bb_draw_string(cx + 24, cy + 168, lo, COL_CLOSEHOV);
        } else if (lock_has_error()) {
            bb_draw_string(cx + 24, cy + 168, "Wrong password - try again", COL_CLOSEHOV);
            if (lock_fail_count() > 0) {
                char fc[24];
                u32 f = lock_fail_count();
                fc[0]='F';fc[1]='a';fc[2]='i';fc[3]='l';fc[4]='s';fc[5]=':';fc[6]=' ';
                fc[7] = (char)('0' + (f % 10));
                fc[8] = 0;
                bb_draw_string(cx + 24, cy + 168 + CHAR_H, fc, RGB(200,120,120));
            }
        } else
            bb_draw_string(cx + 24, cy + 168, "Enter / Tab to switch fields", RGB(120,130,150));
    }

    draw_cursor(mouse_x, mouse_y);
    fb_present();
}



static void desk_open_app(int i); /* fwd */
static void desk_unique_name(char *out, int out_cap, const char *base, bool_t is_dir) {
    /* base, base2, base3... until free in cwd */
    if (out_cap < 8) return;
    int n = 0;
    while (n < 100) {
        int pos = 0;
        for (int i = 0; base[i] && pos < out_cap - 4; i++) out[pos++] = base[i];
        if (n > 0) {
            if (n >= 10) out[pos++] = (char)('0' + (n / 10));
            out[pos++] = (char)('0' + (n % 10));
        }
        out[pos] = 0;
        if (fs_find(out) < 0) return;
        n++;
    }
    out[0] = 0;
    (void)is_dir;
}

static void desk_ctx_run(int item) {
    char name[32];
    if (item == 0) {
        /* Refresh */
        desktop_cache_valid = FALSE;
    } else if (item == 1) {
        desk_unique_name(name, sizeof(name), "NewFolder", TRUE);
        if (name[0]) fs_mkdir(name);
        windows[WIN_FILES].visible = TRUE;
        bring_to_front(WIN_FILES);
        focused_win = WIN_FILES;
    } else if (item == 2) {
        desk_unique_name(name, sizeof(name), "NewFile", FALSE);
        if (name[0]) {
            fs_create(name);
            /* open empty in notepad */
            int idx = fs_find(name);
            if (idx >= 0) note_open_file(idx);
        }
    } else if (item == 3) {
        desk_open_app(0); /* Terminal */
    } else if (item == 4) {
        desk_open_app(2); /* Files */
    } else if (item == 5) {
        desk_open_app(1); /* Notepad */
    }
    desk_ctx_open = FALSE;
}

static void draw_desk_context_menu(void) {
    if (!desk_ctx_open) return;
    int mx = desk_ctx_x, my = desk_ctx_y;
    int mw = DESK_CTX_W, mh = DESK_CTX_H;
    int pad = 6, ih = DESK_CTX_ITEM_H;
    if (mx + mw > (int)fb_width) mx = (int)fb_width - mw - 8;
    if (my + mh > (int)fb_height - 52) my = (int)fb_height - mh - 52;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;
    bb_fill_rounded_glass(mx-2, my+4, mw+4, mh+4, 10, COL_BLACK, 70);
    bb_fill_rounded_glass(mx, my, mw, mh, 10, RGB(22,26,38), 250);
    bb_rounded_border(mx, my, mw, mh, 10, RGB(70,80,110));
    static const char *items[DESK_CTX_N] = {
        "Refresh", "New Folder", "New File", "Terminal", "Files", "Notepad"
    };
    for (int i = 0; i < DESK_CTX_N; i++) {
        int iy = my + pad + i * ih;
        int row_w = mw - pad * 2;
        bool_t hov = point_in(mouse_x, mouse_y, mx + pad, iy, row_w, ih - 2);
        if (hov) {
            bb_fill_rounded_glass(mx + pad, iy, row_w, ih - 2, 6, RGB(50, 70, 120), 240);
            bb_rounded_border(mx + pad, iy, row_w, ih - 2, 6, accent_color());
        }
        /* text vertically centered inside row, left pad so it stays in border */
        int ty = iy + (ih - 2 - CHAR_H) / 2;
        int max_chars = (row_w - 16) / CHAR_W;
        char line[24];
        int n = 0;
        while (items[i][n] && n < max_chars && n < 23) { line[n] = items[i][n]; n++; }
        line[n] = 0;
        bb_draw_string(mx + pad + 10, ty, line, hov ? COL_WHITE : COL_TASKTEXT);
    }
}

/* Desktop icons — simple glyph + label (no big square card) */
#define DESK_ICON_N 5
#define DESK_ICON_S 44
#define DESK_ICON_GAP 16
static void desk_icon_rect(int i, int *ox, int *oy, int *ow, int *oh) {
    int x = 28;
    int y = 36 + i * (DESK_ICON_S + DESK_ICON_GAP + 16);
    *ox = x; *oy = y; *ow = 80; *oh = DESK_ICON_S + 18;
}

static void draw_desk_icon_glyph(int kind, int x, int y, bool_t hov) {
    int s = DESK_ICON_S;
    u32 body = hov ? accent_color() : RGB(48, 54, 72);
    bb_fill_rounded_glass(x, y, s, s, 10, body, 230);
    if (hov) bb_rounded_border(x, y, s, s, 10, COL_WHITE);
    if (kind == 0) {
        bb_fill_rounded_glass(x+7, y+9, s-14, s-18, 4, RGB(10,14,18), 240);
        bb_draw_string(x+10, y+16, ">_", RGB(80,220,120));
    } else if (kind == 1) {
        bb_fill_rounded_glass(x+11, y+7, 22, 28, 3, RGB(245,245,250), 240);
        bb_hline(x+15, y+14, 14, RGB(120,130,150));
        bb_hline(x+15, y+20, 14, RGB(120,130,150));
        bb_hline(x+15, y+26, 10, RGB(120,130,150));
    } else if (kind == 2) {
        bb_fill_rounded_glass(x+7, y+14, 30, 20, 3, RGB(230,180,50), 240);
        bb_fill_rect(x+7, y+10, 12, 7, RGB(240,200,80));
    } else if (kind == 3) {
        bb_fill_rounded_glass(x+11, y+8, 22, 26, 11, RGB(90,150,255), 230);
        bb_draw_string(x+17, y+16, "i", COL_WHITE);
    } else {
        /* Settings gear-ish */
        bb_fill_rounded_glass(x+10, y+10, 24, 24, 12, RGB(180,190,210), 230);
        bb_fill_rounded_glass(x+16, y+16, 12, 12, 6, RGB(40,44,58), 240);
    }
}

static void draw_desktop_icons(void) {
    static const char *labels[DESK_ICON_N] = { "Terminal", "Notepad", "Files", "About", "Settings" };
    for (int i = 0; i < DESK_ICON_N; i++) {
        int ix, iy, iw, ih;
        desk_icon_rect(i, &ix, &iy, &iw, &ih);
        bool_t hov = point_in(mouse_x, mouse_y, ix, iy, iw, ih);
        int gx = ix + (iw - DESK_ICON_S) / 2;
        draw_desk_icon_glyph(i, gx, iy, hov);
        int n = 0; char line[12];
        while (labels[i][n] && n < 10) { line[n] = labels[i][n]; n++; }
        line[n] = 0;
        int lw = n * CHAR_W;
        int lx = ix + (iw - lw) / 2;
        if (lx < ix) lx = ix;
        bb_draw_string(lx, iy + DESK_ICON_S + 4, line, hov ? COL_WHITE : RGB(200,210,225));
    }
}


static void launcher_open_app(int id) {
    if (id == 0) { windows[WIN_TERMINAL].visible = TRUE; bring_to_front(WIN_TERMINAL); focused_win = WIN_TERMINAL; }
    else if (id == 1) { windows[WIN_NOTEPAD].visible = TRUE; bring_to_front(WIN_NOTEPAD); focused_win = WIN_NOTEPAD; }
    else if (id == 2) { windows[WIN_FILES].visible = TRUE; bring_to_front(WIN_FILES); focused_win = WIN_FILES; }
    else if (id == 3) { windows[WIN_ABOUT].visible = TRUE; bring_to_front(WIN_ABOUT); }
    else if (id == 4) { windows[WIN_SETTINGS].visible = TRUE; bring_to_front(WIN_SETTINGS); focused_win = WIN_SETTINGS; }
    else if (id == 5) { calc_open(); windows[WIN_CALC].visible = TRUE; bring_to_front(WIN_CALC); focused_win = WIN_CALC; }
    else if (id == 6) { paint_open(); windows[WIN_PAINT].visible = TRUE; bring_to_front(WIN_PAINT); focused_win = WIN_PAINT; }
    else if (id == 7) {
        tm_clear();
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (windows[i].visible && i != WIN_TASKMGR)
                tm_add(windows[i].title, i);
        }
        tm_open();
        windows[WIN_TASKMGR].visible = TRUE;
        bring_to_front(WIN_TASKMGR);
        focused_win = WIN_TASKMGR;
    }
}

static void desk_open_app(int i) {
    int win = -1;
    if (i == 0) win = WIN_TERMINAL;
    else if (i == 1) win = WIN_NOTEPAD;
    else if (i == 2) win = WIN_FILES;
    else if (i == 3) win = WIN_ABOUT;
    else if (i == 4) win = WIN_SETTINGS;
    if (win < 0) return;
    windows[win].visible = TRUE;
    bring_to_front(win);
    if (win == WIN_TERMINAL || win == WIN_NOTEPAD || win == WIN_FILES || win == WIN_SETTINGS)
        focused_win = win;
}

static void gui_redraw(void) {
    draw_gradient_desktop();
    draw_desktop_icons();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        int idx = win_order[i];
        if (windows[idx].visible) draw_window(&windows[idx]);
    }
    draw_taskbar();
    draw_desk_context_menu();
    if (cv_is_open()) {
        char tbuf[16], dbuf[24];
        lock_format_time(tbuf, sizeof(tbuf));
        lock_format_date(dbuf, sizeof(dbuf));
        cv_tick(timer_ticks);
        cv_draw((int)fb_width, (int)fb_height, tbuf, dbuf, mouse_x, mouse_y,
                (void (*)(int,int,int,int,cv_u32))bb_fill_rect,
                (void (*)(int,int,int,int,int,cv_u32,cv_u32))bb_fill_rounded_glass,
                (void (*)(int,int,const char*,cv_u32))bb_draw_string,
                (void (*)(int,int,const char*,cv_u32,int))bb_draw_string_scaled,
                accent_color());
    }
    toast_draw((int)fb_width, (int)fb_height, timer_ticks,
               (void (*)(int,int,int,int,toast_u32))bb_fill_rect,
               (void (*)(int,int,const char*,toast_u32))bb_draw_string,
               RGB(28,32,44), COL_WHITE, accent_color());
    draw_cursor(mouse_x, mouse_y);
    fb_present();
}

/* ============================================================================
 *  INPUT HANDLING
 * ==========================================================================*/
static bool_t note_handle_click(gui_window_t *win, int mx, int my) {
    int x = win->x, y = win->y, tb = 26;
    (void)win->w;
    int menu_h = 24;
    int cx0 = x + 8, cy0 = y + tb + 8;

    /* Save As / Rename dialog steals clicks */
    if (note_saveas || note_rename) return TRUE;

    /* File menu button */
    if (point_in(mx, my, cx0+4, cy0+2, 48, 20)) {
        note_menu_open = !note_menu_open;
        return TRUE;
    }

    /* Dropdown items */
    if (note_menu_open) {
        int dw = 120, dh = 120, dx = cx0+4, dy = cy0+menu_h;
        if (point_in(mx, my, dx, dy, dw, dh)) {
            int item = (my - dy - 4) / 22;
            if (item < 0) item = 0;
            if (item > 4) item = 4;
            note_menu_open = FALSE;
            if (item == 0) {
                /* New */
                note_clear();
            } else if (item == 1) {
                /* Open -> File Manager pick mode */
                fm_pick_mode = TRUE;
                windows[WIN_FILES].visible = TRUE;
                bring_to_front(WIN_FILES);
                focused_win = WIN_FILES;
                fm_selected = -1;
                {
                    u32 i=0; const char *msg="Pick a file, then Open";
                    while (msg[i] && i < sizeof(fm_status)-1) { fm_status[i]=msg[i]; i++; }
                    fm_status[i]=0;
                }
            } else if (item == 2) {
                /* Save */
                if (note_file_idx >= 0 && fs_files[note_file_idx].used)
                    note_save_to_idx(note_file_idx);
                else {
                    /* no name yet -> Save As */
                    note_saveas = TRUE;
                    note_namelen = 0; note_namebuf[0]=0;
                }
            } else if (item == 3) {
                /* Save As */
                note_saveas = TRUE;
                note_namelen = 0; note_namebuf[0]=0;
                if (note_filename[0]) {
                    note_namelen = k_strlen(note_filename);
                    for (u32 i = 0; i < note_namelen; i++)
                        note_namebuf[i] = note_filename[i];
                    note_namebuf[note_namelen] = 0;
                }
            } else if (item == 4) {
                /* Rename */
                if (note_file_idx >= 0 && note_filename[0]) {
                    note_rename = TRUE;
                    note_namelen = k_strlen(note_filename);
                    for (u32 i = 0; i < note_namelen; i++)
                        note_namebuf[i] = note_filename[i];
                    note_namebuf[note_namelen] = 0;
                }
            }
            return TRUE;
        }
        /* click outside closes menu */
        note_menu_open = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void note_handle_key(char c) {
    if (!(note_saveas || note_rename)) return;
    if (c == '\n') {
        note_namebuf[note_namelen] = 0;
        if (note_namelen == 0) { note_saveas = FALSE; note_rename = FALSE; return; }
        if (note_rename && note_file_idx >= 0) {
            fs_result_t r = fs_rename(fs_files[note_file_idx].name, note_namebuf);
            if (r == FS_OK) {
                u32 n = k_strlen(note_namebuf);
                for (u32 i = 0; i < n; i++)
                    note_filename[i] = note_namebuf[i];
                note_filename[n] = 0;
            }
            note_rename = FALSE;
            return;
        }
        if (note_saveas) {
            /* create or overwrite in cwd */
            int idx = fs_find(note_namebuf);
            if (idx < 0) {
                fs_result_t r = fs_create(note_namebuf);
                if (r != FS_OK) { note_saveas = FALSE; return; }
                idx = fs_find(note_namebuf);
            }
            if (idx >= 0 && !fs_files[idx].is_dir) {
                note_save_to_idx(idx);
                note_file_idx = idx;
                u32 n = k_strlen(note_namebuf);
                for (u32 i = 0; i < n; i++)
                    note_filename[i] = note_namebuf[i];
                note_filename[n] = 0;
            }
            note_saveas = FALSE;
        }
        return;
    }
    if (c == 27) { note_saveas = FALSE; note_rename = FALSE; return; }
    if (c == '\b') {
        if (note_namelen > 0) { note_namelen--; note_namebuf[note_namelen]=0; }
        return;
    }
    if (note_namelen < FS_NAME_MAX) {
        bool_t ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';
        if (ok) { note_namebuf[note_namelen++]=c; note_namebuf[note_namelen]=0; }
    }
}


static void settings_handle_key(char c) {
    if (settings_tab != 3) return;
    if (c == '\t') { settings_pw_focus = 1 - settings_pw_focus; return; }
    if (c == '\n') {
        settings_old[settings_old_len] = 0;
        settings_new[settings_new_len] = 0;
        if (lock_change_password(settings_old, settings_new)) {
            u32 n = 0;
            while (settings_new[n] && n < LOCK_PASS_MAX) { persist_pw[n] = settings_new[n]; n++; }
            persist_pw[n] = 0;
            persist_pw_set = TRUE;
            settings_persist_all(persist_pw);
            const char *ok = "Password saved to disk";
            u32 i=0; while (ok[i] && i<39) { settings_status[i]=ok[i]; i++; }
            settings_status[i]=0;
            settings_old_len = settings_new_len = 0;
            settings_old[0] = settings_new[0] = 0;
        } else {
            const char *bad = "Old password wrong";
            u32 i=0; while (bad[i] && i<39) { settings_status[i]=bad[i]; i++; }
            settings_status[i]=0;
        }
        return;
    }
    if (c == '\b') {
        if (settings_pw_focus == 0 && settings_old_len > 0) {
            settings_old_len--; settings_old[settings_old_len]=0;
        } else if (settings_pw_focus == 1 && settings_new_len > 0) {
            settings_new_len--; settings_new[settings_new_len]=0;
        }
        return;
    }
    if (c >= 32 && c < 127) {
        if (settings_pw_focus == 0 && settings_old_len < LOCK_PASS_MAX) {
            settings_old[settings_old_len++]=c; settings_old[settings_old_len]=0;
        } else if (settings_pw_focus == 1 && settings_new_len < LOCK_PASS_MAX) {
            settings_new[settings_new_len++]=c; settings_new[settings_new_len]=0;
        }
    }
}

static bool_t settings_handle_click(gui_window_t *win, int mx, int my) {
    int x=win->x, y=win->y, w=win->w, h=win->h, tb=26;
    int cx0 = x + 8, cy0 = y + tb + 8, cw = w - 16, ch = h - tb - 16;
    int side = 140;
    for (int i = 0; i < 5; i++) {
        int ty = cy0 + 16 + i * 40;
        if (point_in(mx, my, cx0 + 8, ty, side - 16, 32)) {
            settings_tab = i;
            settings_status[0] = 0;
            return TRUE;
        }
    }
    int ox = cx0 + side + 20, oy = cy0 + 20;
    if (settings_tab == 0) {
        if (point_in(mx, my, ox, oy + 52, 140, 32)) {
            if (persist_pw_set) settings_persist_all(persist_pw);
            session_secure_lock();
            return TRUE;
        }
        static const u32 tsec[] = { 30, 60, 120, 300 };
        for (int i = 0; i < 4; i++) {
            int bx = ox + i * 90;
            int by = oy + 128;
            if (point_in(mx, my, bx, by, 80, 28)) {
                g_idle_timeout_sec = tsec[i];
                if (persist_pw_set) settings_persist_all(persist_pw);
                return TRUE;
            }
        }
    } else if (settings_tab == 1) {
        for (int i = 0; i < 5; i++) {
            int bx = ox + (i % 3) * 140;
            int by = oy + 60 + (i / 3) * 70;
            if (point_in(mx, my, bx, by, 120, 56)) {
                g_wallpaper = (u32)i;
                desktop_cache_valid = FALSE;
                if (persist_pw_set) settings_persist_all(persist_pw);
                return TRUE;
            }
        }
    } else if (settings_tab == 2) {
        for (int i = 0; i < 3; i++) {
            int bx = ox + i * 130;
            int by = oy + 70;
            if (point_in(mx, my, bx, by, 110, 40)) {
                g_security_level = (u32)i;
                lock_set_security_level(g_security_level);
                if (persist_pw_set) settings_persist_all(persist_pw);
                return TRUE;
            }
        }
    } else if (settings_tab == 3) {
        if (point_in(mx, my, ox, oy + 68, 220, 28)) { settings_pw_focus = 0; return TRUE; }
        if (point_in(mx, my, ox, oy + 128, 220, 28)) { settings_pw_focus = 1; return TRUE; }
        if (point_in(mx, my, ox, oy + 172, 160, 32)) {
            settings_old[settings_old_len] = 0;
            settings_new[settings_new_len] = 0;
            if (lock_change_password(settings_old, settings_new)) {
                u32 n = 0;
                while (settings_new[n] && n < LOCK_PASS_MAX) { persist_pw[n] = settings_new[n]; n++; }
                persist_pw[n] = 0;
                persist_pw_set = TRUE;
                settings_persist_all(persist_pw);
                audit_add("AUTH: password changed");
                {
                    const char *ok = "Password saved to disk";
                    u32 i=0; while (ok[i] && i<39) { settings_status[i]=ok[i]; i++; }
                    settings_status[i]=0;
                }
                settings_old_len = settings_new_len = 0;
                settings_old[0] = settings_new[0] = 0;
            } else {
                const char *bad = "Old password wrong";
                u32 i=0; while (bad[i] && i<39) { settings_status[i]=bad[i]; i++; }
                settings_status[i]=0;
            }
            return TRUE;
        }
    } else if (settings_tab == 4) {
        if (point_in(mx, my, ox, oy + 100, 180, 36)) {
            g_theme = (g_theme + 1) % 3;
            desktop_cache_valid = FALSE;
            return TRUE;
        }
    }
    (void)ch; (void)cw;
    return FALSE;
}

static void window_toggle_maximize(gui_window_t *w) {
    int th = 46;
    if (!w->maximized) {
        w->rest_x = w->x; w->rest_y = w->y; w->rest_w = w->w; w->rest_h = w->h;
        w->x = 0; w->y = 0;
        w->w = (int)fb_width;
        w->h = (int)fb_height - th;
        w->maximized = TRUE;
    } else {
        w->x = w->rest_x; w->y = w->rest_y;
        w->w = w->rest_w; w->h = w->rest_h;
        w->maximized = FALSE;
    }
}
static void handle_mouse_click(int mx, int my) {
    int th = 46, ty = (int)fb_height - th;

    /* Desktop context menu click */
    if (desk_ctx_open) {
        int mx0 = desk_ctx_x, my0 = desk_ctx_y;
        int mw = DESK_CTX_W, mh = DESK_CTX_H;
        if (mx0 + mw > (int)fb_width) mx0 = (int)fb_width - mw - 4;
        if (my0 + mh > (int)fb_height - 50) my0 = (int)fb_height - mh - 50;
        if (point_in(mx, my, mx0, my0, mw, mh)) {
            int item = (my - my0 - 6) / DESK_CTX_ITEM_H;
            if (item < 0) item = 0;
            if (item >= DESK_CTX_N) item = DESK_CTX_N - 1;
            desk_ctx_run(item);
            return;
        }
        desk_ctx_open = FALSE;
        /* fall through */
    }

    /* Start menu items */
    if (start_menu_open) {
        const int sm_n = 8;
        int mw = 160, bh = 28, pad = 8;
        int mh = pad * 2 + sm_n * (bh + 4);
        int smx = 8, smy = ty - mh - 8;
        for (int i = 0; i < sm_n; i++) {
            int iy = smy + pad + i * (bh + 4);
            if (!point_in(mx, my, smx + pad, iy, mw - pad*2, bh)) continue;
            start_menu_open = FALSE;
            if (i == 0) { /* Apps */
                al_open(); windows[WIN_LAUNCHER].visible = TRUE;
                bring_to_front(WIN_LAUNCHER); focused_win = WIN_LAUNCHER;
            } else if (i == 1) { /* Task Mgr */
                request_taskmgr = TRUE;
            } else if (i == 2) desk_open_app(0);
            else if (i == 3) desk_open_app(2);
            else if (i == 4) {
                calc_open(); windows[WIN_CALC].visible = TRUE;
                bring_to_front(WIN_CALC); focused_win = WIN_CALC;
            } else if (i == 5) {
                paint_open(); windows[WIN_PAINT].visible = TRUE;
                bring_to_front(WIN_PAINT); focused_win = WIN_PAINT;
            } else if (i == 6) {
                if (persist_pw_set) settings_persist_all(persist_pw);
                session_secure_lock();
            } else if (i == 7) {
                if (persist_pw_set) settings_persist_all(persist_pw);
                persist_pw_clear(); system_shutdown();
            }
            return;
        }
        if (!point_in(mx, my, 4, ty + 4, 88, th - 8) &&
            !point_in(mx, my, smx, smy, mw, mh))
            start_menu_open = FALSE;
        else if (point_in(mx, my, 4, ty + 4, 88, th - 8)) {
            start_menu_open = FALSE; return;
        }
    }
    if (point_in(mx, my, 4, ty + 4, 88, th - 8)) {
        start_menu_open = TRUE; return;
    }
    /* clock -> full-screen modern clock view */
    {
        int clock_x = (int)fb_width - 128;
        if (point_in(mx, my, clock_x, ty, 120, th)) {
            if (cv_is_open()) cv_close();
            else cv_open();
            return;
        }
    }

    /* Desktop icons (only if not clicking a window — checked after windows if none hit) */
    /* handled below after window loop if no window consumed click */

    for (int i = MAX_WINDOWS-1; i >= 0; i--) {
        int idx = win_order[i];
        gui_window_t *w = &windows[idx];
        if (!w->visible) continue;
        if (!point_in(mx, my, w->x, w->y, w->w, w->h)) continue;
        if (w->has_close && point_in(mx, my, w->x+w->w-33, w->y+3, 22, 22)) {
            w->visible = FALSE;
            if (idx == WIN_LAUNCHER) al_close();
            if (idx == WIN_TASKMGR) tm_close();
            if (focused_win == idx) focused_win = WIN_TERMINAL;
            return;
        }
        /* maximize / restore button (left of close) */
        if (w->has_close && point_in(mx, my, w->x+w->w-55, w->y+3, 22, 22)) {
            window_toggle_maximize(w);
            bring_to_front(idx);
            return;
        }
        focused_win = idx;
        if (idx == WIN_NOTEPAD) {
            if (note_handle_click(w, mx, my)) { bring_to_front(idx); return; }
        }
        if (idx == WIN_SETTINGS) {
            if (settings_handle_click(w, mx, my)) { bring_to_front(idx); return; }
        }
        if (idx == WIN_FILES && !fm_naming) {
            if (fm_handle_click(w, mx, my)) { bring_to_front(idx); return; }
        }
        if (idx == WIN_CALC) {
            int tb = 26;
            if (calc_click(w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12, mx, my)) {
                bring_to_front(idx); return;
            }
        }
        if (idx == WIN_PAINT) {
            int tb = 26;
            if (paint_input(w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12,
                            mx, my, mouse_left, prev_left)) {
                bring_to_front(idx); return;
            }
        }
        if (idx == WIN_LAUNCHER) {
            int tb = 26;
            if (point_in(mx, my, w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12)) {
                int app = al_click(w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12, mx, my);
                if (app >= 0) {
                    launcher_open_app(app);
                    al_close();
                    windows[WIN_LAUNCHER].visible = FALSE;
                    bring_to_front(focused_win);
                    return;
                }
                bring_to_front(idx); return;
            }
        }
        if (idx == WIN_TASKMGR) {
            int tb = 26;
            if (point_in(mx, my, w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12)) {
                int r = tm_click(w->x + 6, w->y + tb + 6, w->w - 12, w->h - tb - 12, mx, my);
                if (r == -2) {
                    int cid = tm_last_close_id();
                    if (cid >= 0 && cid < MAX_WINDOWS && cid != WIN_TASKMGR) {
                        windows[cid].visible = FALSE;
                        if (cid == WIN_LAUNCHER) al_close();
                        toast_show("Window closed", timer_ticks + PIT_HZ * 2);
                    }
                    /* refresh list */
                    tm_clear();
                    for (int j = 0; j < MAX_WINDOWS; j++)
                        if (windows[j].visible && j != WIN_TASKMGR)
                            tm_add(windows[j].title, j);
                    bring_to_front(idx); return;
                } else if (r >= 0) {
                    windows[r].visible = TRUE;
                    bring_to_front(r);
                    focused_win = r;
                    return;
                }
                bring_to_front(idx); return;
            }
        }
        if (point_in(mx, my, w->x, w->y, w->w, 26)) {
            if (!w->maximized) {
                dragging = idx; drag_dx = mx - w->x; drag_dy = my - w->y;
            }
            bring_to_front(idx); return;
        }
        bring_to_front(idx); return;
    }

    /* no window hit — desktop icons */
    for (int i = 0; i < DESK_ICON_N; i++) {
        int ix, iy, iw, ih;
        desk_icon_rect(i, &ix, &iy, &iw, &ih);
        if (point_in(mx, my, ix, iy, iw, ih)) {
            desk_open_app(i);
            return;
        }
    }
}
static void process_mouse_state(void) {
    bool_t edge_down = mouse_left && !prev_left;
    if (edge_down && cv_is_open()) {
        cv_close();
        prev_left = mouse_left;
        prev_right = mouse_right;
        return;
    }
    bool_t edge_up   = !mouse_left && prev_left;
    bool_t redge_down = mouse_right && !prev_right;

    if (edge_down) handle_mouse_click(mouse_x, mouse_y);
    if (edge_up) dragging = -1;

    /* right-click: File Manager or desktop menu */
    if (redge_down) {
        bool_t hit_win = FALSE;
        for (int i = MAX_WINDOWS-1; i >= 0; i--) {
            int idx = win_order[i];
            gui_window_t *w = &windows[idx];
            if (!w->visible) continue;
            if (!point_in(mouse_x, mouse_y, w->x, w->y, w->w, w->h)) continue;
            hit_win = TRUE;
            start_menu_open = FALSE;
            desk_ctx_open = FALSE;
            if (idx == WIN_FILES) {
                focused_win = WIN_FILES;
                bring_to_front(WIN_FILES);
                fm_handle_right_click(w, mouse_x, mouse_y);
            } else {
                fm_ctx_open = FALSE;
            }
            break;
        }
        if (!hit_win) {
            /* empty desktop / icons area → desktop context menu */
            fm_ctx_open = FALSE;
            start_menu_open = FALSE;
            /* don't open over taskbar */
            int thb = 46;
            if (mouse_y < (int)fb_height - thb) {
                desk_ctx_open = TRUE;
                desk_ctx_x = mouse_x;
                desk_ctx_y = mouse_y;
            }
        }
    }

    if (dragging >= 0 && mouse_left) {
        gui_window_t *w = &windows[dragging];
        w->x = mouse_x - drag_dx; w->y = mouse_y - drag_dy;
        if (w->x < -w->w+40) w->x = -w->w+40;
        if (w->y < 0) w->y = 0;
        if (w->x > (int)fb_width-40) w->x = (int)fb_width-40;
        if (w->y > (int)fb_height-60) w->y = (int)fb_height-60;
    }
    prev_left = mouse_left;
    prev_right = mouse_right;
}

/* ============================================================================
 *  KERNEL ENTRY
 * ==========================================================================*/
u8 kernel_stack[16384] __attribute__((aligned(16), used));

static void windows_init(void) {
    focused_win = WIN_TERMINAL;
    windows[WIN_TERMINAL] = (gui_window_t){
        .title="TERMINAL", .x=40, .y=30, .w=720, .h=480,
        .visible=FALSE, .has_close=TRUE, .is_terminal=TRUE, .maximized=FALSE
    };
    windows[WIN_ABOUT] = (gui_window_t){
        .title="ABOUT TITAN", .x=320, .y=150, .w=380, .h=250,
        .visible=FALSE, .has_close=TRUE, .is_about=TRUE, .maximized=FALSE
    };
    windows[WIN_NOTEPAD] = (gui_window_t){
        .title="NOTEPAD", .x=200, .y=80, .w=520, .h=340,
        .visible=FALSE, .has_close=TRUE, .is_notepad=TRUE, .maximized=FALSE
    };
    windows[WIN_FILES] = (gui_window_t){
        .title="FILE MANAGER", .x=80, .y=40, .w=780, .h=520,
        .visible=FALSE, .has_close=TRUE, .is_files=TRUE, .maximized=FALSE
    };
    windows[WIN_SETTINGS] = (gui_window_t){
        .title="SETTINGS", .x=180, .y=70, .w=620, .h=420,
        .visible=FALSE, .has_close=TRUE, .is_settings=TRUE, .maximized=FALSE
    };
    windows[WIN_CALC] = (gui_window_t){
        .title="CALCULATOR", .x=400, .y=120, .w=280, .h=340,
        .visible=FALSE, .has_close=TRUE, .is_calc=TRUE, .maximized=FALSE
    };
    windows[WIN_PAINT] = (gui_window_t){
        .title="PAINT", .x=220, .y=90, .w=280, .h=260,
        .visible=FALSE, .has_close=TRUE, .is_paint=TRUE, .maximized=FALSE
    };
    windows[WIN_LAUNCHER] = (gui_window_t){
        .title="APPLICATIONS", .x=360, .y=80, .w=320, .h=380,
        .visible=FALSE, .has_close=TRUE, .is_launcher=TRUE, .maximized=FALSE
    };
    windows[WIN_TASKMGR] = (gui_window_t){
        .title="TASK MANAGER", .x=300, .y=100, .w=360, .h=320,
        .visible=FALSE, .has_close=TRUE, .is_taskmgr=TRUE, .maximized=FALSE
    };
    win_order[0] = WIN_ABOUT;
    win_order[1] = WIN_SETTINGS;
    win_order[2] = WIN_NOTEPAD;
    win_order[3] = WIN_FILES;
    win_order[4] = WIN_TERMINAL;
    win_order[5] = WIN_CALC;
    win_order[6] = WIN_PAINT;
    win_order[7] = WIN_LAUNCHER;
    win_order[8] = WIN_TASKMGR;
    k_memset(note_lines, 0, sizeof(note_lines));
    calc_init();
    paint_init();
    cliphist_init();
    toast_init();
    cv_init();
    tm_init();
    al_init();
    al_register("Terminal", 0);
    al_register("Notepad", 1);
    al_register("File Manager", 2);
    al_register("About", 3);
    al_register("Settings", 4);
    al_register("Calculator", 5);
    al_register("Paint", 6);
    al_register("Task Manager", 7);
}

void kmain(void) {
    serial_init();
    debug_printf("=== TITAN KERNEL v3.0 (Glass Edition) booting ===\n");
    debug_printf("[boot] GDT...     "); gdt_init();  debug_printf("OK\n");
    debug_printf("[boot] IDT...     "); idt_init();  debug_printf("OK\n");
    debug_printf("[boot] PIC...     "); pic_remap(); debug_printf("OK\n");
    debug_printf("[boot] PIT...     "); timer_init(PIT_HZ); debug_printf("OK\n");
    debug_printf("[boot] keyboard...     "); keyboard_init(); debug_printf("OK\n");

    struct multiboot_info *mbi = (struct multiboot_info *)mb_info_ptr;
    if (mbi && (mbi->flags & 0x1000) && mbi->framebuffer_addr && mbi->framebuffer_bpp == 32) {
        fb_addr   = (u8*)(u32)mbi->framebuffer_addr;
        fb_pitch  = mbi->framebuffer_pitch;
        fb_width  = mbi->framebuffer_width;
        fb_height = mbi->framebuffer_height;
        fb_bpp    = mbi->framebuffer_bpp;
        fb_ready  = TRUE;
        debug_printf("[boot] framebuffer: %ux%u @ %ubpp addr=%x pitch=%u\n",
                      fb_width, fb_height, (u32)fb_bpp, (u32)mbi->framebuffer_addr, fb_pitch);
        backbuffer = (u32*)kmalloc(fb_width * fb_height * 4);
        if (!backbuffer) {
            debug_printf("[boot] FATAL: not enough heap for backbuffer (%u bytes needed)\n", fb_width*fb_height*4);
            for (;;) { __asm__ volatile ("hlt"); }
        }
        debug_printf("[boot] backbuffer allocated: %u KiB\n", (fb_width*fb_height*4)/1024);
    } else {
        debug_printf("[boot] FATAL: no usable 32bpp linear framebuffer from bootloader.\n");
        /* VGA text fallback so VirtualBox is not a pure black screen */
        {
            volatile u16 *vga = (volatile u16 *)0xB8000;
            const char *msg = "TITAN: No 32bpp FB. Set VMSVGA + 128MB VRAM or use QEMU.";
            for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F00 | ' ';
            for (int i = 0; msg[i] && i < 80; i++) vga[i] = 0x4F00 | (u8)msg[i];
        }
        for (;;) { __asm__ volatile ("hlt"); }
    }

    debug_printf("[boot] mouse...     "); mouse_init(); debug_printf("OK\n");
    debug_printf("[boot] network...   ");
    if (net_init()) debug_printf("OK (e1000)\n");
    else            debug_printf("no NIC\n");

    safety_init(heap_arena, HEAP_SIZE, panic);
    fs_init_defaults();
    persist_init(fs_persist_export, fs_persist_import);
    windows_init();
    lock_init();
    lock_set_success_hook(on_unlock_success);
    lock_set_verify_hook(lock_verify_password);
    term_puts("Titan Glass shell ready. Type 'help'.\n");
    shell_prompt();

    __asm__ volatile ("sti");

    last_input_tick = timer_ticks;
    lock_set_security_level(g_security_level);
    if (lock_is_active()) draw_lock_screen();
    else gui_redraw();
    u32 last_blink_tick = timer_ticks;
    u32 frame_count = 0, fps_last_tick = timer_ticks;
    for (;;) {
        bool_t need_redraw = FALSE;
        char c;
        net_poll();
        net_tick(timer_ticks);
        if (request_launcher) {
            request_launcher = FALSE;
            last_input_tick = timer_ticks;
            if (!lock_is_active()) {
                if (al_is_open()) { al_close(); windows[WIN_LAUNCHER].visible = FALSE; }
                else {
                    al_open(); windows[WIN_LAUNCHER].visible = TRUE;
                    bring_to_front(WIN_LAUNCHER); focused_win = WIN_LAUNCHER;
                }
                need_redraw = TRUE;
            }
        }
        if (request_taskmgr) {
            request_taskmgr = FALSE;
            last_input_tick = timer_ticks;
            if (!lock_is_active()) {
                tm_clear();
                for (int i = 0; i < MAX_WINDOWS; i++)
                    if (windows[i].visible && i != WIN_TASKMGR)
                        tm_add(windows[i].title, i);
                tm_open();
                windows[WIN_TASKMGR].visible = TRUE;
                bring_to_front(WIN_TASKMGR);
                focused_win = WIN_TASKMGR;
                need_redraw = TRUE;
            }
        }
        while (kbd_buf_pop(&c)) {
            last_input_tick = timer_ticks;
            if (c == 0x04) { /* Alt+F4 */
                if (!lock_is_active() && focused_win >= 0 && focused_win < MAX_WINDOWS) {
                    windows[focused_win].visible = FALSE;
                    if (focused_win == WIN_LAUNCHER) al_close();
                    if (focused_win == WIN_TASKMGR) tm_close();
                    toast_show("Window closed", timer_ticks + PIT_HZ * 2);
                    focused_win = WIN_TERMINAL;
                }
                need_redraw = TRUE; continue;
            }
            if (c == 0x05) { /* Alt+E files */
                if (!lock_is_active()) {
                    windows[WIN_FILES].visible = TRUE;
                    bring_to_front(WIN_FILES); focused_win = WIN_FILES;
                }
                need_redraw = TRUE; continue;
            }
            if (c == 0x06) { /* Alt+T terminal */
                if (!lock_is_active()) {
                    windows[WIN_TERMINAL].visible = TRUE;
                    bring_to_front(WIN_TERMINAL); focused_win = WIN_TERMINAL;
                }
                need_redraw = TRUE; continue;
            }
            if (lock_is_active())
                lock_on_key(c);
            else if (fm_naming)
                fm_handle_key(c);
            else if (focused_win == WIN_LAUNCHER && windows[WIN_LAUNCHER].visible) {
                if (c == 27) { al_close(); windows[WIN_LAUNCHER].visible = FALSE; }
                else if (c == 'j' || c == 's') al_scroll(1);
                else if (c == 'k' || c == 'w') al_scroll(-1);
            } else if (focused_win == WIN_TASKMGR && windows[WIN_TASKMGR].visible) {
                if (c == 27) { tm_close(); windows[WIN_TASKMGR].visible = FALSE; }
            }
            else if (focused_win == WIN_NOTEPAD && windows[WIN_NOTEPAD].visible) {
                if (note_saveas || note_rename) note_handle_key(c);
                else if (!note_menu_open) note_putc(c);
            } else if (focused_win == WIN_SETTINGS && windows[WIN_SETTINGS].visible)
                settings_handle_key(c);
            else if (focused_win == WIN_CALC && windows[WIN_CALC].visible)
                calc_key(c);
            else
                terminal_handle_key(c);
            need_redraw = TRUE;
        }
        if (mouse_dirty) {
            last_input_tick = timer_ticks;
            if (lock_is_active()) {
                bool_t edge_down = mouse_left && !prev_left;
                if (edge_down) lock_on_click();
                prev_left = mouse_left;
                prev_right = mouse_right;
            } else {
                process_mouse_state();
            }
            mouse_dirty = FALSE;
            need_redraw = TRUE;
        }
        lock_tick(timer_ticks, PIT_HZ);

        /* idle auto-lock: Basic 5min, Standard 2min, High 60s */
        if (!lock_is_active()) {
            u32 idle_limit = g_idle_timeout_sec * PIT_HZ;
            if (idle_limit < 10u * PIT_HZ) idle_limit = 10u * PIT_HZ;
            if (timer_ticks - last_input_tick >= idle_limit) {
                if (persist_pw_set) settings_persist_all(persist_pw);
                session_secure_lock();
                need_redraw = TRUE;
                debug_printf("[security] idle auto-lock (level %u)\n", g_security_level);
            }
        }

        if (timer_ticks - last_blink_tick >= 20) { last_blink_tick = timer_ticks; need_redraw = TRUE; }
        if (need_redraw) {
            if (lock_is_active()) draw_lock_screen();
            else gui_redraw();
            frame_count++;
        }
        if (timer_ticks - fps_last_tick >= PIT_HZ) {
            debug_printf("[perf] redraws in last second: %u\n", frame_count);
            frame_count = 0; fps_last_tick = timer_ticks;
        }
        __asm__ volatile ("hlt");
    }
}

/* ============================================================================
 *  ENTRY POINT
 * ==========================================================================*/
__asm__ (
    ".global _start\n"
    "_start:\n"
    "    mov %eax, mb_magic\n"
    "    mov %ebx, mb_info_ptr\n"
    "    mov $(kernel_stack + 16384), %esp\n"
    "    call kmain\n"
    "hang:\n"
    "    cli\n    hlt\n    jmp hang\n"
);
