/* strings.c - the x86 string instructions (movs, stos, lods, scas, cmps, ins, outs) with and without rep
 * prefixes.  The destination is ES:EDI (R.es_base), the source DS:ESI (base 0) or an overridden segment.
 * Large forward copies and fills outside the video window take a memcpy/memset fast path. */
#include <string.h>

#include "rt.h"

static uint32_t rd(int size, uint32_t a)
{
    return size == 1 ? RD8(a) : size == 2 ? RD16(a) : RD32(a);
}

static void wr(int size, uint32_t a, uint32_t v)
{
    if (size == 1)
        WR8(a, v);
    else if (size == 2)
        WR16(a, v);
    else
        WR32(a, v);
}

static int plain(uint32_t a, uint32_t n)
{
    return !(a < 0xC0000u && a + n > 0xA0000u) && a + n <= RT_MEM_SIZE && a + n >= a;
}

void rt_movs(int size, int rep, uint32_t sbase)
{
    int step = R.df ? -size : size;
    if (!rep) {
        wr(size, R.es_base + R.edi, rd(size, sbase + R.esi));
        R.esi += (uint32_t)step;
        R.edi += (uint32_t)step;
        return;
    }
    if (!R.df && R.ecx) {
        uint32_t n = R.ecx * (uint32_t)size, s = sbase + R.esi, d = R.es_base + R.edi;
        /* forward copy semantics: memmove differs only when the destination starts inside the source */
        if (plain(s, n) && plain(d, n) && !(d > s && d < s + n)) {
            memcpy(M + d, M + s, n);
            R.esi += n;
            R.edi += n;
            R.ecx = 0;
            return;
        }
    }
    while (R.ecx) {
        wr(size, R.es_base + R.edi, rd(size, sbase + R.esi));
        R.esi += (uint32_t)step;
        R.edi += (uint32_t)step;
        R.ecx--;
    }
}

void rt_stos(int size, int rep)
{
    int step = R.df ? -size : size;
    uint32_t v = size == 1 ? (R.eax & 0xFFu) : size == 2 ? (R.eax & 0xFFFFu) : R.eax;
    if (!rep) {
        wr(size, R.es_base + R.edi, v);
        R.edi += (uint32_t)step;
        return;
    }
    if (!R.df && R.ecx) {
        uint32_t n = R.ecx * (uint32_t)size, d = R.es_base + R.edi;
        if (plain(d, n)) {
            if (size == 1 || v == 0u || (size == 2 && ((v & 0xFF) == (v >> 8))) ||
                (size == 4 && v == (v & 0xFFu) * 0x01010101u)) {
                memset(M + d, (int)(v & 0xFF), n);
            } else {
                uint32_t i;
                for (i = 0; i < n; i += (uint32_t)size) memcpy(M + d + i, &v, (size_t)size);
            }
            R.edi += n;
            R.ecx = 0;
            return;
        }
    }
    while (R.ecx) {
        wr(size, R.es_base + R.edi, v);
        R.edi += (uint32_t)step;
        R.ecx--;
    }
}

void rt_lods(int size, int rep, uint32_t sbase)
{
    int step = R.df ? -size : size;
    do {
        uint32_t v;
        if (rep && R.ecx == 0) break;
        v = rd(size, sbase + R.esi);
        if (size == 1)
            R.eax = (R.eax & 0xFFFFFF00u) | v;
        else if (size == 2)
            R.eax = (R.eax & 0xFFFF0000u) | v;
        else
            R.eax = v;
        R.esi += (uint32_t)step;
        if (rep) R.ecx--;
    } while (rep);
}

static void cmp_flags(int size, uint32_t a, uint32_t b, int need)
{
    need |= F_Z;
    if (size == 1)
        (void)SUB8(a, b, need);
    else if (size == 2)
        (void)SUB16(a, b, need);
    else
        (void)SUB32(a, b, need);
}

void rt_scas(int size, int rep, int need)
{
    int step = R.df ? -size : size;
    uint32_t a = size == 1 ? (R.eax & 0xFFu) : size == 2 ? (R.eax & 0xFFFFu) : R.eax;
    if (!rep) {
        cmp_flags(size, a, rd(size, R.es_base + R.edi), need | F_C | F_S | F_O | F_Z);
        R.edi += (uint32_t)step;
        return;
    }
    while (R.ecx) {
        cmp_flags(size, a, rd(size, R.es_base + R.edi), need | F_C | F_S | F_O | F_Z);
        R.edi += (uint32_t)step;
        R.ecx--;
        if (rep == 1 && !R.zf) break;
        if (rep == 2 && R.zf) break;
    }
}

void rt_cmps(int size, int rep, uint32_t sbase, int need)
{
    int step = R.df ? -size : size;
    if (!rep) {
        cmp_flags(size, rd(size, sbase + R.esi), rd(size, R.es_base + R.edi), need | F_C | F_S | F_O | F_Z);
        R.esi += (uint32_t)step;
        R.edi += (uint32_t)step;
        return;
    }
    while (R.ecx) {
        cmp_flags(size, rd(size, sbase + R.esi), rd(size, R.es_base + R.edi), need | F_C | F_S | F_O | F_Z);
        R.esi += (uint32_t)step;
        R.edi += (uint32_t)step;
        R.ecx--;
        if (rep == 1 && !R.zf) break;
        if (rep == 2 && R.zf) break;
    }
}

void rt_outs(int size, int rep, uint32_t sbase)
{
    int step = R.df ? -size : size;
    do {
        uint16_t port = (uint16_t)R.edx;
        if (rep && R.ecx == 0) break;
        if (size == 1)
            rt_out8(port, (uint8_t)rd(1, sbase + R.esi));
        else if (size == 2)
            rt_out16(port, (uint16_t)rd(2, sbase + R.esi));
        else
            rt_out32(port, rd(4, sbase + R.esi));
        R.esi += (uint32_t)step;
        if (rep) R.ecx--;
    } while (rep);
}

void rt_ins(int size, int rep)
{
    int step = R.df ? -size : size;
    do {
        uint16_t port = (uint16_t)R.edx;
        if (rep && R.ecx == 0) break;
        if (size == 1)
            wr(1, R.es_base + R.edi, rt_in8(port));
        else if (size == 2)
            wr(2, R.es_base + R.edi, rt_in16(port));
        else
            wr(4, R.es_base + R.edi, rt_in32(port));
        R.edi += (uint32_t)step;
        if (rep) R.ecx--;
    } while (rep);
}
