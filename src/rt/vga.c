/* vga.c - a register-level VGA: 256 KiB of planar memory behind the A0000h window, the sequencer, the
 * graphics controller (write modes 0-3, rotate/logical operations, bit mask, latches, read modes 0/1), the
 * CRTC's addressing (start address, offset, byte/word/doubleword modes, scan doubling), the attribute
 * controller and the DAC.  vga_render produces what the monitor would show: 256-colour modes (chained
 * mode 13h and unchained "mode X" alike) and the 16-colour planar modes; text modes show nothing. */
#include <string.h>

#include "rt.h"

static uint8_t g_plane[4][65536];
static uint8_t g_latch[4];
static uint8_t g_seq[8], g_seq_idx;
static uint8_t g_gc[16], g_gc_idx;
static uint8_t g_crtc[32], g_crtc_idx;
static uint8_t g_attr[32], g_attr_idx, g_attr_flip;
static uint8_t g_dac[256][3], g_dac_widx, g_dac_ridx, g_dac_comp, g_dac_rcomp, g_pel_mask = 0xFF;
static uint8_t g_misc = 0x63;
static int g_mode = 3;
int vga_retrace_reads;

/* ---- standard register sets (as the BIOS programs them) --------------------------------------------- */
typedef struct ModeRegs {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t attr[21];
} ModeRegs;

static const ModeRegs MODE13 = {
    0x63,
    {0x03, 0x01, 0x0F, 0x00, 0x0E},
    {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E,
     0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00,
     0x0F, 0x00, 0x00}};

static const ModeRegs MODE12 = {
    0xE3,
    {0x03, 0x01, 0x0F, 0x00, 0x06},
    {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEA, 0x8C,
     0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x01, 0x00,
     0x0F, 0x00, 0x00}};

static const ModeRegs MODE0D = {
    0x63,
    {0x09, 0x09, 0x0F, 0x00, 0x06},
    {0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E,
     0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x01, 0x00,
     0x0F, 0x00, 0x00}};

static const ModeRegs MODE03 = {
    0x67,
    {0x00, 0x00, 0x03, 0x00, 0x02},
    {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E,
     0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF},
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00,
     0x0F, 0x08, 0x00}};

static void load_regs(const ModeRegs *m)
{
    memcpy(g_seq, m->seq, 5);
    memcpy(g_crtc, m->crtc, 25);
    memcpy(g_gc, m->gc, 9);
    memcpy(g_attr, m->attr, 21);
    g_misc = m->misc;
}

/* the default 256-colour DAC: the 16 EGA colours, 16 greys, then the 216-colour hue table (approximated) */
static void default_dac(void)
{
    static const uint8_t ega[16][3] = {{0, 0, 0},    {0, 0, 42},   {0, 42, 0},   {0, 42, 42},  {42, 0, 0},   {42, 0, 42},
                                       {42, 21, 0},  {42, 42, 42}, {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
                                       {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};
    int i;
    for (i = 0; i < 256; i++) {
        if (i < 16) {
            memcpy(g_dac[i], ega[i], 3);
        } else if (i < 32) {
            uint8_t v = (uint8_t)((i - 16) * 63 / 15);
            g_dac[i][0] = g_dac[i][1] = g_dac[i][2] = v;
        } else {
            g_dac[i][0] = (uint8_t)((i * 7) & 63);
            g_dac[i][1] = (uint8_t)((i * 13) & 63);
            g_dac[i][2] = (uint8_t)((i * 3) & 63);
        }
    }
}

void vga_init(void)
{
    memset(g_plane, 0, sizeof g_plane);
    load_regs(&MODE03);
    default_dac();
    g_mode = 3;
}

void vga_set_mode(int mode)
{
    int clear = !(mode & 0x80);
    mode &= 0x7F;
    switch (mode) {
    case 0x13:
        load_regs(&MODE13);
        break;
    case 0x12:
    case 0x11:
        load_regs(&MODE12);
        break;
    case 0x0D:
        load_regs(&MODE0D);
        break;
    default:
        load_regs(&MODE03);
        break;
    }
    g_mode = mode;
    if (clear) memset(g_plane, 0, sizeof g_plane);
    if (mode != 3) default_dac();
    M[0x449] = (uint8_t)mode;
}

/* ---- CPU access --------------------------------------------------------------------------------------- */
static int map_offset(uint32_t a, uint32_t *off)
{
    uint32_t o = a - 0xA0000u;
    switch ((g_gc[6] >> 2) & 3) {
    case 0:
        *off = o & 0xFFFFu;
        return 1;
    case 1:
        if (o >= 0x10000u) return 0;
        *off = o;
        return 1;
    case 2:
        if (o < 0x10000u || o >= 0x18000u) return 0;
        *off = o - 0x10000u;
        return 1;
    default:
        if (o < 0x18000u) return 0;
        *off = o - 0x18000u;
        return 1;
    }
}

uint8_t vga_read8(uint32_t a)
{
    uint32_t off;
    int p;
    if (!map_offset(a, &off)) return 0xFF;
    if (g_seq[4] & 0x08) { /* chain 4 */
        p = (int)(off & 3u);
        off &= 0xFFFCu;
        return g_plane[p][off];
    }
    if (!(g_seq[4] & 0x04)) { /* odd/even */
        p = (int)(off & 1u) | (g_gc[4] & 2);
        return g_plane[p & 3][off & ~1u];
    }
    for (p = 0; p < 4; p++) g_latch[p] = g_plane[p][off];
    if (g_gc[5] & 0x08) { /* read mode 1: colour compare */
        uint8_t res = 0;
        int bit;
        for (bit = 0; bit < 8; bit++) {
            int match = 1;
            for (p = 0; p < 4; p++) {
                if (!(g_gc[7] & (1 << p))) continue;
                if ((((g_latch[p] >> bit) & 1) != ((g_gc[2] >> p) & 1))) match = 0;
            }
            if (match) res |= (uint8_t)(1 << bit);
        }
        return res;
    }
    return g_latch[g_gc[4] & 3];
}

static uint8_t alu(uint8_t v, uint8_t latch)
{
    switch ((g_gc[3] >> 3) & 3) {
    case 1:
        return v & latch;
    case 2:
        return v | latch;
    case 3:
        return v ^ latch;
    default:
        return v;
    }
}

void vga_write8(uint32_t a, uint8_t v)
{
    uint32_t off;
    int p;
    uint8_t mask = g_seq[2] & 0x0F;
    if (!map_offset(a, &off)) return;
    if (g_seq[4] & 0x08) { /* chain 4: the mode 13h fast path */
        p = (int)(off & 3u);
        if (mask & (1 << p)) {
            if ((g_gc[5] & 3) == 0 && g_gc[1] == 0 && g_gc[3] == 0 && g_gc[8] == 0xFF)
                g_plane[p][off & 0xFFFCu] = v;
            else
                g_plane[p][off & 0xFFFCu] = v; /* TODO: write modes in chain 4 (unused by the games) */
        }
        return;
    }
    if (!(g_seq[4] & 0x04)) { /* odd/even */
        p = (int)(off & 1u);
        if (mask & (1 << p)) g_plane[p][off & ~1u] = v;
        if (mask & (4 << p)) g_plane[p + 2][off & ~1u] = v;
        return;
    }
    {
        int wm = g_gc[5] & 3;
        uint8_t rot = g_gc[3] & 7, bitmask = g_gc[8];
        uint8_t val[4];
        if (rot) v = (uint8_t)((v >> rot) | (v << (8 - rot)));
        for (p = 0; p < 4; p++) {
            uint8_t d;
            switch (wm) {
            case 0:
                d = (g_gc[1] & (1 << p)) ? ((g_gc[0] & (1 << p)) ? 0xFF : 0x00) : v;
                d = alu(d, g_latch[p]);
                d = (uint8_t)((d & bitmask) | (g_latch[p] & ~bitmask));
                break;
            case 1:
                d = g_latch[p];
                break;
            case 2:
                d = (v & (1 << p)) ? 0xFF : 0x00;
                d = alu(d, g_latch[p]);
                d = (uint8_t)((d & bitmask) | (g_latch[p] & ~bitmask));
                break;
            default: {
                uint8_t m = v & bitmask;
                d = (g_gc[0] & (1 << p)) ? 0xFF : 0x00;
                d = alu(d, g_latch[p]);
                d = (uint8_t)((d & m) | (g_latch[p] & ~m));
                break;
            }
            }
            val[p] = d;
        }
        for (p = 0; p < 4; p++)
            if (mask & (1 << p)) g_plane[p][off] = val[p];
    }
}

/* ---- ports -------------------------------------------------------------------------------------------- */
static int in_retrace(void)
{
    uint64_t t = host_time_us() % 14286u;
    return t >= 13000u;
}

uint8_t vga_port_in(uint16_t port)
{
    switch (port) {
    case 0x3C0:
        return g_attr_idx;
    case 0x3C1:
        return g_attr[g_attr_idx & 0x1F];
    case 0x3C2:
        return 0x10;
    case 0x3C4:
        return g_seq_idx;
    case 0x3C5:
        return g_seq[g_seq_idx & 7];
    case 0x3C6:
        return g_pel_mask;
    case 0x3C7:
        return 0x03;
    case 0x3C8:
        return g_dac_widx;
    case 0x3C9: {
        uint8_t v = g_dac[g_dac_ridx][g_dac_rcomp];
        if (++g_dac_rcomp == 3) {
            g_dac_rcomp = 0;
            g_dac_ridx++;
        }
        return v;
    }
    case 0x3CC:
        return g_misc;
    case 0x3CE:
        return g_gc_idx;
    case 0x3CF:
        return g_gc[g_gc_idx & 15];
    case 0x3D4:
    case 0x3B4:
        return g_crtc_idx;
    case 0x3D5:
    case 0x3B5:
        return g_crtc[g_crtc_idx & 31];
    case 0x3DA:
    case 0x3BA: {
        int r = in_retrace();
        g_attr_flip = 0;
        vga_retrace_reads++;
        if (!r && vga_retrace_reads > 200) {
            /* a retrace wait loop: give the time back instead of spinning */
            uint64_t t = host_time_us() % 14286u;
            if (t < 12000u) host_sleep_us(500);
            vga_retrace_reads = 0;
        }
        return (uint8_t)(r ? 0x09 : ((host_time_us() & 0x10) ? 0x01 : 0x00));
    }
    default:
        return 0xFF;
    }
}

void vga_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!g_attr_flip)
            g_attr_idx = v;
        else
            g_attr[g_attr_idx & 0x1F] = v;
        g_attr_flip ^= 1;
        return;
    case 0x3C2:
        g_misc = v;
        return;
    case 0x3C4:
        g_seq_idx = v;
        return;
    case 0x3C5:
        g_seq[g_seq_idx & 7] = v;
        return;
    case 0x3C6:
        g_pel_mask = v;
        return;
    case 0x3C7:
        g_dac_ridx = v;
        g_dac_rcomp = 0;
        return;
    case 0x3C8:
        g_dac_widx = v;
        g_dac_comp = 0;
        return;
    case 0x3C9:
        g_dac[g_dac_widx][g_dac_comp] = v & 63;
        if (++g_dac_comp == 3) {
            g_dac_comp = 0;
            g_dac_widx++;
        }
        return;
    case 0x3CE:
        g_gc_idx = v;
        return;
    case 0x3CF:
        g_gc[g_gc_idx & 15] = v;
        return;
    case 0x3D4:
    case 0x3B4:
        g_crtc_idx = v;
        return;
    case 0x3D5:
    case 0x3B5:
        if ((g_crtc_idx & 31) < 8 && (g_crtc[0x11] & 0x80)) {
            if ((g_crtc_idx & 31) == 7) g_crtc[7] = (uint8_t)((g_crtc[7] & ~0x10) | (v & 0x10));
            return; /* write-protected */
        }
        g_crtc[g_crtc_idx & 31] = v;
        return;
    default:
        return;
    }
}

/* the BIOS palette services work on the DAC directly */
void vga_dac_set(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    g_dac[idx & 255][0] = r & 63;
    g_dac[idx & 255][1] = g & 63;
    g_dac[idx & 255][2] = b & 63;
}

void vga_dac_get(int idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = g_dac[idx & 255][0];
    *g = g_dac[idx & 255][1];
    *b = g_dac[idx & 255][2];
}

void vga_attr_set(int idx, uint8_t v)
{
    g_attr[idx & 31] = v;
}

int vga_mode(void)
{
    return g_mode;
}

/* ---- rendering ------------------------------------------------------------------------------------------ */
int vga_render(uint8_t *pixels, int *w, int *h, uint32_t *palette)
{
    int i, chars, vdisp, row_h, rows, y, x;
    uint32_t start, offset, line;
    int graphics = (g_gc[6] & 1) != 0;
    int c256 = (g_attr[0x10] & 0x40) != 0;
    int shift;
    if (!graphics) return 0;
    for (i = 0; i < 256; i++) {
        uint32_t r = g_dac[i][0], g = g_dac[i][1], b = g_dac[i][2];
        palette[i] = (((r << 2) | (r >> 4)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 2) | (b >> 4));
    }
    chars = g_crtc[1] + 1;
    vdisp = (g_crtc[0x12] | ((g_crtc[7] & 0x02) << 7) | ((g_crtc[7] & 0x40) << 3)) + 1;
    row_h = (g_crtc[9] & 0x1F) + 1;
    if (g_crtc[9] & 0x80) row_h *= 2;
    rows = vdisp / row_h;
    start = ((uint32_t)g_crtc[0x0C] << 8) | g_crtc[0x0D];
    offset = (uint32_t)g_crtc[0x13] * 2u;
    shift = (g_crtc[0x14] & 0x40) ? 2 : ((g_crtc[0x17] & 0x40) ? 0 : 1);
    if (c256) {
        int pw = chars * 4;
        if (pw > 640) pw = 640;
        if (rows > 480) rows = 480;
        *w = pw;
        *h = rows;
        line = start;
        for (y = 0; y < rows; y++) {
            uint8_t *dst = pixels + y * pw;
            for (x = 0; x < pw / 4; x++) {
                uint32_t a = ((line + (uint32_t)x) << shift) & 0xFFFFu;
                dst[4 * x + 0] = g_plane[0][a];
                dst[4 * x + 1] = g_plane[1][a];
                dst[4 * x + 2] = g_plane[2][a];
                dst[4 * x + 3] = g_plane[3][a];
            }
            line += offset;
        }
        for (i = 0; i < pw * rows; i++) pixels[i] &= g_pel_mask;
        return 1;
    }
    /* 16-colour planar */
    {
        int pw = chars * 8;
        uint32_t pal16[16];
        if (pw > 640) pw = 640;
        if (rows > 480) rows = 480;
        *w = pw;
        *h = rows;
        for (i = 0; i < 16; i++) {
            uint8_t a = g_attr[i] & 0x3F;
            uint8_t idx = (g_attr[0x10] & 0x80) ? (uint8_t)((a & 0x0F) | ((g_attr[0x14] & 0x0F) << 4))
                                                : (uint8_t)(a | ((g_attr[0x14] & 0x0C) << 4));
            pal16[i] = idx;
        }
        line = start;
        for (y = 0; y < rows; y++) {
            uint8_t *dst = pixels + y * pw;
            for (x = 0; x < pw; x++) {
                uint32_t a = ((line + (uint32_t)(x >> 3)) << shift) & 0xFFFFu;
                int bit = 7 - (x & 7), c;
                c = ((g_plane[0][a] >> bit) & 1) | (((g_plane[1][a] >> bit) & 1) << 1) |
                    (((g_plane[2][a] >> bit) & 1) << 2) | (((g_plane[3][a] >> bit) & 1) << 3);
                dst[x] = (uint8_t)pal16[c & g_attr[0x12] & 0x0F];
            }
            line += offset;
        }
        return 1;
    }
}
