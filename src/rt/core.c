/* core.c - the CPU side of the runtime: registers, eflags, selectors, indirect calls, interrupts and
 * ports, the poll loop (timer/keyboard interrupts, frame presentation), and running a recompiled program
 * as a DOS/4GW process (loading its LE image, nesting for DOS EXEC). */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "sha1.h"
#include "vfs.h"

Cpu R;
uint8_t *M;
int32_t rt_poll_budget = 1;
int rt_verbose = 0;
int rt_exit_code;
int rt_in_irq;
char rt_game_dir[1024];
char rt_user_dir[1024];

const uint8_t rt_parity[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

static void dump_ring(void);

/* ---- logging ------------------------------------------------------------------------------------------- */
void rt_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    host_log(buf);
}

void rt_trace(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (rt_verbose < 2) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    host_log(buf);
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    host_log(buf);
    rt_log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x", R.eax, R.ebx, R.ecx,
           R.edx, R.esi, R.edi, R.ebp, R.esp);
    dump_ring();
    host_fatal(buf);
}

void rt_unimplemented(uint32_t addr, const char *what)
{
    rt_fatal("unimplemented instruction at %06x: %s", addr, what);
}

void rt_divide_error(void)
{
    rt_fatal("divide error (eax=%08x edx=%08x)", R.eax, R.edx);
}

#ifdef RT_TRACE_CALLS
/* the last function entries, printed by rt_fatal */
static uint32_t g_ring[64], g_ring_esp[64];
static unsigned g_ring_pos;
static uint32_t g_shadow_esp[4096], g_shadow_fn[4096];
static int g_shadow_n;
static int g_shadow_reported;
void rt_trace_enter(uint32_t addr)
{
    if (g_shadow_n < 4096) {
        g_shadow_esp[g_shadow_n] = R.esp;
        g_shadow_fn[g_shadow_n++] = addr;
    }
    g_ring[g_ring_pos & 63] = addr;
    g_ring_esp[g_ring_pos & 63] = R.esp;
    g_ring_pos++;
    if (rt_verbose >= 3) rt_log("> %06x esp=%08x eax=%08x edx=%08x ebx=%08x ecx=%08x", addr, R.esp, R.eax, R.edx, R.ebx, R.ecx);
}
void rt_trace_leave(uint32_t addr)
{
    /* the stack must be where it was on entry (tail calls leave entries with the same esp behind) */
    int k = g_shadow_n - 1;
    while (k >= 0 && g_shadow_esp[k] != R.esp && g_shadow_esp[k] < R.esp + 64u) k--;
    if (k >= 0 && g_shadow_esp[k] == R.esp) {
        g_shadow_n = k;
    } else if (g_shadow_n > 0 && g_shadow_reported < 10) {
        g_shadow_reported++;
        rt_log("STACK IMBALANCE: ret at %06x esp=%08x, function %06x entered with esp=%08x", addr, R.esp,
               g_shadow_fn[g_shadow_n - 1], g_shadow_esp[g_shadow_n - 1]);
        g_shadow_n--;
    }
    if (rt_verbose >= 3) rt_log("< %06x esp=%08x ret=%08x eax=%08x", addr, R.esp, RD32(R.esp), R.eax);
}
static void dump_ring(void)
{
    unsigned i;
    for (i = 0; i < 64; i++) {
        unsigned k = (g_ring_pos + i) & 63;
        if (g_ring[k]) rt_log("  call %06x esp=%08x", g_ring[k], g_ring_esp[k]);
    }
}
#else
static void dump_ring(void)
{
}
#endif

/* ---- eflags ---------------------------------------------------------------------------------------------- */
uint32_t rt_get_eflags(void)
{
    return 0x0002u | (uint32_t)R.cf | ((uint32_t)R.pf << 2) | ((uint32_t)R.af << 4) | ((uint32_t)R.zf << 6) |
           ((uint32_t)R.sf << 7) | ((uint32_t)R.iflag << 9) | ((uint32_t)R.df << 10) | ((uint32_t)R.of << 11);
}

void rt_set_eflags(uint32_t v)
{
    R.cf = (uint8_t)(v & 1u);
    R.pf = (uint8_t)((v >> 2) & 1u);
    R.af = (uint8_t)((v >> 4) & 1u);
    R.zf = (uint8_t)((v >> 6) & 1u);
    R.sf = (uint8_t)((v >> 7) & 1u);
    R.iflag = (uint8_t)((v >> 9) & 1u);
    R.df = (uint8_t)((v >> 10) & 1u);
    R.of = (uint8_t)((v >> 11) & 1u);
}

void rt_cli(void)
{
    R.iflag = 0;
}

void rt_sti(void)
{
    R.iflag = 1;
}

void rt_hlt(void)
{
    rt_fatal("hlt");
}

/* ---- selectors (an LDT of 8192 descriptors) ----------------------------------------------------------- */
typedef struct Desc {
    uint32_t base, limit;
    uint8_t used;
} Desc;
static Desc g_ldt[8192];

void rt_sel_reset(void)
{
    memset(g_ldt, 0, sizeof g_ldt);
    g_ldt[SEL_CODE >> 3] = (Desc){0u, 0xFFFFFFFFu, 1};
    g_ldt[SEL_DATA >> 3] = (Desc){0u, 0xFFFFFFFFu, 1};
    g_ldt[SEL_ZERO >> 3] = (Desc){0u, 0xFFFFFFFFu, 1};
    g_ldt[SEL_PSP >> 3] = (Desc){LOW_PSP, 0xFFu, 1};
    g_ldt[SEL_ENV >> 3] = (Desc){LOW_ENV, 0xFFFu, 1};
    g_ldt[0x40 >> 3] = (Desc){LOW_BDA, 0x2FFu, 1}; /* DOS/4GW: selector 0040h is the BIOS data area */
}

int rt_sel_valid(uint16_t sel)
{
    return g_ldt[sel >> 3].used;
}

uint32_t rt_sel_base(uint16_t sel)
{
    if ((sel >> 3) == 0) return 0u;
    return g_ldt[sel >> 3].base;
}

uint32_t rt_sel_limit(uint16_t sel)
{
    return g_ldt[sel >> 3].limit;
}

uint16_t rt_sel_alloc(uint32_t base, uint32_t limit)
{
    int i;
    for (i = 0x40; i < 8192; i++) {
        if (!g_ldt[i].used) {
            g_ldt[i] = (Desc){base, limit, 1};
            return (uint16_t)((i << 3) | 7);
        }
    }
    return 0;
}

void rt_sel_free(uint16_t sel)
{
    if ((sel >> 3) >= 0x40) g_ldt[sel >> 3].used = 0;
}

void rt_sel_set_base(uint16_t sel, uint32_t base)
{
    g_ldt[sel >> 3].base = base;
    if (R.es == sel) R.es_base = base;
    if (R.fs == sel) R.fs_base = base;
    if (R.gs == sel) R.gs_base = base;
    if (R.ds == sel) R.ds_base = base;
}

void rt_sel_set_limit(uint16_t sel, uint32_t limit)
{
    g_ldt[sel >> 3].limit = limit;
}

void rt_load_seg(int seg, uint16_t sel)
{
    uint32_t base = rt_sel_base(sel);
    switch (seg) {
    case SEG_ES:
        R.es = sel;
        R.es_base = base;
        break;
    case SEG_FS:
        R.fs = sel;
        R.fs_base = base;
        break;
    case SEG_GS:
        R.gs = sel;
        R.gs_base = base;
        break;
    case SEG_DS:
        R.ds = sel;
        R.ds_base = base; /* used by the functions that load DS themselves (recomp.py: ds_dyn) */
        if (base != 0u) rt_trace("DS loaded with selector %04x of base %08x", sel, base);
        break;
    case SEG_SS:
        R.ss = sel;
        if (base != 0u) rt_log("warning: SS loaded with selector %04x of base %08x (treated as flat)", sel, base);
        break;
    default:
        R.cs = sel;
        break;
    }
}

/* ---- the running process ------------------------------------------------------------------------------ */
struct RtProcess {
    const RtModuleDef *def;
    uint8_t *mem;
    Cpu regs;
    jmp_buf exit_jmp;
    uint32_t pm_vectors[256];
    Desc *ldt_saved;
    void *dos_state;
    struct RtProcess *parent;
};
static RtProcess *g_proc;

static int cmp_func(const void *k, const void *e)
{
    uint32_t a = *(const uint32_t *)k;
    const RtFunc *f = (const RtFunc *)e;
    return a < f->addr ? -1 : a > f->addr ? 1 : 0;
}

/* ---- relocatable modules (sound drivers the game loads and relocates itself) ----------------------------- */
static const RtModuleDef *g_reloc[8];
static int g_nreloc;

void rt_register_reloc_module(const RtModuleDef *def)
{
    if (g_nreloc < (int)(sizeof g_reloc / sizeof g_reloc[0])) g_reloc[g_nreloc++] = def;
}

/* do the code bytes of function k of module m sit in guest memory with load delta d? */
static int reloc_sig_ok(const RtModuleDef *m, uint32_t k, uint32_t d)
{
    const uint8_t *sig = m->sig + k * 16u;
    uint32_t a = m->funcs[k].addr + d, i;
    if (a >= RT_MEM_SIZE - 8u) return 0;
    for (i = 0; i < 8; i++)
        if ((M[a + i] & sig[8 + i]) != (sig[i] & sig[8 + i])) return 0;
    return 1;
}

static int reloc_image_ok(const RtModuleDef *m, uint32_t d)
{
    uint32_t k;
    for (k = 0; k < m->nfuncs; k++)
        if (!reloc_sig_ok(m, k, d)) return 0;
    return 1;
}

static const RtFunc *reloc_find(uint32_t addr)
{
    int i;
    uint32_t k;
    /* a module at its known place */
    for (i = 0; i < g_nreloc; i++) {
        const RtModuleDef *m = g_reloc[i];
        uint32_t off = addr - *m->delta;
        const RtFunc *f;
        if (*m->delta == 0u || off - m->link_base >= m->link_size) continue;
        f = (const RtFunc *)bsearch(&off, m->funcs, m->nfuncs, sizeof(RtFunc), cmp_func);
        if (f != NULL && reloc_sig_ok(m, (uint32_t)(f - m->funcs), *m->delta)) return f;
    }
    /* a module loaded (again) somewhere new: try every function as the target */
    for (i = 0; i < g_nreloc; i++) {
        const RtModuleDef *m = g_reloc[i];
        for (k = 0; k < m->nfuncs; k++) {
            uint32_t d = addr - m->funcs[k].addr;
            if (reloc_sig_ok(m, k, d) && reloc_image_ok(m, d)) {
                *m->delta = d;
                rt_log("%s found at %08x", m->exe_name, m->link_base + d);
                return &m->funcs[k];
            }
        }
    }
    return NULL;
}

static const RtFunc *find_func(uint32_t addr)
{
    const RtFunc *f = (const RtFunc *)bsearch(&addr, g_proc->def->funcs, g_proc->def->nfuncs, sizeof(RtFunc),
                                              cmp_func);
    if (f == NULL && g_nreloc > 0) f = reloc_find(addr);
    return f;
}

void rt_call_ind(uint32_t addr)
{
    const RtFunc *f;
    if (g_proc == NULL) rt_fatal("indirect call without a process");
    f = find_func(addr);
    if (f == NULL) rt_fatal("%s: indirect call/jump to unknown address %08x", g_proc->def->exe_name, addr);
    f->fn();
}

/* a computed jump ("push target; ret"): a recompiled function, or one of the tiny "int N; ret" stubs */
void rt_jump_ind(uint32_t addr)
{
    const RtFunc *f = NULL;
    if (g_proc == NULL) rt_fatal("indirect jump without a process");
    f = find_func(addr);
    if (f != NULL) {
        f->fn();
        return;
    }
    if (addr < RT_MEM_SIZE - 4) {
        const uint8_t *b = M + addr;
        if (b[0] == 0xCD && b[2] == 0xC3) {
            rt_int(b[1]);
            R.esp += 4u;
            return;
        }
        if (b[0] == 0xCC && b[1] == 0x90 && b[2] == 0xC3) {
            rt_int(3);
            R.esp += 4u;
            return;
        }
    }
    rt_fatal("%s: computed jump to unknown address %08x", g_proc->def->exe_name, addr);
}

void rt_exit_process(int code)
{
    rt_exit_code = code;
    longjmp(g_proc->exit_jmp, 1);
}

uint32_t dpmi_pm_vector(int n)
{
    return g_proc != NULL ? g_proc->pm_vectors[n & 0xFF] : 0u;
}

void dpmi_set_pm_vector(int n, uint32_t addr)
{
    if (RT_IS_DEFAULT_VEC(addr)) addr = 0u; /* restoring the default handler */
    if (g_proc != NULL) g_proc->pm_vectors[n & 0xFF] = addr;
}

uint32_t dpmi_pm_vector_get(int n)
{
    uint32_t a = dpmi_pm_vector(n);
    return a != 0u ? a : RT_DEFAULT_VEC(n & 0xFF);
}

/* guest interrupt handler: the stack frame of a 32-bit interrupt gate (eflags, cs, eip); the handler's
 * iretd (rt_iret) pops it again, the C return comes back here */
#define RT_IRET_MAGIC 0xFFFF0000u
/* a handler may leave by chaining to the default handler (retf to the old vector); the default handler's
 * iretd ends the interrupt, so the C frames of the guest handler are unwound back to here */
static jmp_buf *g_int_jb[32];
static int g_int_depth;

void rt_call_guest_interrupt(uint32_t handler)
{
    jmp_buf jb;
    int depth = g_int_depth;
    PUSH32(rt_get_eflags());
    PUSH32(R.cs);
    PUSH32(RT_IRET_MAGIC);
    R.iflag = 0;
    if (depth >= (int)(sizeof g_int_jb / sizeof g_int_jb[0])) rt_fatal("interrupts nested too deeply");
    g_int_jb[depth] = &jb;
    g_int_depth = depth + 1;
    if (setjmp(jb) == 0) rt_call_ind(handler);
    g_int_depth = depth;
}

static int rt_int_service(uint32_t n);

void rt_iret(void)
{
    uint32_t eip = POP32();
    uint32_t cs = POP32();
    uint32_t fl = POP32();
    (void)eip;
    (void)cs;
    rt_set_eflags(fl);
}

void rt_retf(uint32_t n)
{
    uint32_t eip = POP32();
    (void)POP32(); /* cs */
    R.esp += n;
    if (RT_IS_DEFAULT_VEC(eip)) {
        /* chained to the default handler: its service (hardware interrupts need none), then its iretd */
        uint32_t v = eip & 0xFFu;
        if (v == 0x09)
            kbd_irq_ack(); /* the BIOS keyboard handler: the key into the int 16h buffer (text entry reads it) */
        else if (!((v >= 0x08 && v <= 0x0F) || (v >= 0x70 && v <= 0x77)))
            rt_int_service(v);
        rt_iret();
        if (g_int_depth == 0) rt_fatal("chain to the default handler of int %02x outside an interrupt", v);
        longjmp(*g_int_jb[g_int_depth - 1], 1);
    }
}

/* ---- software interrupts ----------------------------------------------------------------------------- */
/* the services of the emulated DOS, DOS extender and BIOS; 0 for a number they do not provide */
static int rt_int_service(uint32_t n)
{
    switch (n) {
    case 0x21: dos_int21(); return 1;
    case 0x31: dpmi_int31(); return 1;
    case 0x10: bios_int10(); return 1;
    case 0x16: bios_int16(); return 1;
    case 0x1A: bios_int1a(); return 1;
    case 0x33: bios_int33(); return 1;
    case 0x15: bios_int15(); return 1;
    case 0x2F: dos_int2f(); return 1;
    default: return 0;
    }
}

void rt_int(uint32_t n)
{
    if (rt_int_service(n)) return;
    if (dpmi_pm_vector((int)n) != 0u) {
        rt_call_guest_interrupt(dpmi_pm_vector((int)n));
        return;
    }
    rt_log("unhandled software interrupt %02x (eax=%08x)", n, R.eax);
    R.cf = 1;
}

/* ---- ports ---------------------------------------------------------------------------------------------- */
static uint8_t g_pic_mask[2]; /* interrupt mask registers of the two PICs (only gate the sound card IRQ) */
static uint8_t g_pic_read_isr[2]; /* OCW3: port 20h/A0h reads the ISR instead of the IRR */
static uint8_t g_pic_isr[2];      /* in service (tracked for the sound card IRQ; cleared by EOI) */

static uint8_t pic_irr(int slave)
{
    int irq = audio_irq();
    uint8_t v = 0;
    if (audio_irq_pending() && (irq >= 8) == slave) v |= (uint8_t)(1u << (irq & 7));
    if (!slave && kbd_irq_pending()) v |= 2u;
    return v;
}

uint8_t rt_in8(uint16_t port)
{
    if (port >= 0x3B0 && port <= 0x3DF) return vga_port_in(port);
    if (port >= 0x40 && port <= 0x43) return pit_port_in(port);
    if (port == 0x60) return kbd_port60();
    if (port == 0x64) return kbd_port64();
    if (port == 0x61) return 0x20;
    if (port == 0x20 || port == 0xA0)
        return g_pic_read_isr[port == 0xA0] ? g_pic_isr[port == 0xA0] : pic_irr(port == 0xA0);
    if (port == 0x21) return g_pic_mask[0];
    if (port == 0xA1) return g_pic_mask[1];
    if ((port >= 0x220 && port <= 0x22F) || (port >= 0x388 && port <= 0x38B) || (port >= 0x240 && port <= 0x24F) ||
        (port >= 0x260 && port <= 0x26F) || (port >= 0x280 && port <= 0x28F) || port == 0x330 || port == 0x331 ||
        port <= 0x0F || (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF))
        return audio_port_in(port);
    if (port == 0x201) return 0xFF; /* no joystick */
    rt_trace("in8 %04x", port);
    return 0xFF;
}

uint16_t rt_in16(uint16_t port)
{
    return (uint16_t)(rt_in8(port) | (rt_in8((uint16_t)(port + 1)) << 8));
}

uint32_t rt_in32(uint16_t port)
{
    return (uint32_t)rt_in16(port) | ((uint32_t)rt_in16((uint16_t)(port + 2)) << 16);
}

void rt_out8(uint16_t port, uint8_t v)
{
    if (port >= 0x3B0 && port <= 0x3DF) {
        vga_port_out(port, v);
        return;
    }
    if (port >= 0x40 && port <= 0x43) {
        pit_port_out(port, v);
        return;
    }
    if (port == 0x20 || port == 0xA0) {
        if (v == 0x20 || (v & 0xE0) == 0x60) {
            g_pic_isr[port == 0xA0] = 0;
            pic_eoi(port == 0x20);
        }
        else if ((v & 0x18) == 0x08 && (v & 2)) g_pic_read_isr[port == 0xA0] = (uint8_t)(v & 1); /* OCW3 */
        return;
    }
    if (port == 0x21 || port == 0xA1) {
        g_pic_mask[port == 0xA1] = v;
        return;
    }
    if (port == 0x61) {
        kbd_port61_out(v);
        return;
    }
    if ((port >= 0x220 && port <= 0x22F) || (port >= 0x388 && port <= 0x38B) || (port >= 0x240 && port <= 0x24F) ||
        (port >= 0x260 && port <= 0x26F) || (port >= 0x280 && port <= 0x28F) || port == 0x330 || port == 0x331 ||
        (port <= 0x0F) || (port >= 0x80 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF)) {
        audio_port_out(port, v);
        return;
    }
    rt_trace("out8 %04x %02x", port, v);
}

void rt_out16(uint16_t port, uint16_t v)
{
    rt_out8(port, (uint8_t)v);
    rt_out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
}

void rt_out32(uint16_t port, uint32_t v)
{
    rt_out16(port, (uint16_t)v);
    rt_out16((uint16_t)(port + 2), (uint16_t)(v >> 16));
}

/* ---- the poll loop: interrupts, frames, pacing ------------------------------------------------------------ */
static uint64_t g_next_tick;   /* next IRQ 0 (us) */
static uint64_t g_next_bios;   /* next 18.2 Hz BIOS tick */
static uint64_t g_next_frame;  /* next presentation */
static uint64_t g_last_poll;
static int g_irq0_pending, g_polls_idle;
static volatile int g_paused;
static uint8_t g_frame[640 * 480];
static uint32_t g_pal[256];

void rt_set_paused(int paused)
{
    g_paused = paused;
}

void rt_irq_init(void)
{
    uint64_t now = host_time_us();
    g_next_tick = now + pit_irq0_period_us();
    g_next_bios = now + 54925u;
    g_next_frame = now;
    g_last_poll = now;
}

void pic_eoi(int master)
{
    (void)master;
}

static void present(void)
{
    int w = 0, h = 0;
    if (vga_render(g_frame, &w, &h, g_pal)) host_present(g_frame, w, h, g_pal);
}

void rt_poll(void)
{
    uint64_t now;
    int delivered = 0;
    rt_poll_budget = 4000;
    if (rt_in_irq) return;
    now = host_time_us();
    while (g_paused && !host_should_quit()) {
        host_sleep_us(10000);
        now = host_time_us();
        g_next_tick = g_next_bios = g_next_frame = now;
    }
    if (host_should_quit()) rt_exit_process(0);

    /* BIOS tick (0x46C), independent of the PIT reprogramming */
    while (now >= g_next_bios) {
        bios_tick();
        g_next_bios += 54925u;
        if (now - g_next_bios > 1000000u) g_next_bios = now + 54925u;
    }
    /* IRQ 0: every period that passed counts (one is delivered per poll), so the game's timers and the music
     * keep their pace when polls come late; after a long stall it does not catch up */
    if (now >= g_next_tick) {
        uint32_t period = pit_irq0_period_us();
        while (now >= g_next_tick && g_irq0_pending < 8) {
            g_irq0_pending++;
            g_next_tick += period;
        }
        if (now > g_next_tick + 200000u) g_next_tick = now + period;
    }
    if (R.iflag) {
        uint32_t h = dpmi_pm_vector(0x08);
        if (g_irq0_pending > 0) {
            g_irq0_pending--;
            if (h != 0u) {
                rt_in_irq++;
                rt_call_guest_interrupt(h);
                rt_in_irq--;
                delivered = 1;
            }
        }
        if (audio_irq_take()) {
            int irq = audio_irq();
            int masked = irq < 8 ? (g_pic_mask[0] >> irq) & 1 : ((g_pic_mask[1] >> (irq - 8)) & 1);
            uint32_t a = dpmi_pm_vector(irq < 8 ? 0x08 + irq : 0x70 + irq - 8);
            if (rt_verbose >= 3) rt_log("sb irq: masked %d vector %08x if %d", masked, a, R.iflag);
            if (!masked && a != 0u) {
                g_pic_isr[irq >= 8] |= (uint8_t)(1u << (irq & 7));
                if (irq >= 8) g_pic_isr[0] |= 4u; /* the cascade */
                rt_in_irq++;
                rt_call_guest_interrupt(a);
                rt_in_irq--;
                delivered = 1;
            }
        }
        if (mouse_poll_handler()) delivered = 1;
        if (kbd_irq_pending()) {
            uint32_t k = dpmi_pm_vector(0x09);
            rt_in_irq++;
            if (k != 0u)
                rt_call_guest_interrupt(k);
            else
                kbd_irq_ack(); /* the BIOS handler: into the int 16h buffer */
            rt_in_irq--;
            delivered = 1;
        }
    }
    audio_poll();
    if (now >= g_next_frame) {
        present();
        g_next_frame += 14286u; /* 70 Hz */
        if (now > g_next_frame + 100000u) g_next_frame = now + 14286u;
        delivered = 1;
    }
    /* pacing: frequent polls with nothing to do mean the guest is waiting for time to pass */
    if (!delivered && now - g_last_poll < 200u) {
        if (++g_polls_idle > 50) {
            /* until the next timer interrupt or frame (at most 4 ms) */
            uint64_t next = g_next_tick < g_next_frame ? g_next_tick : g_next_frame;
            if (next > now + 1500u) host_sleep_us(next - now > 4500u ? 4000u : (uint32_t)(next - now - 500u));
            g_polls_idle = 0;
        }
    } else if (delivered) {
        g_polls_idle = 0;
    }
    g_last_poll = now;
}

/* the guest polls a clock and got the same answer `repeats` times in a row: it is waiting for time to pass,
 * so wait here (until the next timer interrupt, frame or BIOS tick, at most 2 ms) instead of spinning */
void rt_clock_wait(int repeats)
{
    uint64_t now, next;
    if (repeats < 8 || rt_in_irq) return;
    now = host_time_us();
    next = g_next_tick < g_next_frame ? g_next_tick : g_next_frame;
    if (g_next_bios < next) next = g_next_bios;
    if (next > now + 200u) host_sleep_us(next - now > 2000u ? 2000u : (uint32_t)(next - now));
    rt_poll();
}

/* ---- LE loading ------------------------------------------------------------------------------------------- */
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t *read_file(const char *path, size_t *n)
{
    VFile *f = vfs_open(path, "rb");
    uint8_t *buf;
    long len;
    if (f == NULL) return NULL;
    len = vfs_seek(f, 0, SEEK_END);
    vfs_seek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)len + 1);
    if (buf == NULL || vfs_read(f, buf, (size_t)len) != (size_t)len) {
        free(buf);
        vfs_close(f);
        return NULL;
    }
    vfs_close(f);
    *n = (size_t)len;
    return buf;
}

/* place the LE objects at their bases in M and apply the 32-bit offset fixups; returns 0 on success */
static int load_le(const uint8_t *raw, size_t n, uint32_t *top)
{
    uint32_t le = 0, i, page_size, page_count, obj_tab, obj_count, page_map, fix_pages, fix_recs, data_pages;
    uint32_t last_page;
    struct {
        uint32_t vsize, base, flags, pidx, pcnt;
    } objs[16];
    if (n < 0x40 || raw[0] != 'M' || raw[1] != 'Z') return -1;
    for (i = 0; i + 4 < n && i < 0x10000; i++) {
        if (raw[i] == 'L' && raw[i + 1] == 'E' && raw[i + 2] == 0 && raw[i + 3] == 0) {
            le = i;
            break;
        }
    }
    if (le == 0) return -1;
    page_count = rd32le(raw + le + 0x14);
    page_size = rd32le(raw + le + 0x28);
    last_page = rd32le(raw + le + 0x2C);
    obj_tab = le + rd32le(raw + le + 0x40);
    obj_count = rd32le(raw + le + 0x44);
    page_map = le + rd32le(raw + le + 0x48);
    fix_pages = le + rd32le(raw + le + 0x68);
    fix_recs = le + rd32le(raw + le + 0x6C);
    data_pages = rd32le(raw + le + 0x80);
    if (obj_count > 16) return -1;
    *top = 0;
    for (i = 0; i < obj_count; i++) {
        const uint8_t *o = raw + obj_tab + i * 24;
        uint32_t p;
        objs[i].vsize = rd32le(o);
        objs[i].base = rd32le(o + 4) + RT_IMAGE_BIAS;
        objs[i].flags = rd32le(o + 8);
        objs[i].pidx = rd32le(o + 12);
        objs[i].pcnt = rd32le(o + 16);
        if (objs[i].base + objs[i].vsize > *top) *top = objs[i].base + objs[i].vsize;
        for (p = 0; p < objs[i].pcnt; p++) {
            uint32_t page = objs[i].pidx - 1 + p;
            const uint8_t *pm = raw + page_map + page * 4;
            uint32_t num = ((uint32_t)pm[0] << 16) | ((uint32_t)pm[1] << 8) | pm[2];
            uint32_t size = page == page_count - 1 ? last_page : page_size;
            uint32_t off;
            if (num == 0) continue;
            off = data_pages + (num - 1) * page_size;
            if (off + size > n) return -2;
            memcpy(M + objs[i].base + p * page_size, raw + off, size);
        }
    }
    /* fixups */
    for (i = 0; i < page_count; i++) {
        uint32_t start = rd32le(raw + fix_pages + i * 4), end = rd32le(raw + fix_pages + i * 4 + 4);
        uint32_t page_base = 0, k, p = fix_recs + start, q = fix_recs + end;
        for (k = 0; k < obj_count; k++)
            if (i + 1 >= objs[k].pidx && i + 1 < objs[k].pidx + objs[k].pcnt)
                page_base = objs[k].base + (i + 1 - objs[k].pidx) * page_size;
        while (p < q) {
            uint8_t src = raw[p], flags = raw[p + 1];
            uint32_t count, tobj, toff = 0, j;
            int16_t single = 0;
            p += 2;
            if (src & 0x20) {
                count = raw[p++];
            } else {
                count = 1;
                single = (int16_t)rd16le(raw + p);
                p += 2;
            }
            if ((flags & 3) != 0) return -3;
            if (flags & 0x40) {
                tobj = rd16le(raw + p);
                p += 2;
            } else {
                tobj = raw[p++];
            }
            if ((src & 0x0F) != 2) {
                if (flags & 0x10) {
                    toff = rd32le(raw + p);
                    p += 4;
                } else {
                    toff = rd16le(raw + p);
                    p += 2;
                }
            }
            if ((src & 0x0F) != 7 || tobj == 0 || tobj > obj_count) return -4;
            for (j = 0; j < count; j++) {
                int16_t so = (src & 0x20) ? (int16_t)rd16le(raw + p + 2 * j) : single;
                uint32_t site = page_base + (uint32_t)(int32_t)so;
                uint32_t v = objs[tobj - 1].base + toff;
                memcpy(M + site, &v, 4);
            }
            if (src & 0x20) p += 2 * count;
        }
    }
    return 0;
}

uint32_t g_heap_top_hint;

int rt_run_module(const RtModuleDef *def, const char *dos_path, const char *args)
{
    RtProcess proc, *parent = g_proc;
    int parent_mouse;
    char host[1024], sha[41];
    uint8_t *raw;
    size_t n;
    uint32_t top;
    int rc;

    if (!dos_resolve_read(dos_path, host, sizeof host)) rt_fatal("cannot find %s", dos_path);
    raw = read_file(host, &n);
    if (raw == NULL) rt_fatal("cannot read %s", host);
    sha1_hex(raw, n, sha);
    if (strcmp(sha, def->sha1) != 0)
        rt_fatal("%s is not the version this program was built from (SHA-1 %s, expected %s)", host, sha, def->sha1);

    memset(&proc, 0, sizeof proc);
    proc.def = def;
    proc.parent = parent;
    proc.mem = (uint8_t *)calloc(1, RT_MEM_SIZE + 16u);
    if (proc.mem == NULL) rt_fatal("out of memory");
    if (parent != NULL) {
        /* the child shares the first megabyte (DOS memory, BIOS data, video) with its parent */
        memcpy(proc.mem, parent->mem, 0x100000u);
        parent->regs = R;
        proc.ldt_saved = (Desc *)malloc(sizeof g_ldt);
        memcpy(proc.ldt_saved, g_ldt, sizeof g_ldt);
        dos_save_state(&proc.dos_state);
    }
    M = proc.mem;
    if (parent == NULL) bios_init();
    rc = load_le(raw, n, &top);
    free(raw);
    if (rc != 0) rt_fatal("%s: unsupported LE image (%d)", host, rc);
    g_heap_top_hint = top;
    g_proc = &proc;

    memset(&R, 0, sizeof R);
    rt_sel_reset();
    dpmi_init();
    dos_set_psp(LOW_PSP, args, dos_path);
    R.cs = SEL_CODE;
    R.ds = R.ss = SEL_DATA;
    R.fs = 0;
    R.gs = 0;
    rt_load_seg(SEG_ES, SEL_PSP);
    R.esp = def->stack;
    R.iflag = 1;
    R.ebx = 0;
    rt_irq_init();
    parent_mouse = mouse_used_swap(0); /* each program shows whether it uses the mouse; the parent's comes back */
    rt_log("run %s %s", dos_path, args);

    if (setjmp(proc.exit_jmp) == 0) {
        rt_call_ind(def->entry);
        rt_exit_code = (int)(R.eax & 0xFF);
    }
    rc = rt_exit_code;
    rt_log("%s exited with %d", dos_path, rc);
    mouse_used_swap(parent_mouse);

    g_proc = parent;
    if (parent != NULL) {
        /* DOS memory, the BIOS data and the vectors go back to the parent; its environment and PSP stay */
        memcpy(parent->mem, proc.mem, LOW_ENV);
        memcpy(parent->mem + LOW_PSP + 0x100u, proc.mem + LOW_PSP + 0x100u, 0x100000u - (LOW_PSP + 0x100u));
        M = parent->mem;
        R = parent->regs;
        memcpy(g_ldt, proc.ldt_saved, sizeof g_ldt);
        free(proc.ldt_saved);
        dos_restore_state(proc.dos_state);
        rt_irq_init();
        free(proc.mem);
        if (host_should_quit()) rt_exit_process(0); /* closing the window ends every program level */
        return rc;
    } else {
        M = NULL;
    }
    free(proc.mem);
    return rc;
}
