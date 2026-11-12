/* recomp.h - what the recompiled guest code (gen/<module>/*.c) needs: the emulated CPU state, the guest
 * memory, the arithmetic/flag helpers and the calls into the runtime (interrupts, ports, indirect calls).
 *
 * Portable C11: no OS headers, no compiler-specific code except the inline hints.  The guest is a 32-bit
 * flat-model DOS/4GW program; every guest address is an offset into M (one linear space, as under DOS/4GW
 * where the flat selectors have base 0).  The host is little-endian (x86-64, arm64). */
#pragma once

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define RT_INLINE static __forceinline
#define RT_NORETURN __declspec(noreturn)
#else
#define RT_INLINE static inline __attribute__((always_inline))
#define RT_NORETURN __attribute__((noreturn))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CPU state ---------------------------------------------------------------------------------------- */
typedef struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, pf, af, df, iflag;
    uint16_t es, cs, ss, ds, fs, gs;
    uint32_t es_base, fs_base, gs_base, ds_base;
} Cpu;

extern Cpu R;
extern uint8_t *M;  /* guest linear memory */

enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };
enum { F_C = 1, F_Z = 2, F_S = 4, F_O = 8, F_P = 16, F_A = 32 };

#define RT_MEM_SIZE (64u << 20)
#define RT_MEM_MASK (RT_MEM_SIZE - 1u)

/* ---- runtime services (rt/*.c) -------------------------------------------------------------------------- */
typedef void (*RtFn)(void);
typedef struct RtFunc {
    uint32_t addr;
    RtFn fn;
} RtFunc;
typedef struct RtModuleDef {
    const char *exe_name;
    const char *sha1;
    uint32_t entry;
    uint32_t stack;
    const RtFunc *funcs;
    uint32_t nfuncs;
    /* relocatable images only (AIL/32 drivers that the program loads itself): the current load delta, the
     * link-time object, and per function 8 code bytes + 8 mask bytes used to find where the image landed */
    uint32_t *delta;
    uint32_t link_base;
    uint32_t link_size;
    const uint8_t *sig;
} RtModuleDef;

void rt_call_ind(uint32_t addr);
void rt_jump_ind(uint32_t addr);
void rt_int(uint32_t n);
void rt_iret(void);
void rt_retf(uint32_t n);
void rt_hlt(void);
void rt_cli(void);
void rt_sti(void);
uint32_t rt_get_eflags(void);
void rt_set_eflags(uint32_t v);
void rt_load_seg(int seg, uint16_t sel);
uint8_t rt_in8(uint16_t port);
uint16_t rt_in16(uint16_t port);
uint32_t rt_in32(uint16_t port);
void rt_out8(uint16_t port, uint8_t v);
void rt_out16(uint16_t port, uint16_t v);
void rt_out32(uint16_t port, uint32_t v);
void rt_unimplemented(uint32_t addr, const char *what);
RT_NORETURN void rt_fatal(const char *fmt, ...);
void rt_poll(void);
void rt_divide_error(void);

uint8_t vga_read8(uint32_t a);
void vga_write8(uint32_t a, uint8_t v);

extern int32_t rt_poll_budget;
extern const uint8_t rt_parity[256];

#ifdef RT_TRACE_CALLS
void rt_trace_enter(uint32_t addr);
void rt_trace_leave(uint32_t addr);
#define RT_ENTER(a) rt_trace_enter(a)
#define RT_LEAVE(a) rt_trace_leave(a)
#else
#define RT_ENTER(a) ((void)0)
#define RT_LEAVE(a) ((void)0)
#endif

#define POLL()                       \
    do {                             \
        if (--rt_poll_budget <= 0) { \
            rt_poll();               \
        }                            \
    } while (0)

/* ---- memory --------------------------------------------------------------------------------------------- */
#define RT_IS_VGA(a) ((uint32_t)((a) - 0xA0000u) < 0x20000u)

RT_INLINE uint8_t RD8(uint32_t a)
{
    if (RT_IS_VGA(a)) return vga_read8(a);
    return M[a & RT_MEM_MASK];
}
RT_INLINE uint16_t RD16(uint32_t a)
{
    uint16_t v;
    if (RT_IS_VGA(a) || RT_IS_VGA(a + 1u)) return (uint16_t)(RD8(a) | (RD8(a + 1u) << 8));
    memcpy(&v, M + (a & RT_MEM_MASK), 2);
    return v;
}
RT_INLINE uint32_t RD32(uint32_t a)
{
    uint32_t v;
    if (RT_IS_VGA(a) || RT_IS_VGA(a + 3u))
        return (uint32_t)RD8(a) | ((uint32_t)RD8(a + 1u) << 8) | ((uint32_t)RD8(a + 2u) << 16) |
               ((uint32_t)RD8(a + 3u) << 24);
    memcpy(&v, M + (a & RT_MEM_MASK), 4);
    return v;
}
RT_INLINE void WR8(uint32_t a, uint32_t v)
{
    if (RT_IS_VGA(a)) {
        vga_write8(a, (uint8_t)v);
        return;
    }
    M[a & RT_MEM_MASK] = (uint8_t)v;
}
RT_INLINE void WR16(uint32_t a, uint32_t v)
{
    uint16_t w = (uint16_t)v;
    if (RT_IS_VGA(a) || RT_IS_VGA(a + 1u)) {
        WR8(a, w & 0xFFu);
        WR8(a + 1u, w >> 8);
        return;
    }
    memcpy(M + (a & RT_MEM_MASK), &w, 2);
}
RT_INLINE void WR32(uint32_t a, uint32_t v)
{
    if (RT_IS_VGA(a) || RT_IS_VGA(a + 3u)) {
        WR8(a, v & 0xFFu);
        WR8(a + 1u, (v >> 8) & 0xFFu);
        WR8(a + 2u, (v >> 16) & 0xFFu);
        WR8(a + 3u, v >> 24);
        return;
    }
    memcpy(M + (a & RT_MEM_MASK), &v, 4);
}

/* ---- stack ---------------------------------------------------------------------------------------------- */
RT_INLINE void PUSH32(uint32_t v)
{
    R.esp -= 4u;
    WR32(R.esp, v);
}
RT_INLINE void PUSH16(uint32_t v)
{
    R.esp -= 2u;
    WR16(R.esp, v);
}
RT_INLINE uint32_t POP32(void)
{
    uint32_t v = RD32(R.esp);
    R.esp += 4u;
    return v;
}
RT_INLINE uint32_t POP16(void)
{
    uint32_t v = RD16(R.esp);
    R.esp += 2u;
    return v;
}

/* ---- flags ---------------------------------------------------------------------------------------------- */
#define RT_SZP(r, bits, need)                                                   \
    do {                                                                        \
        if ((need) & F_Z) R.zf = (uint8_t)(((r) & RT_M##bits) == 0u);           \
        if ((need) & F_S) R.sf = (uint8_t)(((r) >> ((bits) - 1)) & 1u);         \
        if ((need) & F_P) R.pf = rt_parity[(r) & 0xFFu];                        \
    } while (0)

#define RT_M8 0xFFu
#define RT_M16 0xFFFFu
#define RT_M32 0xFFFFFFFFu

#define RT_DEFINE_ALU(bits, T)                                                                         \
    RT_INLINE uint32_t ADD##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t r = (T)(a + b);                                                                       \
        a = (T)a;                                                                                      \
        b = (T)b;                                                                                      \
        if (need & F_C) R.cf = (uint8_t)(r < a);                                                       \
        if (need & F_O) R.of = (uint8_t)(((~(a ^ b) & (a ^ r)) >> ((bits) - 1)) & 1u);                 \
        if (need & F_A) R.af = (uint8_t)(((a ^ b ^ r) >> 4) & 1u);                                     \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t SUB##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t r = (T)(a - b);                                                                       \
        a = (T)a;                                                                                      \
        b = (T)b;                                                                                      \
        if (need & F_C) R.cf = (uint8_t)(a < b);                                                       \
        if (need & F_O) R.of = (uint8_t)((((a ^ b) & (a ^ r)) >> ((bits) - 1)) & 1u);                  \
        if (need & F_A) R.af = (uint8_t)(((a ^ b ^ r) >> 4) & 1u);                                     \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t ADC##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t c = R.cf;                                                                             \
        uint32_t r = (T)(a + b + c);                                                                   \
        a = (T)a;                                                                                      \
        b = (T)b;                                                                                      \
        if (need & F_C) R.cf = (uint8_t)(c ? r <= a : r < a);                                          \
        if (need & F_O) R.of = (uint8_t)(((~(a ^ b) & (a ^ r)) >> ((bits) - 1)) & 1u);                 \
        if (need & F_A) R.af = (uint8_t)(((a ^ b ^ r) >> 4) & 1u);                                     \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t SBB##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t c = R.cf;                                                                             \
        uint32_t r = (T)(a - b - c);                                                                   \
        a = (T)a;                                                                                      \
        b = (T)b;                                                                                      \
        if (need & F_C) R.cf = (uint8_t)(c ? a <= b : a < b);                                          \
        if (need & F_O) R.of = (uint8_t)((((a ^ b) & (a ^ r)) >> ((bits) - 1)) & 1u);                  \
        if (need & F_A) R.af = (uint8_t)(((a ^ b ^ r) >> 4) & 1u);                                     \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE void rt_flags_logic##bits(uint32_t r, int need)                                          \
    {                                                                                                  \
        if (need & F_C) R.cf = 0;                                                                      \
        if (need & F_O) R.of = 0;                                                                      \
        if (need & F_A) R.af = 0;                                                                      \
        RT_SZP(r, bits, need);                                                                         \
    }                                                                                                  \
    RT_INLINE uint32_t AND##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t r = (T)(a & b);                                                                       \
        rt_flags_logic##bits(r, need);                                                                 \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t OR##bits(uint32_t a, uint32_t b, int need)                                      \
    {                                                                                                  \
        uint32_t r = (T)(a | b);                                                                       \
        rt_flags_logic##bits(r, need);                                                                 \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t XOR##bits(uint32_t a, uint32_t b, int need)                                     \
    {                                                                                                  \
        uint32_t r = (T)(a ^ b);                                                                       \
        rt_flags_logic##bits(r, need);                                                                 \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t INC##bits(uint32_t a, int need)                                                 \
    {                                                                                                  \
        uint8_t c = R.cf;                                                                              \
        uint32_t r = ADD##bits(a, 1u, need);                                                           \
        R.cf = c;                                                                                      \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t DEC##bits(uint32_t a, int need)                                                 \
    {                                                                                                  \
        uint8_t c = R.cf;                                                                              \
        uint32_t r = SUB##bits(a, 1u, need);                                                           \
        R.cf = c;                                                                                      \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t SHL##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = cnt & 31u, r;                                                                     \
        a = (T)a;                                                                                      \
        if (c == 0u) return a;                                                                         \
        r = (c < 32u) ? (T)(a << c) : 0u;                                                              \
        if (need & F_C) R.cf = (uint8_t)(c <= (bits) ? ((a >> ((bits) - c)) & 1u) : 0u);               \
        if (need & F_O) R.of = (uint8_t)(((r >> ((bits) - 1)) & 1u) ^ R.cf);                           \
        if (need & F_A) R.af = 0;                                                                      \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t SHR##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = cnt & 31u, r;                                                                     \
        a = (T)a;                                                                                      \
        if (c == 0u) return a;                                                                         \
        r = (c < (bits)) ? (a >> c) : 0u;                                                              \
        if (need & F_C) R.cf = (uint8_t)(c <= (bits) ? ((a >> (c - 1u)) & 1u) : 0u);                   \
        if (need & F_O) R.of = (uint8_t)((a >> ((bits) - 1)) & 1u);                                    \
        if (need & F_A) R.af = 0;                                                                      \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t SAR##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = cnt & 31u, r;                                                                     \
        int32_t s = (int32_t)(T##_S)a;                                                                 \
        if (c == 0u) return (T)a;                                                                      \
        r = (T)(uint32_t)(s >> (c < 32u ? c : 31u));                                                   \
        if (need & F_C) R.cf = (uint8_t)(((uint32_t)(s >> (c - 1u < 31u ? c - 1u : 31u))) & 1u);       \
        if (need & F_O) R.of = 0;                                                                      \
        if (need & F_A) R.af = 0;                                                                      \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t ROL##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = cnt & 31u, k, r;                                                                  \
        a = (T)a;                                                                                      \
        if (c == 0u) return a;                                                                         \
        k = c % (bits);                                                                                \
        r = k ? (T)((a << k) | (a >> ((bits) - k))) : a;                                               \
        if (need & F_C) R.cf = (uint8_t)(r & 1u);                                                      \
        if (need & F_O) R.of = (uint8_t)(((r >> ((bits) - 1)) & 1u) ^ (r & 1u));                       \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t ROR##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = cnt & 31u, k, r;                                                                  \
        a = (T)a;                                                                                      \
        if (c == 0u) return a;                                                                         \
        k = c % (bits);                                                                                \
        r = k ? (T)((a >> k) | (a << ((bits) - k))) : a;                                               \
        if (need & F_C) R.cf = (uint8_t)((r >> ((bits) - 1)) & 1u);                                    \
        if (need & F_O) R.of = (uint8_t)(((r >> ((bits) - 1)) ^ (r >> ((bits) - 2))) & 1u);            \
        return r;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t RCL##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = (cnt & 31u) % ((bits) + 1), i;                                                    \
        uint32_t cf = R.cf;                                                                            \
        a = (T)a;                                                                                      \
        if ((cnt & 31u) == 0u) return a;                                                               \
        for (i = 0; i < c; i++) {                                                                      \
            uint32_t out = (a >> ((bits) - 1)) & 1u;                                                   \
            a = (T)((a << 1) | cf);                                                                    \
            cf = out;                                                                                  \
        }                                                                                              \
        R.cf = (uint8_t)cf;                                                                            \
        if (need & F_O) R.of = (uint8_t)(((a >> ((bits) - 1)) & 1u) ^ cf);                             \
        return a;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t RCR##bits(uint32_t a, uint32_t cnt, int need)                                   \
    {                                                                                                  \
        uint32_t c = (cnt & 31u) % ((bits) + 1), i;                                                    \
        uint32_t cf = R.cf;                                                                            \
        a = (T)a;                                                                                      \
        if ((cnt & 31u) == 0u) return a;                                                               \
        if (need & F_O) R.of = (uint8_t)(((a >> ((bits) - 1)) & 1u) ^ cf);                             \
        for (i = 0; i < c; i++) {                                                                      \
            uint32_t out = a & 1u;                                                                     \
            a = (T)((a >> 1) | (cf << ((bits) - 1)));                                                  \
            cf = out;                                                                                  \
        }                                                                                              \
        R.cf = (uint8_t)cf;                                                                            \
        return a;                                                                                      \
    }                                                                                                  \
    RT_INLINE uint32_t IMUL##bits(uint32_t a, uint32_t b, int need)                                    \
    {                                                                                                  \
        int64_t p = (int64_t)(T##_S)a * (int64_t)(T##_S)b;                                             \
        uint32_t r = (T)(uint64_t)p;                                                                   \
        if (need & (F_C | F_O)) R.cf = R.of = (uint8_t)(p != (int64_t)(T##_S)r);                       \
        RT_SZP(r, bits, need);                                                                         \
        return r;                                                                                      \
    }

typedef int8_t uint8_t_S;
typedef int16_t uint16_t_S;
typedef int32_t uint32_t_S;
RT_DEFINE_ALU(8, uint8_t)
RT_DEFINE_ALU(16, uint16_t)
RT_DEFINE_ALU(32, uint32_t)

/* double-precision shifts (32-bit operands; the count is taken modulo 32, flags unchanged for 0) */
RT_INLINE uint32_t rt_shld32(uint32_t a, uint32_t b, uint32_t c, int need)
{
    uint32_t r;
    c &= 31u;
    if (c == 0) return a;
    r = (a << c) | (b >> (32u - c));
    if (need & F_C) R.cf = (a >> (32u - c)) & 1u;
    if (need & F_O) R.of = ((r ^ a) >> 31) & 1u;
    RT_SZP(r, 32, need);
    return r;
}
RT_INLINE uint32_t rt_shrd32(uint32_t a, uint32_t b, uint32_t c, int need)
{
    uint32_t r;
    c &= 31u;
    if (c == 0) return a;
    r = (a >> c) | (b << (32u - c));
    if (need & F_C) R.cf = (a >> (c - 1u)) & 1u;
    if (need & F_O) R.of = ((r ^ a) >> 31) & 1u;
    RT_SZP(r, 32, need);
    return r;
}

RT_INLINE uint32_t rt_bsr(uint32_t v)
{
    uint32_t r = 31u;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        r--;
    }
    return r;
}

/* one-operand multiply / divide */
RT_INLINE void IMUL1_8(uint32_t b, int need)
{
    int16_t p = (int16_t)((int8_t)R.eax * (int8_t)b);
    R.eax = (R.eax & 0xFFFF0000u) | (uint16_t)p;
    if (need) R.cf = R.of = (uint8_t)(p != (int8_t)p);
}
RT_INLINE void IMUL1_16(uint32_t b, int need)
{
    int32_t p = (int32_t)(int16_t)R.eax * (int16_t)b;
    R.eax = (R.eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFFu);
    R.edx = (R.edx & 0xFFFF0000u) | (((uint32_t)p >> 16) & 0xFFFFu);
    if (need) R.cf = R.of = (uint8_t)(p != (int16_t)p);
}
RT_INLINE void IMUL1_32(uint32_t b, int need)
{
    int64_t p = (int64_t)(int32_t)R.eax * (int32_t)b;
    R.eax = (uint32_t)(uint64_t)p;
    R.edx = (uint32_t)((uint64_t)p >> 32);
    if (need) R.cf = R.of = (uint8_t)(p != (int32_t)p);
}
RT_INLINE void MUL8(uint32_t b, int need)
{
    uint32_t p = (R.eax & 0xFFu) * (b & 0xFFu);
    R.eax = (R.eax & 0xFFFF0000u) | p;
    if (need) R.cf = R.of = (uint8_t)((p >> 8) != 0u);
}
RT_INLINE void MUL16(uint32_t b, int need)
{
    uint32_t p = (R.eax & 0xFFFFu) * (b & 0xFFFFu);
    R.eax = (R.eax & 0xFFFF0000u) | (p & 0xFFFFu);
    R.edx = (R.edx & 0xFFFF0000u) | (p >> 16);
    if (need) R.cf = R.of = (uint8_t)((p >> 16) != 0u);
}
RT_INLINE void MUL32(uint32_t b, int need)
{
    uint64_t p = (uint64_t)R.eax * b;
    R.eax = (uint32_t)p;
    R.edx = (uint32_t)(p >> 32);
    if (need) R.cf = R.of = (uint8_t)(R.edx != 0u);
}
RT_INLINE void DIV8(uint32_t b, int need)
{
    uint32_t n = R.eax & 0xFFFFu, q;
    (void)need;
    b &= 0xFFu;
    if (b == 0u || (q = n / b) > 0xFFu) {
        rt_divide_error();
        return;
    }
    R.eax = (R.eax & 0xFFFF0000u) | ((n % b) << 8) | q;
}
RT_INLINE void DIV16(uint32_t b, int need)
{
    uint32_t n = ((R.edx & 0xFFFFu) << 16) | (R.eax & 0xFFFFu), q;
    (void)need;
    b &= 0xFFFFu;
    if (b == 0u || (q = n / b) > 0xFFFFu) {
        rt_divide_error();
        return;
    }
    R.eax = (R.eax & 0xFFFF0000u) | q;
    R.edx = (R.edx & 0xFFFF0000u) | (n % b);
}
RT_INLINE void DIV32(uint32_t b, int need)
{
    uint64_t n = ((uint64_t)R.edx << 32) | R.eax, q;
    (void)need;
    if (b == 0u || (q = n / b) > 0xFFFFFFFFu) {
        rt_divide_error();
        return;
    }
    R.eax = (uint32_t)q;
    R.edx = (uint32_t)(n % b);
}
RT_INLINE void IDIV8(uint32_t b, int need)
{
    int32_t n = (int16_t)R.eax, d = (int8_t)b, q;
    (void)need;
    if (d == 0 || (q = n / d) > 127 || q < -128) {
        rt_divide_error();
        return;
    }
    R.eax = (R.eax & 0xFFFF0000u) | ((uint32_t)(uint8_t)(n % d) << 8) | (uint8_t)q;
}
RT_INLINE void IDIV16(uint32_t b, int need)
{
    int32_t n = (int32_t)(((R.edx & 0xFFFFu) << 16) | (R.eax & 0xFFFFu)), d = (int16_t)b, q;
    (void)need;
    if (d == 0 || (q = n / d) > 32767 || q < -32768) {
        rt_divide_error();
        return;
    }
    R.eax = (R.eax & 0xFFFF0000u) | (uint16_t)q;
    R.edx = (R.edx & 0xFFFF0000u) | (uint16_t)(n % d);
}
RT_INLINE void IDIV32(uint32_t b, int need)
{
    int64_t n = (int64_t)(((uint64_t)R.edx << 32) | R.eax), d = (int32_t)b, q;
    (void)need;
    if (d == 0 || (q = n / d) > 2147483647LL || q < -2147483648LL) {
        rt_divide_error();
        return;
    }
    R.eax = (uint32_t)(int32_t)q;
    R.edx = (uint32_t)(int32_t)(n % d);
}

/* ---- string instructions (rt/strings.c) ----------------------------------------------------------------- */
void rt_movs(int size, int rep, uint32_t sbase);
void rt_stos(int size, int rep);
void rt_lods(int size, int rep, uint32_t sbase);
void rt_scas(int size, int rep, int need);  /* rep: 0 none, 1 repe, 2 repne */
void rt_cmps(int size, int rep, uint32_t sbase, int need);
void rt_outs(int size, int rep, uint32_t sbase);
void rt_ins(int size, int rep);

#define MOVSB(s, n) rt_movs(1, 0, s)
#define MOVSW(s, n) rt_movs(2, 0, s)
#define MOVSD(s, n) rt_movs(4, 0, s)
#define REP_MOVSB(s, n) rt_movs(1, 1, s)
#define REP_MOVSW(s, n) rt_movs(2, 1, s)
#define REP_MOVSD(s, n) rt_movs(4, 1, s)
#define STOSB(s, n) rt_stos(1, 0)
#define STOSW(s, n) rt_stos(2, 0)
#define STOSD(s, n) rt_stos(4, 0)
#define REP_STOSB(s, n) rt_stos(1, 1)
#define REP_STOSW(s, n) rt_stos(2, 1)
#define REP_STOSD(s, n) rt_stos(4, 1)
#define LODSB(s, n) rt_lods(1, 0, s)
#define LODSW(s, n) rt_lods(2, 0, s)
#define LODSD(s, n) rt_lods(4, 0, s)
#define REP_LODSB(s, n) rt_lods(1, 1, s)
#define SCASB(s, n) rt_scas(1, 0, n)
#define SCASW(s, n) rt_scas(2, 0, n)
#define SCASD(s, n) rt_scas(4, 0, n)
#define REPE_SCASB(s, n) rt_scas(1, 1, n)
#define REPNE_SCASB(s, n) rt_scas(1, 2, n)
#define REPE_SCASW(s, n) rt_scas(2, 1, n)
#define REPNE_SCASW(s, n) rt_scas(2, 2, n)
#define REPE_SCASD(s, n) rt_scas(4, 1, n)
#define REPNE_SCASD(s, n) rt_scas(4, 2, n)
#define CMPSB(s, n) rt_cmps(1, 0, s, n)
#define CMPSW(s, n) rt_cmps(2, 0, s, n)
#define CMPSD(s, n) rt_cmps(4, 0, s, n)
#define REPE_CMPSB(s, n) rt_cmps(1, 1, s, n)
#define REPNE_CMPSB(s, n) rt_cmps(1, 2, s, n)
#define REPE_CMPSD(s, n) rt_cmps(4, 1, s, n)
#define REPNE_CMPSD(s, n) rt_cmps(4, 2, s, n)
#define REPE_CMPSW(s, n) rt_cmps(2, 1, s, n)
#define OUTSB(s, n) rt_outs(1, 0, s)
#define OUTSW(s, n) rt_outs(2, 0, s)
#define OUTSD(s, n) rt_outs(4, 0, s)
#define REP_OUTSB(s, n) rt_outs(1, 1, s)
#define REP_OUTSW(s, n) rt_outs(2, 1, s)
#define REP_OUTSD(s, n) rt_outs(4, 1, s)
#define INSB(s, n) rt_ins(1, 0)
#define INSW(s, n) rt_ins(2, 0)
#define INSD(s, n) rt_ins(4, 0)
#define REP_INSB(s, n) rt_ins(1, 1)
#define REP_INSD(s, n) rt_ins(4, 1)

#ifdef __cplusplus
}
#endif
