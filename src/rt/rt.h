/* rt.h - the runtime's internal interface (shared by src/rt/*.c and src/game). */
#pragma once

#include <stdarg.h>
#include <stdio.h>

#include "host.h"
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- logging ------------------------------------------------------------------------------------------- */
extern int rt_verbose;  /* 0 errors only, 1 info, 2 trace of DOS/DPMI calls */
void rt_log(const char *fmt, ...);
void rt_trace(const char *fmt, ...);

/* ---- low memory layout (guest linear addresses) ----------------------------------------------------------- */
#define LOW_IVT 0x00000u          /* real-mode interrupt vectors */
#define LOW_BDA 0x00400u          /* BIOS data area */
#define LOW_ENV 0x01000u          /* environment block (paragraph 0x100) */
#define LOW_PSP 0x02000u          /* PSP (paragraph 0x200) */
#define LOW_DTA_DEFAULT (LOW_PSP + 0x80u)
#define LOW_DOS_HEAP 0x10000u     /* DOS memory blocks for DPMI 0100h start here ... */
#define LOW_DOS_HEAP_END 0x9F000u /* ... and end below the video memory */
#define HIGH_HEAP_BASE 0x00200000u /* DPMI 0501h blocks: from 2 MiB up to RT_MEM_SIZE */
#define RT_IMAGE_BIAS 0x003F0000u  /* programs (linked at 0x10000) run relocated to 0x400000 (tools/recomp.py) */

/* ---- selectors ------------------------------------------------------------------------------------------ */
#define SEL_CODE 0x0170u
#define SEL_DATA 0x0178u
#define SEL_PSP 0x0180u
#define SEL_ENV 0x0188u
#define SEL_ZERO 0x0190u  /* base 0, 4 GiB: DOS/4GW's selector for the first megabyte */
uint32_t rt_sel_base(uint16_t sel);
int mouse_poll_handler(void);
void rt_clock_wait(int repeats);
struct tm;
extern int rt_fixed_clock;           /* devices.c: a fixed date and time (repeatable test runs) */
void rt_wall_time(struct tm *out);   /* devices.c: the date and time of day the guest sees */ /* core.c: the guest spins on a clock that did not change */ /* devices.c: runs the game's int 33h event handler when due */
int rt_sel_valid(uint16_t sel);
uint16_t rt_sel_alloc(uint32_t base, uint32_t limit);
void rt_sel_free(uint16_t sel);
void rt_sel_set_base(uint16_t sel, uint32_t base);
void rt_sel_set_limit(uint16_t sel, uint32_t limit);
uint32_t rt_sel_limit(uint16_t sel);
void rt_sel_reset(void);

/* ---- modules and processes ---------------------------------------------------------------------------- */
typedef struct RtProcess RtProcess;
/* run a recompiled program to completion (nested for DOS EXEC); returns its exit code */
int rt_run_module(const RtModuleDef *def, const char *dos_path, const char *args);
/* the game layer maps a DOS program path ("D:\SYNDICAT\MAIN.EXE") to its recompiled module */
const RtModuleDef *game_find_module(const char *dos_path);
void rt_register_reloc_module(const RtModuleDef *def);
RT_NORETURN void rt_exit_process(int code);
extern int rt_exit_code;

/* host directory of the original game files (contains SYND.EXE, SYNDICAT/, DATADISK/) */
extern char rt_game_dir[1024];
/* writable directory: the overlay for files the game writes, and the C: drive */
extern char rt_user_dir[1024];

/* ---- DOS ------------------------------------------------------------------------------------------------ */
void dos_init(void);
void dos_int21(void);
void dos_int2f(void);
void dos_close_all(void);
void dos_set_psp(uint32_t linear, const char *cmdtail, const char *progpath);
/* DOS path -> host path for reading (overlay first, then the game files); returns 0 when not found */
int dos_resolve_read(const char *dos_path, char *host, size_t n);
int dos_resolve_write(const char *dos_path, char *host, size_t n);
void dos_get_cwd(char *out, size_t n); /* "D:\SYNDICAT" */
void dos_save_state(void **state);
void dos_restore_state(void *state);

/* ---- DPMI ------------------------------------------------------------------------------------------------ */
void dpmi_init(void);
void dpmi_int31(void);
uint32_t dpmi_pm_vector(int n);     /* guest protected-mode handler (0 = none) */
/* what "get vector" hands the guest for a vector it has not set: a marker address standing for the default
 * (DOS extender / BIOS) handler, so chaining to it (retf to the old vector) can be recognised */
#define RT_DEFAULT_VEC(n) (0xFFFFFE00u | (uint32_t)(n))
#define RT_IS_DEFAULT_VEC(a) (((a) & 0xFFFFFF00u) == 0xFFFFFE00u)
uint32_t dpmi_pm_vector_get(int n);
void dpmi_set_pm_vector(int n, uint32_t addr);
uint32_t dpmi_dos_alloc(uint32_t paragraphs, uint32_t *largest);

/* ---- BIOS / devices ------------------------------------------------------------------------------------ */
void bios_init(void);
void bios_int10(void);
void bios_int16(void);
void bios_int1a(void);
void bios_int33(void);
void bios_int15(void);
void bios_tick(void);  /* 18.2 Hz BIOS tick at 0x46C */

void vga_init(void);
void vga_set_mode(int mode);
uint8_t vga_port_in(uint16_t port);
void vga_port_out(uint16_t port, uint8_t v);
/* render the current screen as indices + palette; returns 0 when nothing is visible (text mode) */
int vga_render(uint8_t *pixels, int *w, int *h, uint32_t *palette);
extern int vga_retrace_reads;

void pit_init(void);
void pit_port_out(uint16_t port, uint8_t v);
uint8_t pit_port_in(uint16_t port);
uint32_t pit_irq0_period_us(void);  /* the programmed channel-0 period */

void kbd_init(void);
int kbd_irq_pending(void);
uint8_t kbd_port60(void);
uint8_t kbd_port64(void);
void kbd_port61_out(uint8_t v);
void kbd_irq_ack(void);
int kbd_bios_peek(uint16_t *key);    /* int 16h: 1 when a key is buffered */
uint16_t kbd_bios_get(void);
uint16_t kbd_bios_shift(void);

void mouse_get(int *x, int *y, int *buttons);
void mouse_take_motion(int *dx, int *dy);
int mouse_used_swap(int used); /* sets whether the running program uses the mouse; returns the old value */
int mouse_take_press(int button, int *count, int *x, int *y);
int mouse_take_release(int button, int *count, int *x, int *y);

void pic_eoi(int master);
void audio_port_out(uint16_t port, uint8_t v);
uint8_t audio_port_in(uint16_t port);
void audio_poll(void);
void audio_init(void);
int audio_irq(void);      /* the IRQ line of the Sound Blaster */
int audio_irq_take(void); /* 1 once per interrupt the card raised */
int audio_irq_pending(void); /* the card's request line (the PIC's IRR) */

/* ---- interrupts ------------------------------------------------------------------------------------------ */
/* call a guest protected-mode interrupt handler as the hardware would (pushes eflags, cs, eip) */
void rt_call_guest_interrupt(uint32_t handler);
void rt_irq_init(void);
extern int rt_in_irq;

#ifdef __cplusplus
}
#endif
