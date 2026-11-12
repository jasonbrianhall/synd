/* devices.c - the PC's other devices as the games see them: the keyboard controller (port 60h, IRQ 1, the
 * BIOS buffer behind int 16h), the mouse driver (int 33h), the programmable interval timer (ports 40h-43h)
 * and the BIOS services (int 10h video, int 1Ah time, int 15h). */
#include <string.h>
#include <time.h>

#include "rt.h"

void vga_dac_set(int idx, uint8_t r, uint8_t g, uint8_t b);
void vga_dac_get(int idx, uint8_t *r, uint8_t *g, uint8_t *b);
void vga_attr_set(int idx, uint8_t v);
int vga_mode(void);

/* ==== keyboard ============================================================================================ */
#define KQ 256
static uint8_t g_kq[KQ];
static unsigned g_kq_head, g_kq_tail;      /* raw scancode bytes from the host */
static uint8_t g_port60;
static uint16_t g_bios_buf[32];
static unsigned g_bios_head, g_bios_tail;
static uint16_t g_shift;                   /* BIOS shift flags (0x417) */
static int g_e0;

/* scancode -> ASCII (US layout), unshifted and shifted */
static const char SC_ASCII[2][0x3A] = {
    {0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8,   9,   'q', 'w', 'e', 'r',
     't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
     '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '},
    {0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8,   9,   'Q', 'W', 'E', 'R',
     'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13,  0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
     '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '}};

void kbd_init(void)
{
    host_lock();
    g_kq_head = g_kq_tail = 0;
    g_bios_head = g_bios_tail = 0;
    g_shift = 0;
    host_unlock();
}

void rt_key_event(uint32_t scancode, int down)
{
    host_lock();
    if (scancode & 0xE000u) {
        if ((g_kq_tail + 1) % KQ != g_kq_head) {
            g_kq[g_kq_tail] = 0xE0;
            g_kq_tail = (g_kq_tail + 1) % KQ;
        }
    }
    if ((g_kq_tail + 1) % KQ != g_kq_head) {
        g_kq[g_kq_tail] = (uint8_t)((scancode & 0x7F) | (down ? 0 : 0x80));
        g_kq_tail = (g_kq_tail + 1) % KQ;
    }
    host_unlock();
}

static void bios_buffer_scancode(uint8_t sc)
{
    int up = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;
    uint16_t key = 0;
    if (sc == 0xE0) {
        g_e0 = 1;
        return;
    }
    switch (code) {
    case 0x2A:
        g_shift = (uint16_t)(up ? g_shift & ~2u : g_shift | 2u);
        break;
    case 0x36:
        g_shift = (uint16_t)(up ? g_shift & ~1u : g_shift | 1u);
        break;
    case 0x1D:
        g_shift = (uint16_t)(up ? g_shift & ~4u : g_shift | 4u);
        break;
    case 0x38:
        g_shift = (uint16_t)(up ? g_shift & ~8u : g_shift | 8u);
        break;
    default:
        if (!up) {
            char c = 0;
            if (code < 0x3A && !g_e0) c = SC_ASCII[(g_shift & 3) ? 1 : 0][code];
            if (g_e0 && code == 0x1C) c = 13;
            if (g_e0 && code == 0x35) c = '/';
            key = (uint16_t)((code << 8) | (uint8_t)c);
            if (g_shift & 4) {
                if (c >= 'a' && c <= 'z') key = (uint16_t)((code << 8) | (uint8_t)(c - 'a' + 1));
            }
            if (((g_bios_tail + 1) & 31) != g_bios_head) {
                g_bios_buf[g_bios_tail] = key;
                g_bios_tail = (g_bios_tail + 1) & 31;
            }
        }
        break;
    }
    g_e0 = 0;
    M[0x417] = (uint8_t)g_shift;
}

int kbd_irq_pending(void)
{
    int r = 0;
    host_lock();
    if (g_kq_head != g_kq_tail) {
        g_port60 = g_kq[g_kq_head];
        g_kq_head = (g_kq_head + 1) % KQ;
        r = 1;
    }
    host_unlock();
    return r;
}

void kbd_irq_ack(void)
{
    bios_buffer_scancode(g_port60);
}

uint8_t kbd_port60(void)
{
    return g_port60;
}

uint8_t kbd_port64(void)
{
    return 0x1C;
}

void kbd_port61_out(uint8_t v)
{
    (void)v;
}

int kbd_bios_peek(uint16_t *key)
{
    /* without a game keyboard handler the BIOS one fills the buffer; drain the host queue here too */
    if (dpmi_pm_vector(0x09) == 0u) {
        while (kbd_irq_pending()) kbd_irq_ack();
    }
    if (g_bios_head == g_bios_tail) return 0;
    *key = g_bios_buf[g_bios_head];
    return 1;
}

uint16_t kbd_bios_get(void)
{
    uint16_t k;
    while (!kbd_bios_peek(&k)) {
        rt_poll();
        host_sleep_us(2000);
    }
    g_bios_head = (g_bios_head + 1) & 31;
    return k;
}

uint16_t kbd_bios_shift(void)
{
    return g_shift;
}

/* ==== mouse =============================================================================================== */
static int g_mfx = 32768, g_mfy = 32768, g_mbuttons, g_mdx, g_mdy;
static int g_minx = 0, g_maxx = 639, g_miny = 0, g_maxy = 199;
static int g_mx = 320, g_my = 100, g_mouse_set; /* last position set by the game (function 4) */
static int g_press_count[3], g_release_count[3], g_press_x[3], g_press_y[3], g_rel_x[3], g_rel_y[3];
static int g_mouse_moved;
/* the event handler (function 0Ch/14h): called by mouse_poll_handler() for the events in its mask */
static uint16_t g_mh_mask, g_mh_sel;
static uint32_t g_mh_off;
/* events for it, oldest first: bits 1 moved, 2/4 left down/up, 8/16 right, 32/64 middle, and the buttons
 * after the event; a press and its release are separate calls, as from the real driver */
#define MQ_SIZE 32
static struct {
    int ev, buttons;
} g_mq[MQ_SIZE];
static int g_mq_n;
static int g_mh_lastx, g_mh_lasty, g_mickx, g_micky;

static void mq_add(int ev, int buttons) /* host_lock held */
{
    if (g_mq_n > 0 && g_mq[g_mq_n - 1].ev == 1 && ev == 1) return; /* moves merge */
    if (g_mq_n == MQ_SIZE) {
        memmove(g_mq, g_mq + 1, (MQ_SIZE - 1) * sizeof g_mq[0]);
        g_mq_n--;
    }
    g_mq[g_mq_n].ev = ev;
    g_mq[g_mq_n].buttons = buttons;
    g_mq_n++;
}

void rt_mouse_move(int fx, int fy)
{
    int ox, oy, nx, ny;
    host_lock();
    if (fx < 0) fx = 0;
    if (fx > 65535) fx = 65535;
    if (fy < 0) fy = 0;
    if (fy > 65535) fy = 65535;
    mouse_get(&ox, &oy, NULL);
    g_mfx = fx;
    g_mfy = fy;
    g_mouse_set = 0;
    mouse_get(&nx, &ny, NULL);
    /* the motion counters (function 0Bh), at the driver's default 8 mickeys per 8 pixels across and 16 down */
    g_mdx += nx - ox;
    g_mdy += (ny - oy) * 2;
    g_mouse_moved = 1;
    mq_add(1, g_mbuttons);
    host_unlock();
}

void rt_mouse_motion(int dx, int dy)
{
    host_lock();
    g_mdx += dx;
    g_mdy += dy;
    host_unlock();
}

void rt_mouse_buttons(int buttons)
{
    int i, x, y, cur;
    host_lock();
    cur = g_mbuttons;
    mouse_get(&x, &y, NULL);
    for (i = 0; i < 3; i++) {
        int was = (g_mbuttons >> i) & 1, now = (buttons >> i) & 1;
        if (now != was) {
            cur = (cur & ~(1 << i)) | (now << i);
            mq_add(1 << (1 + 2 * i + !now), cur);
        }
        if (now && !was) {
            g_press_count[i]++;
            g_press_x[i] = x;
            g_press_y[i] = y;
        } else if (!now && was) {
            g_release_count[i]++;
            g_rel_x[i] = x;
            g_rel_y[i] = y;
        }
    }
    g_mbuttons = buttons;
    host_unlock();
}

void mouse_get(int *x, int *y, int *buttons)
{
    if (g_mouse_set) {
        *x = g_mx;
        *y = g_my;
    } else {
        *x = g_minx + (int)((long long)g_mfx * (g_maxx - g_minx + 1) / 65536);
        *y = g_miny + (int)((long long)g_mfy * (g_maxy - g_miny + 1) / 65536);
    }
    if (buttons) *buttons = g_mbuttons;
}

void mouse_take_motion(int *dx, int *dy)
{
    host_lock();
    *dx = g_mdx;
    *dy = g_mdy;
    g_mdx = g_mdy = 0;
    host_unlock();
}

static void set_reg16(uint32_t *r, int v)
{
    *r = (*r & 0xFFFF0000u) | ((uint32_t)v & 0xFFFFu);
}

/* whether the running program uses the mouse: it read the position, the buttons or the motion, or set an event
 * handler (the launcher only sets the range and never reads it; it draws no pointer) */
static volatile int g_mouse_used;

int rt_mouse_used(void)
{
    return g_mouse_used;
}

int mouse_used_swap(int used)
{
    int was = g_mouse_used;
    g_mouse_used = used;
    return was;
}

void bios_int33(void)
{
    uint16_t ax = (uint16_t)R.eax;
    int x, y, b, i;
    if (ax == 0x0003 || ax == 0x0005 || ax == 0x0006 || ax == 0x000B || ax == 0x000C || ax == 0x0014)
        g_mouse_used = 1;
    switch (ax) {
    case 0x0000:
    case 0x0021:
        g_mh_mask = 0;
        set_reg16(&R.eax, 0xFFFF);
        set_reg16(&R.ebx, 2);
        g_minx = 0;
        g_maxx = 639;
        g_miny = 0;
        g_maxy = 199;
        return;
    case 0x0001:
    case 0x0002:
        return;
    case 0x0003:
        host_lock();
        mouse_get(&x, &y, &b);
        host_unlock();
        set_reg16(&R.ebx, b);
        set_reg16(&R.ecx, x);
        set_reg16(&R.edx, y);
        return;
    case 0x0004:
        host_lock();
        g_mx = (int16_t)R.ecx;
        g_my = (int16_t)R.edx;
        if (g_maxx > g_minx) g_mfx = (int)((long long)(g_mx - g_minx) * 65536 / (g_maxx - g_minx + 1));
        if (g_maxy > g_miny) g_mfy = (int)((long long)(g_my - g_miny) * 65536 / (g_maxy - g_miny + 1));
        g_mouse_set = 0;
        host_unlock();
        return;
    case 0x0005:
    case 0x0006:
        i = (int)(R.ebx & 0xFFFF) % 3;
        host_lock();
        set_reg16(&R.eax, g_mbuttons);
        if (ax == 5) {
            set_reg16(&R.ebx, g_press_count[i]);
            set_reg16(&R.ecx, g_press_x[i]);
            set_reg16(&R.edx, g_press_y[i]);
            g_press_count[i] = 0;
        } else {
            set_reg16(&R.ebx, g_release_count[i]);
            set_reg16(&R.ecx, g_rel_x[i]);
            set_reg16(&R.edx, g_rel_y[i]);
            g_release_count[i] = 0;
        }
        host_unlock();
        return;
    case 0x0007:
        g_minx = (int16_t)R.ecx;
        g_maxx = (int16_t)R.edx;
        if (g_minx > g_maxx) {
            int t = g_minx;
            g_minx = g_maxx;
            g_maxx = t;
        }
        return;
    case 0x0008:
        g_miny = (int16_t)R.ecx;
        g_maxy = (int16_t)R.edx;
        if (g_miny > g_maxy) {
            int t = g_miny;
            g_miny = g_maxy;
            g_maxy = t;
        }
        return;
    case 0x000B: {
        int dx, dy;
        mouse_take_motion(&dx, &dy);
        set_reg16(&R.ecx, dx);
        set_reg16(&R.edx, dy);
        return;
    }
    case 0x000C: /* the event handler: CX the event mask, ES:EDX the routine (DOS/4GW: a 32-bit far call) */
    case 0x0014: { /* the same, returning the previous one */
        uint16_t mask = g_mh_mask, sel = g_mh_sel;
        uint32_t off = g_mh_off;
        host_lock();
        g_mh_mask = (uint16_t)R.ecx;
        g_mh_sel = R.es;
        g_mh_off = R.edx;
        g_mq_n = 0;
        mouse_get(&g_mh_lastx, &g_mh_lasty, NULL);
        host_unlock();
        rt_trace("int 33h: event handler %04x:%08x mask %04x", g_mh_sel, g_mh_off, g_mh_mask);
        if (ax == 0x0014) {
            set_reg16(&R.ecx, mask);
            R.edx = off;
            rt_load_seg(SEG_ES, sel);
        }
        return;
    }
    case 0x000F:
    case 0x001A:
    case 0x001D:
        return;
    case 0x0024:
        set_reg16(&R.ebx, 0x0626);
        set_reg16(&R.ecx, 0x0400);
        return;
    default:
        rt_trace("int 33h AX=%04x", ax);
        return;
    }
}

/* called from the poll loop with interrupts enabled: runs the game's mouse event handler when one of its
 * events happened, the way the mouse driver calls it from its interrupt (AX the events, BX the buttons,
 * CX/DX the position, SI/DI the motion counters; every register is kept for the interrupted code) */
int mouse_poll_handler(void)
{
    int ev = 0, x, y, b = 0;
    Cpu saved;
    if (g_mh_mask == 0) return 0;
    host_lock();
    while (g_mq_n > 0 && ev == 0) {
        ev = g_mq[0].ev & g_mh_mask;
        b = g_mq[0].buttons;
        memmove(g_mq, g_mq + 1, (size_t)(g_mq_n - 1) * sizeof g_mq[0]);
        g_mq_n--;
    }
    mouse_get(&x, &y, NULL);
    host_unlock();
    if (ev == 0) return 0;
    g_mickx += x - g_mh_lastx; /* as the motion counters: 1 mickey per pixel across, 2 down */
    g_micky += (y - g_mh_lasty) * 2;
    g_mh_lastx = x;
    g_mh_lasty = y;
    saved = R;
    R.eax = (uint32_t)ev;
    R.ebx = (uint32_t)b;
    R.ecx = (uint32_t)x;
    R.edx = (uint32_t)y;
    R.esi = (uint32_t)(uint16_t)g_mickx;
    R.edi = (uint32_t)(uint16_t)g_micky;
    R.iflag = 0;
    PUSH32(R.cs);
    PUSH32(0xFFFFFFF0u); /* the return address the handler's retf pops (not used: the call returns here) */
    rt_in_irq++;
    rt_call_ind(rt_sel_base(g_mh_sel) + g_mh_off);
    rt_in_irq--;
    rt_trace("mouse handler: events %02x buttons %d at %d,%d", ev, b, x, y);
    R = saved;
    return 1;
}

/* ==== PIT ================================================================================================ */
static uint32_t g_pit_div = 65536;
static int g_pit_lohi, g_pit_mode;
static uint16_t g_pit_latch;
static int g_pit_latched, g_pit_read_hi;
static uint64_t g_pit_epoch;

void pit_init(void)
{
    g_pit_div = 65536;
    g_pit_epoch = host_time_us();
}

uint32_t pit_irq0_period_us(void)
{
    return (uint32_t)((uint64_t)g_pit_div * 1000000u / 1193182u);
}

static uint16_t pit_count(void)
{
    uint64_t ticks = (host_time_us() - g_pit_epoch) * 1193182u / 1000000u;
    return (uint16_t)(g_pit_div - (uint32_t)(ticks % g_pit_div));
}

void pit_port_out(uint16_t port, uint8_t v)
{
    if (port == 0x43) {
        if ((v >> 6) != 0) return; /* channels 1/2 (speaker) ignored */
        if (((v >> 4) & 3) == 0) {
            g_pit_latch = pit_count();
            g_pit_latched = 1;
            g_pit_read_hi = 0;
            return;
        }
        g_pit_mode = (v >> 1) & 7;
        g_pit_lohi = 0;
        return;
    }
    if (port == 0x40) {
        if (g_pit_lohi == 0) {
            g_pit_div = (g_pit_div & 0xFF00u) | v;
            g_pit_lohi = 1;
        } else {
            g_pit_div = (g_pit_div & 0x00FFu) | ((uint32_t)v << 8);
            if (g_pit_div == 0) g_pit_div = 65536;
            g_pit_lohi = 0;
            g_pit_epoch = host_time_us();
            rt_trace("PIT channel 0 divisor %u (%.2f Hz)", g_pit_div, 1193182.0 / g_pit_div);
        }
    }
}

uint8_t pit_port_in(uint16_t port)
{
    if (port == 0x40) {
        uint16_t c = g_pit_latched ? g_pit_latch : pit_count();
        if (!g_pit_read_hi) {
            g_pit_read_hi = 1;
            return (uint8_t)c;
        }
        g_pit_read_hi = 0;
        g_pit_latched = 0;
        return (uint8_t)(c >> 8);
    }
    return 0;
}

/* ==== BIOS =============================================================================================== */
void bios_init(void)
{
    memset(M + LOW_BDA, 0, 256);
    WR16(0x410, 0x0021);   /* equipment: 80x25 colour, FPU none */
    WR16(0x413, 640);      /* conventional memory KiB */
    M[0x449] = 3;
    WR16(0x44A, 80);
    WR16(0x463, 0x3D4);
    M[0x484] = 24;
    {
        /* the tick count starts at the time of day, as the BIOS sets it at boot */
        struct tm tm;
        uint32_t s;
        rt_wall_time(&tm);
        s = (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        WR32(0x46C, (uint32_t)((uint64_t)s * 1193180u / 65536u));
    }
}

/* the date and time of day: the computer's, or with rt_fixed_clock (repeatable test runs) 15 June 1995
 * 12:00:00 plus the emulated time since the start */
int rt_fixed_clock;

void rt_wall_time(struct tm *out)
{
    if (rt_fixed_clock) {
        static uint64_t start;
        uint64_t now = host_time_us();
        uint32_t s;
        if (start == 0) start = now;
        s = 12u * 3600u + (uint32_t)((now - start) / 1000000u);
        memset(out, 0, sizeof *out);
        out->tm_year = 95;
        out->tm_mon = 5;
        out->tm_mday = 15 + (int)(s / 86400u);
        out->tm_wday = (4 + (int)(s / 86400u)) % 7;
        s %= 86400u;
        out->tm_hour = (int)(s / 3600u);
        out->tm_min = (int)(s / 60u % 60u);
        out->tm_sec = (int)(s % 60u);
    } else {
        time_t t = time(NULL);
        *out = *localtime(&t);
    }
}

void bios_tick(void)
{
    uint32_t t = RD32(0x46C) + 1u;
    if (t >= 0x1800B0u) {
        t = 0;
        M[0x470] = 1;
    }
    WR32(0x46C, t);
}

void bios_int10(void)
{
    uint8_t ah = (uint8_t)(R.eax >> 8), al = (uint8_t)R.eax;
    switch (ah) {
    case 0x00:
        rt_log("video mode %02x", al);
        vga_set_mode(al);
        return;
    case 0x0F:
        R.eax = (R.eax & 0xFFFF0000u) | ((uint32_t)(vga_mode() == 0x13 ? 40 : 80) << 8) | (uint32_t)vga_mode();
        R.ebx &= 0xFFFF00FFu;
        return;
    case 0x10:
        switch (al) {
        case 0x00:
            vga_attr_set(R.ebx & 0xFF, (uint8_t)(R.ebx >> 8));
            return;
        case 0x01:
            vga_attr_set(0x11, (uint8_t)(R.ebx >> 8));
            return;
        case 0x02: {
            int i;
            for (i = 0; i < 17; i++) vga_attr_set(i == 16 ? 0x11 : i, RD8(R.es_base + R.edx + (uint32_t)i));
            return;
        }
        case 0x10:
            vga_dac_set(R.ebx & 0xFF, (uint8_t)(R.edx >> 8), (uint8_t)(R.ecx >> 8), (uint8_t)R.ecx);
            return;
        case 0x12: {
            uint32_t i, a = R.es_base + R.edx;
            for (i = 0; i < (R.ecx & 0xFFFF); i++)
                vga_dac_set((int)((R.ebx & 0xFF) + i), RD8(a + 3 * i), RD8(a + 3 * i + 1), RD8(a + 3 * i + 2));
            return;
        }
        case 0x15: {
            uint8_t r, g, b;
            vga_dac_get(R.ebx & 0xFF, &r, &g, &b);
            R.edx = (R.edx & 0xFFFF00FFu) | ((uint32_t)r << 8);
            R.ecx = (R.ecx & 0xFFFF0000u) | ((uint32_t)g << 8) | b;
            return;
        }
        case 0x17: {
            uint32_t i, a = R.es_base + R.edx;
            for (i = 0; i < (R.ecx & 0xFFFF); i++) {
                uint8_t r, g, b;
                vga_dac_get((int)((R.ebx & 0xFF) + i), &r, &g, &b);
                WR8(a + 3 * i, r);
                WR8(a + 3 * i + 1, g);
                WR8(a + 3 * i + 2, b);
            }
            return;
        }
        default:
            return;
        }
    case 0x11:
    case 0x01:
    case 0x02:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x09:
    case 0x0E:
        return;
    case 0x03:
        R.edx &= 0xFFFF0000u;
        R.ecx = (R.ecx & 0xFFFF0000u) | 0x0607u;
        return;
    case 0x08:
        R.eax = (R.eax & 0xFFFF0000u) | 0x0720u;
        return;
    case 0x12:
        if ((R.ebx & 0xFF) == 0x10) { /* EGA info: colour, 256 KiB */
            R.ebx = (R.ebx & 0xFFFF0000u) | 0x0003u;
            R.ecx &= 0xFFFF0000u;
            return;
        }
        R.eax = (R.eax & 0xFFFFFF00u) | 0x12u;
        return;
    case 0x1A:
        R.eax = (R.eax & 0xFFFFFF00u) | 0x1Au;
        R.ebx = (R.ebx & 0xFFFF0000u) | 0x0008u;
        return;
    case 0x1B:
        R.eax = (R.eax & 0xFFFFFF00u) | 0x1Bu;
        return;
    case 0x4F: /* no VESA */
        R.eax = (R.eax & 0xFFFF0000u) | 0x0100u;
        return;
    default:
        rt_trace("int 10h AX=%04x", (uint16_t)R.eax);
        return;
    }
}

void bios_int16(void)
{
    uint8_t ah = (uint8_t)(R.eax >> 8);
    uint16_t k;
    switch (ah) {
    case 0x00:
    case 0x10:
        k = kbd_bios_get();
        R.eax = (R.eax & 0xFFFF0000u) | k;
        return;
    case 0x01:
    case 0x11:
        rt_poll();
        if (kbd_bios_peek(&k)) {
            R.zf = 0;
            R.eax = (R.eax & 0xFFFF0000u) | k;
        } else {
            R.zf = 1;
        }
        return;
    case 0x02:
    case 0x12:
        R.eax = (R.eax & 0xFFFFFF00u) | (kbd_bios_shift() & 0xFFu);
        return;
    default:
        return;
    }
}

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void bios_int1a(void)
{
    uint8_t ah = (uint8_t)(R.eax >> 8);
    struct tm tmv, *tm = &tmv;
    rt_wall_time(&tmv);
    switch (ah) {
    case 0x00: {
        static uint32_t last;
        static int repeats;
        uint32_t ticks = RD32(0x46C);
        if (ticks == last)
            rt_clock_wait(++repeats);
        else
            repeats = 0;
        ticks = last = RD32(0x46C);
        R.ecx = (R.ecx & 0xFFFF0000u) | (ticks >> 16);
        R.edx = (R.edx & 0xFFFF0000u) | (ticks & 0xFFFFu);
        R.eax = (R.eax & 0xFFFFFF00u) | M[0x470];
        M[0x470] = 0;
        return;
    }
    case 0x02:
        R.ecx = (R.ecx & 0xFFFF0000u) | ((uint32_t)bcd(tm->tm_hour) << 8) | bcd(tm->tm_min);
        R.edx = (R.edx & 0xFFFF0000u) | ((uint32_t)bcd(tm->tm_sec) << 8);
        R.cf = 0;
        return;
    case 0x04:
        R.ecx = (R.ecx & 0xFFFF0000u) | ((uint32_t)bcd((tm->tm_year + 1900) / 100) << 8) |
                bcd((tm->tm_year + 1900) % 100);
        R.edx = (R.edx & 0xFFFF0000u) | ((uint32_t)bcd(tm->tm_mon + 1) << 8) | bcd(tm->tm_mday);
        R.cf = 0;
        return;
    default:
        return;
    }
}

void bios_int15(void)
{
    uint8_t ah = (uint8_t)(R.eax >> 8);
    if (ah == 0x86) {
        uint32_t us = ((R.ecx & 0xFFFFu) << 16) | (R.edx & 0xFFFFu);
        uint64_t end = host_time_us() + us;
        while (host_time_us() < end) {
            rt_poll();
            host_sleep_us(500);
        }
        R.cf = 0;
        return;
    }
    if (ah == 0x88) {
        R.eax &= 0xFFFF0000u;
        R.cf = 0;
        return;
    }
    R.cf = 1;
    R.eax = (R.eax & 0xFFFF00FFu) | 0x8600u;
}
