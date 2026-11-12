/* host.h - the boundary between the portable game core (src/rt, src/game, gen) and the platform layer
 * (src/platform/win32, src/platform/macos, src/platform/headless).
 *
 * The core runs the recompiled DOS programs on one thread (the "game thread").  It calls the host_*
 * functions below; the platform calls the rt_* entry points (rt_key_event, rt_mouse_event, ...) from its
 * own UI thread, so those are thread-safe.  Nothing in this header depends on an OS header. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented by the platform -------------------------------------------------------------------------- */

/* monotonic time in microseconds */
uint64_t host_time_us(void);
/* sleep the game thread for about `us` microseconds */
void host_sleep_us(uint32_t us);

/* a finished frame: `w` x `h` palette indices (row stride `w`) and 256 RGB colours (0x00RRGGBB). Every VGA
 * mode the games use fills a 4:3 screen (320x200 has tall pixels). Called on the game thread; the platform
 * copies what it needs before returning. */
void host_present(const uint8_t *pixels, int w, int h, const uint32_t *palette);

/* audio: the core produces signed 16-bit stereo at host_audio_rate() and pushes it here */
int host_audio_rate(void);
void host_audio_push(const int16_t *frames, size_t count);

/* one lock shared by the input queues (the UI thread feeds them, the game thread drains them) */
void host_lock(void);
void host_unlock(void);

/* log a line (UTF-8, no newline needed) */
void host_log(const char *line);

/* the original game files built into the program (a pack written by tools/pack_game.py), used when no game
 * folder is given; NULL when the program has none */
const uint8_t *host_game_pack(size_t *size);

/* the core cannot go on (an emulation error): tell the player and end the process; does not return */
void host_fatal(const char *text);

/* the game thread calls this regularly; non-zero when the player closed the window (the core then ends) */
int host_should_quit(void);

/* file system helpers the C library does not offer portably (UTF-8 paths, '/' separators accepted) */
int host_mkdir(const char *path);              /* 0 on success or when it exists */
int host_is_dir(const char *path);
typedef struct HostDirEntry {
    char name[260];
    uint32_t size;
    int is_dir;
    uint16_t dos_time, dos_date;
} HostDirEntry;
void *host_dir_open(const char *path);         /* NULL when it does not exist */
int host_dir_next(void *dir, HostDirEntry *e); /* 1 while there is an entry */
void host_dir_close(void *dir);
/* case-insensitive lookup of one path component in `dir`: writes the real name; 1 when found */
int host_find_name(const char *dir, const char *name, char *out, size_t out_size);

/* ---- implemented by the core (thread-safe, callable from the UI thread) --------------------------------- */

/* PC/AT scancode set 1 (make code; extended keys with the 0xE0 prefix as 0xE000 | code) */
void rt_key_event(uint32_t scancode, int down);
/* mouse position as a fraction of the displayed game picture (0..65535 each way; the core maps it onto
 * the range the game gave the mouse driver); buttons bit 0 left, bit 1 right, bit 2 middle */
void rt_mouse_move(int fx, int fy);
void rt_mouse_buttons(int buttons);
/* relative motion in mickeys (used when the game reads motion counters) */
void rt_mouse_motion(int dx, int dy);
/* non-zero while the running program uses the mouse (reads its position, buttons or motion, or has an event
 * handler); such a program draws its own pointer, so the front end may hide the system's.  The launcher does
 * not use the mouse and draws no pointer. */
int rt_mouse_used(void);
/* pause/resume the game thread at its next poll (e.g. while a modal dialog is open) */
void rt_set_paused(int paused);

#ifdef __cplusplus
}
#endif
