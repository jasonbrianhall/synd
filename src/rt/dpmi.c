/* dpmi.c - the DPMI 0.9 host services (int 31h) DOS/4GW gives a program: descriptors, DOS memory (in the
 * first megabyte of M), extended memory (above HIGH_HEAP_BASE), interrupt vectors and real-mode interrupt
 * simulation (forwarded to the BIOS/DOS emulation). */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

/* ---- a first-fit allocator over [lo, hi) --------------------------------------------------------------- */
typedef struct Block {
    uint32_t addr, size;
    uint32_t handle;
    int used;
} Block;

typedef struct Heap {
    Block b[512];
    int n;
    uint32_t lo, hi, align;
} Heap;

static Heap g_dos_heap, g_xms_heap;
static uint32_t g_next_handle = 1;
static uint32_t g_rm_vectors[256];
static uint32_t g_exc_handlers[32];
extern uint32_t g_heap_top_hint;

static void heap_init(Heap *h, uint32_t lo, uint32_t hi, uint32_t align)
{
    memset(h, 0, sizeof *h);
    h->lo = lo;
    h->hi = hi;
    h->align = align;
    h->b[0] = (Block){lo, hi - lo, 0, 0};
    h->n = 1;
}

static uint32_t heap_alloc(Heap *h, uint32_t size, uint32_t *handle)
{
    int i;
    size = (size + h->align - 1) & ~(h->align - 1);
    if (size == 0) size = h->align;
    for (i = 0; i < h->n; i++) {
        if (!h->b[i].used && h->b[i].size >= size) {
            if (h->b[i].size > size && h->n < 512) {
                memmove(&h->b[i + 2], &h->b[i + 1], (size_t)(h->n - i - 1) * sizeof(Block));
                h->b[i + 1] = (Block){h->b[i].addr + size, h->b[i].size - size, 0, 0};
                h->n++;
                h->b[i].size = size;
            }
            h->b[i].used = 1;
            h->b[i].handle = g_next_handle++;
            if (handle) *handle = h->b[i].handle;
            memset(M + h->b[i].addr, 0, size);
            return h->b[i].addr;
        }
    }
    return 0;
}

static int heap_find(Heap *h, uint32_t key, int by_handle)
{
    int i;
    for (i = 0; i < h->n; i++)
        if (h->b[i].used && (by_handle ? h->b[i].handle == key : h->b[i].addr == key)) return i;
    return -1;
}

static void heap_merge(Heap *h)
{
    int i;
    for (i = 0; i + 1 < h->n;) {
        if (!h->b[i].used && !h->b[i + 1].used) {
            h->b[i].size += h->b[i + 1].size;
            memmove(&h->b[i + 1], &h->b[i + 2], (size_t)(h->n - i - 2) * sizeof(Block));
            h->n--;
        } else {
            i++;
        }
    }
}

static void heap_free_idx(Heap *h, int i)
{
    h->b[i].used = 0;
    h->b[i].handle = 0;
    heap_merge(h);
}

static uint32_t heap_largest(Heap *h)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < h->n; i++)
        if (!h->b[i].used && h->b[i].size > m) m = h->b[i].size;
    return m;
}

static uint32_t heap_total_free(Heap *h)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < h->n; i++)
        if (!h->b[i].used) m += h->b[i].size;
    return m;
}

/* resize in place when possible, else move */
static uint32_t heap_resize(Heap *h, int i, uint32_t size)
{
    uint32_t old = h->b[i].addr, oldsize = h->b[i].size, handle = h->b[i].handle, na;
    size = (size + h->align - 1) & ~(h->align - 1);
    if (size <= oldsize) {
        if (size < oldsize && h->n < 512) {
            memmove(&h->b[i + 2], &h->b[i + 1], (size_t)(h->n - i - 1) * sizeof(Block));
            h->b[i + 1] = (Block){old + size, oldsize - size, 0, 0};
            h->n++;
            h->b[i].size = size;
            heap_merge(h);
        }
        return old;
    }
    if (i + 1 < h->n && !h->b[i + 1].used && oldsize + h->b[i + 1].size >= size) {
        uint32_t extra = size - oldsize;
        memset(M + old + oldsize, 0, extra);
        h->b[i].size = size;
        h->b[i + 1].addr += extra;
        h->b[i + 1].size -= extra;
        if (h->b[i + 1].size == 0) {
            memmove(&h->b[i + 1], &h->b[i + 2], (size_t)(h->n - i - 2) * sizeof(Block));
            h->n--;
        }
        return old;
    }
    na = heap_alloc(h, size, NULL);
    if (na == 0) return 0;
    memcpy(M + na, M + old, oldsize);
    {
        int j = heap_find(h, na, 0);
        h->b[j].handle = handle;
        i = heap_find(h, old, 0);
        heap_free_idx(h, i);
    }
    return na;
}

void dpmi_init(void)
{
    uint32_t lo = (g_heap_top_hint + 0xFFFFu) & ~0xFFFFu;
    if (lo < HIGH_HEAP_BASE) lo = HIGH_HEAP_BASE;
    heap_init(&g_dos_heap, LOW_DOS_HEAP, LOW_DOS_HEAP_END, 16);
    heap_init(&g_xms_heap, lo, RT_MEM_SIZE, 16);
    memset(g_rm_vectors, 0, sizeof g_rm_vectors);
    memset(g_exc_handlers, 0, sizeof g_exc_handlers);
}

uint32_t dpmi_dos_alloc(uint32_t paragraphs, uint32_t *largest)
{
    uint32_t a = heap_alloc(&g_dos_heap, paragraphs * 16u, NULL);
    if (a == 0) {
        *largest = heap_largest(&g_dos_heap) / 16u;
        return 0;
    }
    return a >> 4;
}

static void set_ax(uint32_t v)
{
    R.eax = (R.eax & 0xFFFF0000u) | (v & 0xFFFFu);
}

static void fail(uint16_t code)
{
    set_ax(code);
    R.cf = 1;
}

/* real-mode call structure (int 31h 0300h) <-> registers for the BIOS/DOS emulation */
static void simulate_rm_int(uint8_t n, uint32_t s)
{
    Cpu save = R;
    uint16_t rm_ds = RD16(s + 0x24), rm_es = RD16(s + 0x22);
    R.edi = RD32(s + 0x00);
    R.esi = RD32(s + 0x04);
    R.ebp = RD32(s + 0x08);
    R.ebx = RD32(s + 0x10);
    R.edx = RD32(s + 0x14);
    R.ecx = RD32(s + 0x18);
    R.eax = RD32(s + 0x1C);
    R.cf = 0;
    /* addresses in DS:DX / ES:DI become linear */
    switch (n) {
    case 0x10:
        R.es_base = (uint32_t)rm_es << 4;
        R.edx = (R.edx & 0xFFFFu) + ((uint32_t)rm_es << 4);
        bios_int10();
        break;
    case 0x33:
        R.es_base = (uint32_t)rm_es << 4;
        bios_int33();
        break;
    case 0x16:
        bios_int16();
        break;
    case 0x21:
        R.edx = (R.edx & 0xFFFFu) + ((uint32_t)rm_ds << 4);
        R.es_base = (uint32_t)rm_es << 4;
        dos_int21();
        break;
    case 0x2F:
        dos_int2f();
        break;
    default:
        rt_log("DPMI 0300h: real-mode int %02x AX=%04x not emulated", n, (uint16_t)R.eax);
        R.cf = 1;
        break;
    }
    WR32(s + 0x00, R.edi);
    WR32(s + 0x04, R.esi);
    WR32(s + 0x08, R.ebp);
    WR32(s + 0x10, R.ebx);
    WR32(s + 0x14, n == 0x21 ? (R.edx & 0xFFFFu) | (RD32(s + 0x14) & 0xFFFF0000u) : R.edx);
    WR32(s + 0x18, R.ecx);
    WR32(s + 0x1C, R.eax);
    WR16(s + 0x20, (uint16_t)((RD16(s + 0x20) & ~1u) | (R.cf ? 1u : 0u) | (R.zf ? 0x40u : 0u)));
    {
        uint32_t es_base = R.es_base;
        R = save;
        (void)es_base;
    }
    R.cf = 0;
}

void dpmi_int31(void)
{
    uint16_t ax = (uint16_t)R.eax;
    uint16_t bx = (uint16_t)R.ebx;
    rt_trace("int31 ax=%04x ebx=%08x ecx=%08x edx=%08x", ax, R.ebx, R.ecx, R.edx);
    R.cf = 0;
    switch (ax) {
    case 0x0000: { /* allocate CX descriptors */
        uint16_t n = (uint16_t)R.ecx, first = 0, i;
        for (i = 0; i < n; i++) {
            uint16_t s = rt_sel_alloc(0, 0);
            if (i == 0) first = s;
        }
        if (first == 0) {
            fail(0x8011);
            return;
        }
        set_ax(first);
        return;
    }
    case 0x0001:
        rt_sel_free(bx);
        return;
    case 0x0002: { /* real-mode segment to descriptor */
        uint16_t s = rt_sel_alloc((uint32_t)bx << 4, 0xFFFF);
        set_ax(s);
        return;
    }
    case 0x0003:
        set_ax(8);
        return;
    case 0x0006: {
        uint32_t b = rt_sel_base(bx);
        R.ecx = (R.ecx & 0xFFFF0000u) | (b >> 16);
        R.edx = (R.edx & 0xFFFF0000u) | (b & 0xFFFFu);
        return;
    }
    case 0x0007:
        rt_sel_set_base(bx, ((R.ecx & 0xFFFFu) << 16) | (R.edx & 0xFFFFu));
        return;
    case 0x0008:
        rt_sel_set_limit(bx, ((R.ecx & 0xFFFFu) << 16) | (R.edx & 0xFFFFu));
        return;
    case 0x0009:
        return;
    case 0x000A: {
        uint16_t s = rt_sel_alloc(rt_sel_base(bx), rt_sel_limit(bx));
        set_ax(s);
        return;
    }
    case 0x000B: { /* get descriptor into ES:EDI */
        uint32_t d = R.es_base + R.edi, b = rt_sel_base(bx), l = rt_sel_limit(bx);
        uint32_t lim = l > 0xFFFFFu ? l >> 12 : l;
        WR16(d, lim & 0xFFFFu);
        WR16(d + 2, b & 0xFFFFu);
        WR8(d + 4, (b >> 16) & 0xFFu);
        WR8(d + 5, 0xF2);
        WR8(d + 6, ((lim >> 16) & 0x0Fu) | (l > 0xFFFFFu ? 0xC0u : 0x40u));
        WR8(d + 7, b >> 24);
        return;
    }
    case 0x000C: {
        uint32_t d = R.es_base + R.edi;
        uint32_t b = RD16(d + 2) | ((uint32_t)RD8(d + 4) << 16) | ((uint32_t)RD8(d + 7) << 24);
        uint32_t l = RD16(d) | ((uint32_t)(RD8(d + 6) & 0x0F) << 16);
        if (RD8(d + 6) & 0x80) l = (l << 12) | 0xFFFu;
        rt_sel_set_base(bx, b);
        rt_sel_set_limit(bx, l);
        return;
    }
    case 0x0100: { /* allocate DOS memory: BX paragraphs -> AX segment, DX selector */
        uint32_t largest = 0, seg = dpmi_dos_alloc(bx, &largest);
        uint16_t sel;
        if (seg == 0) {
            R.ebx = (R.ebx & 0xFFFF0000u) | largest;
            fail(0x0008);
            return;
        }
        sel = rt_sel_alloc(seg << 4, (uint32_t)bx * 16u - 1u);
        set_ax(seg);
        R.edx = (R.edx & 0xFFFF0000u) | sel;
        return;
    }
    case 0x0101: { /* free DOS memory: DX selector */
        uint16_t sel = (uint16_t)R.edx;
        int i = heap_find(&g_dos_heap, rt_sel_base(sel), 0);
        if (i >= 0) heap_free_idx(&g_dos_heap, i);
        rt_sel_free(sel);
        return;
    }
    case 0x0102: { /* resize DOS memory */
        uint16_t sel = (uint16_t)R.edx;
        int i = heap_find(&g_dos_heap, rt_sel_base(sel), 0);
        if (i < 0 || heap_resize(&g_dos_heap, i, (uint32_t)bx * 16u) != rt_sel_base(sel)) {
            fail(0x0008);
            return;
        }
        rt_sel_set_limit(sel, (uint32_t)bx * 16u - 1u);
        return;
    }
    case 0x0200:
        R.ecx = (R.ecx & 0xFFFF0000u) | (g_rm_vectors[bx & 0xFF] >> 16);
        R.edx = (R.edx & 0xFFFF0000u) | (g_rm_vectors[bx & 0xFF] & 0xFFFFu);
        return;
    case 0x0201:
        g_rm_vectors[bx & 0xFF] = ((R.ecx & 0xFFFFu) << 16) | (R.edx & 0xFFFFu);
        return;
    case 0x0202:
        R.ecx = (R.ecx & 0xFFFF0000u) | SEL_CODE;
        R.edx = g_exc_handlers[bx & 31];
        return;
    case 0x0203:
        g_exc_handlers[bx & 31] = R.edx;
        return;
    case 0x0204:
        R.ecx = (R.ecx & 0xFFFF0000u) | SEL_CODE;
        R.edx = dpmi_pm_vector_get(bx & 0xFF);
        return;
    case 0x0205:
        rt_trace("DPMI: protected-mode vector %02x -> %04x:%08x", bx & 0xFF, (uint16_t)R.ecx, R.edx);
        dpmi_set_pm_vector(bx & 0xFF, R.edx);
        return;
    case 0x0300:
        simulate_rm_int((uint8_t)bx, R.es_base + R.edi);
        return;
    case 0x0301:
    case 0x0302:
        rt_log("DPMI %04x: real-mode procedure call not emulated", ax);
        fail(0x8001);
        return;
    case 0x0303: /* allocate a real-mode callback: a dummy address */
        R.ecx = (R.ecx & 0xFFFF0000u) | 0xF000u;
        R.edx = (R.edx & 0xFFFF0000u) | 0x0100u;
        return;
    case 0x0304:
        return;
    case 0x0400:
        set_ax(0x005A);
        R.ebx = (R.ebx & 0xFFFF0000u) | 0x0003u;
        R.ecx = (R.ecx & 0xFFFFFF00u) | 4u;
        R.edx = (R.edx & 0xFFFF0000u) | 0x0870u;
        return;
    case 0x0500: { /* free memory information at ES:EDI */
        uint32_t d = R.es_base + R.edi, i;
        uint32_t largest = heap_largest(&g_xms_heap), total = heap_total_free(&g_xms_heap);
        for (i = 0; i < 48; i += 4) WR32(d + i, 0xFFFFFFFFu);
        WR32(d + 0x00, largest);
        WR32(d + 0x04, largest / 4096u);
        WR32(d + 0x08, largest / 4096u);
        WR32(d + 0x0C, (RT_MEM_SIZE - HIGH_HEAP_BASE) / 4096u);
        WR32(d + 0x10, total / 4096u);
        WR32(d + 0x14, total / 4096u);
        WR32(d + 0x18, RT_MEM_SIZE / 4096u);
        WR32(d + 0x1C, total / 4096u);
        WR32(d + 0x20, 0);
        return;
    }
    case 0x0501: { /* allocate memory block: BX:CX bytes -> BX:CX linear, SI:DI handle */
        uint32_t size = ((uint32_t)bx << 16) | (R.ecx & 0xFFFFu), handle = 0;
        uint32_t a = heap_alloc(&g_xms_heap, size, &handle);
        rt_trace("DPMI 0501h %u bytes -> %08x", size, a);
        if (a == 0) {
            fail(0x8013);
            return;
        }
        R.ebx = (R.ebx & 0xFFFF0000u) | (a >> 16);
        R.ecx = (R.ecx & 0xFFFF0000u) | (a & 0xFFFFu);
        R.esi = (R.esi & 0xFFFF0000u) | (handle >> 16);
        R.edi = (R.edi & 0xFFFF0000u) | (handle & 0xFFFFu);
        return;
    }
    case 0x0502: {
        uint32_t handle = ((R.esi & 0xFFFFu) << 16) | (R.edi & 0xFFFFu);
        int i = heap_find(&g_xms_heap, handle, 1);
        if (i < 0) {
            fail(0x8023);
            return;
        }
        heap_free_idx(&g_xms_heap, i);
        return;
    }
    case 0x0503: {
        uint32_t handle = ((R.esi & 0xFFFFu) << 16) | (R.edi & 0xFFFFu);
        uint32_t size = ((uint32_t)bx << 16) | (R.ecx & 0xFFFFu), a;
        int i = heap_find(&g_xms_heap, handle, 1);
        if (i < 0) {
            fail(0x8023);
            return;
        }
        a = heap_resize(&g_xms_heap, i, size);
        if (a == 0) {
            fail(0x8013);
            return;
        }
        R.ebx = (R.ebx & 0xFFFF0000u) | (a >> 16);
        R.ecx = (R.ecx & 0xFFFF0000u) | (a & 0xFFFFu);
        return;
    }
    case 0x0600:
    case 0x0601:
    case 0x0602:
    case 0x0603:
    case 0x0702:
    case 0x0703:
        return;
    case 0x0604:
        R.ebx &= 0xFFFF0000u;
        R.ecx = (R.ecx & 0xFFFF0000u) | 4096u;
        return;
    case 0x0800: /* physical address mapping: the first megabyte is identity-mapped */
        return;
    case 0x0801:
        return;
    case 0x0900:
        R.eax = (R.eax & 0xFFFFFF00u) | R.iflag;
        R.iflag = 0;
        return;
    case 0x0901:
        R.eax = (R.eax & 0xFFFFFF00u) | R.iflag;
        R.iflag = 1;
        return;
    case 0x0902:
        R.eax = (R.eax & 0xFFFFFF00u) | R.iflag;
        return;
    case 0x0A00: /* vendor API entry */
        fail(0x8001);
        return;
    default:
        rt_log("int 31h: unhandled AX=%04x", ax);
        fail(0x8001);
        return;
    }
}
