/* linux_main.c - the Linux front end of Syndicate Plus 64bit (SDL2): one window that shows the game, feeds it
 * the keyboard and the mouse and plays its sound.  The portable core (game_main) runs on its own thread; this
 * file is the UI (main) thread.  It behaves as the Windows front end (platform/win32/win32_main.c) does:
 *
 * - Alt+Enter toggles full screen (on the display the window is on), Alt+S the CRT scanlines (off at first,
 *   remembered in Syndicate64.ini), F12 shows the About box;
 * - Alt is passed to the game only once another key comes with it (or alone, on release), so that the
 *   window's Alt shortcuts never reach the game;
 * - in full screen the pointer stays on the picture while the game has the focus and uses the mouse;
 * - the game pauses while the window is minimized or a message box is open.
 *
 *   Syndicate64 [--game-dir DIR] [--user-dir DIR] [--start "SYNDICAT\MAIN.EXE /args"] [--log FILE]
 *               [--verbose N] [--fullscreen]
 *
 * The original game files built into a private build (SYN_EMBED_GAME: linux_pack.c) are used unless
 * --game-dir names a folder; a syndgame.pak next to the program is used too.  Without them the folder of the
 * original game is looked for next to the program, in _references/dos above it (a source checkout) and in the
 * folder chosen last time; otherwise the player picks it (zenity or kdialog, when one is installed).  Saved
 * games go to a Save folder next to the program and the settings to Syndicate64.ini beside it; where that
 * folder cannot be written to both go to $XDG_DATA_HOME/Syndicate64 (~/.local/share/Syndicate64). */
#define _GNU_SOURCE
#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../game/game.h"
#include "../../rt/host.h"
#include "../../video/crt.h"

#ifndef SYN_VERSION
#define SYN_VERSION "1.0.0"
#endif
#define APP_CAPTION "Syndicate Plus 64bit"
#define APP_NAME "Syndicate64"
#define ABOUT_URL "https://github.com/Buyukcaglar/synd"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern FILE *g_logfile; /* posix_sys.c */

static SDL_Window *g_window;
static SDL_Renderer *g_ren;
static pthread_t g_game_thread;
static int g_game_started;
static atomic_int g_quit;
static atomic_int g_game_done;
static int g_full, g_minimized, g_modal, g_focus = 1;
static GameOptions g_opt;
static char g_game_dir[PATH_MAX], g_user_dir[PATH_MAX];
static char g_ini[PATH_MAX];
static Uint32 g_ev_frame, g_ev_done, g_ev_fatal;

static void update_pause(void);
static void update_capture(void);

/* ==== settings (Syndicate64.ini, the same [section] key=value file Windows writes) ======================== */
static int ini_get(const char *section, const char *key, char *out, size_t n)
{
    FILE *f = g_ini[0] ? fopen(g_ini, "r") : NULL;
    char line[2048];
    int in = 0, found = 0;
    if (f == NULL) return 0;
    while (!found && fgets(line, sizeof line, f) != NULL) {
        char *s = line, *e;
        line[strcspn(line, "\r\n")] = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            e = strchr(s, ']');
            if (e) *e = 0;
            in = strcasecmp(s + 1, section) == 0;
        } else if (in && (e = strchr(s, '=')) != NULL) {
            char *k = s, *ke = e;
            while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            *ke = 0;
            if (strcasecmp(k, key) == 0) {
                char *v = e + 1;
                while (*v == ' ' || *v == '\t') v++;
                snprintf(out, n, "%s", v);
                found = 1;
            }
        }
    }
    fclose(f);
    return found;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

/* rewrites the file with `key` in `section` set (other lines kept as they were) */
static void ini_set(const char *section, const char *key, const char *value)
{
    char **lines = NULL, line[2048], tmp[PATH_MAX + 16];
    int n = 0, cap = 0, i, in = 0, replace_at = -1, insert_at = -1;
    FILE *f;
    if (!g_ini[0]) return;
    if ((f = fopen(g_ini, "r")) != NULL) {
        while (fgets(line, sizeof line, f) != NULL) {
            line[strcspn(line, "\r\n")] = 0;
            if (n == cap) {
                char **nl = (char **)realloc(lines, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *nl);
                if (nl == NULL) break;
                lines = nl;
            }
            if ((lines[n] = strdup(line)) == NULL) break;
            n++;
        }
        fclose(f);
    }
    while (n > 0 && lines[n - 1][0] == 0) free(lines[--n]);
    /* the line to replace, else the end of the section, else a new section at the end */
    for (i = 0; i < n && replace_at < 0; i++) {
        char buf[2048], *t, *e;
        snprintf(buf, sizeof buf, "%s", lines[i]);
        t = trim(buf);
        if (*t == '[') {
            if (in) insert_at = i;
            if ((e = strchr(t, ']')) != NULL) *e = 0;
            in = strcasecmp(trim(t + 1), section) == 0;
            if (in) insert_at = -1;
        } else if (in && (e = strchr(t, '=')) != NULL) {
            *e = 0;
            if (strcasecmp(trim(t), key) == 0) replace_at = i;
        }
    }
    if (in && replace_at < 0 && insert_at < 0) insert_at = n;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini);
    if ((f = fopen(tmp, "w")) != NULL) {
        for (i = 0; i < n; i++) {
            if (i == insert_at) fprintf(f, "%s=%s\n", key, value);
            if (i == replace_at)
                fprintf(f, "%s=%s\n", key, value);
            else
                fprintf(f, "%s\n", lines[i]);
        }
        if (insert_at == n) fprintf(f, "%s=%s\n", key, value);
        if (replace_at < 0 && insert_at < 0) fprintf(f, "[%s]\n%s=%s\n", section, key, value);
        if (fclose(f) == 0) rename(tmp, g_ini);
    }
    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* ==== the picture ======================================================================================= */
#define FRAME_MAX_W 640
#define FRAME_MAX_H 480
static SDL_mutex *g_frame_lock;
static uint32_t g_frame[FRAME_MAX_W * FRAME_MAX_H];
static int g_fw, g_fh;
static atomic_int g_present_pending;

void host_present(const uint8_t *pixels, int w, int h, const uint32_t *palette)
{
    int i;
    if (w <= 0 || h <= 0 || w > FRAME_MAX_W || h > FRAME_MAX_H) return;
    SDL_LockMutex(g_frame_lock);
    for (i = 0; i < w * h; i++) g_frame[i] = palette[pixels[i]];
    g_fw = w;
    g_fh = h;
    SDL_UnlockMutex(g_frame_lock);
    /* one update waits on the UI thread at most; it shows the newest frame */
    if (atomic_exchange(&g_present_pending, 1) == 0) {
        SDL_Event e;
        SDL_zero(e);
        e.type = g_ev_frame;
        SDL_PushEvent(&e);
    }
}

/* ---- the CRT scanline filter (src/video/crt.c), rendered in row chunks over all cores ---- */
static int g_crt_on;
static CrtFilter *g_crt;
#define CRT_CHUNK_ROWS 32
#define CRT_MAX_THREADS 16

typedef struct CrtJob {
    const CrtFilter *f;
    uint32_t *dst;
    int stride, h;
    atomic_int next;
} CrtJob;

static void *crt_worker(void *arg)
{
    CrtJob *j = (CrtJob *)arg;
    for (;;) {
        int y0 = atomic_fetch_add(&j->next, CRT_CHUNK_ROWS), y1;
        if (y0 >= j->h) break;
        y1 = y0 + CRT_CHUNK_ROWS < j->h ? y0 + CRT_CHUNK_ROWS : j->h;
        crt_render_rows(j->f, j->dst + (size_t)y0 * (size_t)j->stride, j->stride, y0, y1);
    }
    return NULL;
}

static void crt_render_all(const CrtFilter *f, uint32_t *dst, int stride, int h)
{
    static int ncpu;
    pthread_t th[CRT_MAX_THREADS];
    int started[CRT_MAX_THREADS] = {0};
    CrtJob j;
    int i;
    if (ncpu == 0) {
        ncpu = SDL_GetCPUCount();
        if (ncpu < 1) ncpu = 1;
        if (ncpu > CRT_MAX_THREADS) ncpu = CRT_MAX_THREADS;
    }
    j.f = f;
    j.dst = dst;
    j.stride = stride;
    j.h = h;
    atomic_init(&j.next, 0);
    for (i = 1; i < ncpu; i++) started[i] = pthread_create(&th[i], NULL, crt_worker, &j) == 0;
    crt_worker(&j);
    for (i = 1; i < ncpu; i++)
        if (started[i]) pthread_join(th[i], NULL);
}

/* the largest 4:3 rectangle in the window's drawable, centred (in output pixels) */
static SDL_Rect picture_rect(void)
{
    int ow = 0, oh = 0, w, h;
    SDL_Rect r;
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    if (ow * 3 >= oh * 4) {
        h = oh;
        w = h * 4 / 3;
    } else {
        w = ow;
        h = w * 3 / 4;
    }
    r.x = (ow - w) / 2;
    r.y = (oh - h) / 2;
    r.w = w;
    r.h = h;
    return r;
}

/* the window's coordinates (events) to output pixels: they differ on a HiDPI display */
static void window_scale(double *sx, double *sy)
{
    int ww = 1, wh = 1, ow = 1, oh = 1;
    SDL_GetWindowSize(g_window, &ww, &wh);
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    *sx = ww > 0 ? (double)ow / ww : 1.0;
    *sy = wh > 0 ? (double)oh / wh : 1.0;
}

static SDL_Texture *g_tex;
static int g_tex_w, g_tex_h;
static uint32_t *g_crt_buf;
static size_t g_crt_buf_size;
static uint32_t g_src[FRAME_MAX_W * FRAME_MAX_H];

static SDL_Texture *texture(int w, int h)
{
    if (g_tex != NULL && g_tex_w == w && g_tex_h == h) return g_tex;
    if (g_tex != NULL) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w = w;
    g_tex_h = h;
    return g_tex;
}

static int g_game_mouse;
static void check_game_mouse(void);

static void show_frame(void)
{
    SDL_Rect r = picture_rect();
    SDL_Texture *t = NULL;
    int fw, fh;
    check_game_mouse();
    SDL_LockMutex(g_frame_lock);
    fw = g_fw;
    fh = g_fh;
    if (fw > 0) memcpy(g_src, g_frame, (size_t)fw * (size_t)fh * 4u);
    SDL_UnlockMutex(g_frame_lock);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    if (fw <= 0) {
        SDL_RenderPresent(g_ren);
        return;
    }
    if (g_crt_on && r.w > 0 && r.h > 0) {
        size_t need = (size_t)r.w * (size_t)r.h * 4u;
        if (g_crt == NULL) g_crt = crt_create();
        if (need > g_crt_buf_size) {
            free(g_crt_buf);
            g_crt_buf = (uint32_t *)malloc(need);
            g_crt_buf_size = g_crt_buf != NULL ? need : 0;
        }
        if (g_crt != NULL && g_crt_buf != NULL && crt_prepare(g_crt, g_src, fw, fh, r.w, r.h)) {
            crt_render_all(g_crt, g_crt_buf, r.w, r.h);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
            if ((t = texture(r.w, r.h)) != NULL) SDL_UpdateTexture(t, NULL, g_crt_buf, r.w * 4);
        }
    }
    if (t == NULL) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest"); /* sharp pixels, as on Windows */
        if ((t = texture(fw, fh)) != NULL) SDL_UpdateTexture(t, NULL, g_src, fw * 4);
    }
    if (t != NULL) SDL_RenderCopy(g_ren, t, NULL, &r);
    SDL_RenderPresent(g_ren);
}

/* ==== sound (an SDL audio device fed from a ring the game thread fills) =================================== */
#define AUD_RATE 44100
#define AUD_RING 32768u /* frames */
#define AUD_BLOCK 1024u /* frames asked for per callback */
static SDL_mutex *g_aud_lock;
static int16_t g_ring[AUD_RING * 2];
static uint32_t g_ring_r, g_ring_w; /* frame counters */
static int g_aud_primed;
static uint32_t g_aud_underruns, g_aud_block = AUD_BLOCK;
static SDL_AudioDeviceID g_aud;

int host_audio_rate(void)
{
    return AUD_RATE;
}

void host_audio_push(const int16_t *frames, size_t count)
{
    size_t i;
    SDL_LockMutex(g_aud_lock);
    for (i = 0; i < count; i++) {
        uint32_t k = g_ring_w % AUD_RING;
        g_ring[k * 2] = frames[i * 2];
        g_ring[k * 2 + 1] = frames[i * 2 + 1];
        g_ring_w++;
    }
    if (g_ring_w - g_ring_r > AUD_RING) g_ring_r = g_ring_w - AUD_RING;
    SDL_UnlockMutex(g_aud_lock);
}

static void audio_callback(void *user, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    uint32_t i, have, take, n = (uint32_t)len / 4u, blk = g_aud_block;
    (void)user;
    SDL_LockMutex(g_aud_lock);
    have = g_ring_w - g_ring_r;
    /* after a gap (the start, a pause) wait for two blocks before playing again, so that the small unevenness
     * of the game thread's pace does not empty the ring every other block (a crackle each time) */
    if (!g_aud_primed && have >= blk * 2u) g_aud_primed = 1;
    /* the game's clock and the sound card's drift apart a little: keep the delay near two blocks */
    if (have > blk * 6u) {
        g_ring_r = g_ring_w - blk * 2u;
        have = blk * 2u;
    }
    take = g_aud_primed ? (have < n ? have : n) : 0u;
    for (i = 0; i < n; i++) {
        if (i < take) {
            uint32_t k = (g_ring_r + i) % AUD_RING;
            out[i * 2] = g_ring[k * 2];
            out[i * 2 + 1] = g_ring[k * 2 + 1];
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    g_ring_r += take;
    if (g_aud_primed && take < n) {
        g_aud_primed = 0;
        g_aud_underruns++;
    }
    SDL_UnlockMutex(g_aud_lock);
}

static void audio_start(void)
{
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUD_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUD_BLOCK;
    want.callback = audio_callback;
    g_aud = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* SDL converts if the device differs */
    if (g_aud == 0) {
        char line[256];
        snprintf(line, sizeof line, "sound: no output device (%s)", SDL_GetError());
        host_log(line);
        return;
    }
    if (have.samples > 0) g_aud_block = have.samples;
    SDL_PauseAudioDevice(g_aud, 0);
}

static void audio_stop(void)
{
    if (g_aud == 0) return;
    SDL_CloseAudioDevice(g_aud);
    g_aud = 0;
    if (g_opt.verbose) {
        char line[80];
        snprintf(line, sizeof line, "sound: %u underruns", g_aud_underruns);
        host_log(line);
    }
}

/* ==== the game thread ==================================================================================== */
int host_should_quit(void)
{
    return atomic_load(&g_quit) != 0;
}

static void message(Uint32 flags, const char *text)
{
    g_modal++;
    update_pause();
    if (SDL_ShowSimpleMessageBox(flags, APP_CAPTION, text, g_window) != 0)
        fprintf(stderr, "%s\n", text);
    g_modal--;
    update_pause();
}

static const char *g_fatal_text;
static SDL_sem *g_fatal_done;

void host_fatal(const char *text)
{
    if (g_logfile != NULL) {
        fprintf(g_logfile, "fatal: %s\n", text);
        fflush(g_logfile);
    }
    fprintf(stderr, "fatal: %s\n", text);
    if (g_game_started && pthread_equal(pthread_self(), g_game_thread) && g_fatal_done != NULL) {
        /* from the game thread: the UI thread shows it, the process ends after the message box */
        SDL_Event e;
        SDL_zero(e);
        e.type = g_ev_fatal;
        g_fatal_text = text;
        SDL_PushEvent(&e);
        SDL_SemWait(g_fatal_done);
        _exit(3);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_CAPTION, text, g_window);
    exit(3);
}

static void *game_thread(void *unused)
{
    SDL_Event e;
    (void)unused;
    game_main(&g_opt);
    atomic_store(&g_game_done, 1);
    SDL_zero(e);
    e.type = g_ev_done;
    SDL_PushEvent(&e);
    return NULL;
}

static void update_pause(void)
{
    rt_set_paused(g_minimized || g_modal);
    update_capture();
}

/* ==== the keyboard ======================================================================================= */
/* SDL scancodes (USB HID usages, by position on the keyboard as PC scancodes are) to PC scancode set 1;
 * 0: not for the game */
static uint16_t scan_of(SDL_Scancode s)
{
    static const uint16_t t[SDL_NUM_SCANCODES] = {
        [SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_C] = 0x2E, [SDL_SCANCODE_D] = 0x20,
        [SDL_SCANCODE_E] = 0x12, [SDL_SCANCODE_F] = 0x21, [SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23,
        [SDL_SCANCODE_I] = 0x17, [SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25, [SDL_SCANCODE_L] = 0x26,
        [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_O] = 0x18, [SDL_SCANCODE_P] = 0x19,
        [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_R] = 0x13, [SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_T] = 0x14,
        [SDL_SCANCODE_U] = 0x16, [SDL_SCANCODE_V] = 0x2F, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_X] = 0x2D,
        [SDL_SCANCODE_Y] = 0x15, [SDL_SCANCODE_Z] = 0x2C,
        [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03, [SDL_SCANCODE_3] = 0x04, [SDL_SCANCODE_4] = 0x05,
        [SDL_SCANCODE_5] = 0x06, [SDL_SCANCODE_6] = 0x07, [SDL_SCANCODE_7] = 0x08, [SDL_SCANCODE_8] = 0x09,
        [SDL_SCANCODE_9] = 0x0A, [SDL_SCANCODE_0] = 0x0B,
        [SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_BACKSPACE] = 0x0E,
        [SDL_SCANCODE_TAB] = 0x0F, [SDL_SCANCODE_SPACE] = 0x39, [SDL_SCANCODE_MINUS] = 0x0C,
        [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_LEFTBRACKET] = 0x1A, [SDL_SCANCODE_RIGHTBRACKET] = 0x1B,
        [SDL_SCANCODE_BACKSLASH] = 0x2B, [SDL_SCANCODE_NONUSHASH] = 0x2B, [SDL_SCANCODE_SEMICOLON] = 0x27,
        [SDL_SCANCODE_APOSTROPHE] = 0x28, [SDL_SCANCODE_GRAVE] = 0x29, [SDL_SCANCODE_COMMA] = 0x33,
        [SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH] = 0x35, [SDL_SCANCODE_CAPSLOCK] = 0x3A,
        [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D, [SDL_SCANCODE_F4] = 0x3E,
        [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40, [SDL_SCANCODE_F7] = 0x41, [SDL_SCANCODE_F8] = 0x42,
        [SDL_SCANCODE_F9] = 0x43, [SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_F11] = 0x57,
        [SDL_SCANCODE_PRINTSCREEN] = 0xE037, [SDL_SCANCODE_SCROLLLOCK] = 0x46, [SDL_SCANCODE_PAUSE] = 0,
        [SDL_SCANCODE_INSERT] = 0xE052, [SDL_SCANCODE_HOME] = 0xE047, [SDL_SCANCODE_PAGEUP] = 0xE049,
        [SDL_SCANCODE_DELETE] = 0xE053, [SDL_SCANCODE_END] = 0xE04F, [SDL_SCANCODE_PAGEDOWN] = 0xE051,
        [SDL_SCANCODE_RIGHT] = 0xE04D, [SDL_SCANCODE_LEFT] = 0xE04B, [SDL_SCANCODE_DOWN] = 0xE050,
        [SDL_SCANCODE_UP] = 0xE048, [SDL_SCANCODE_NUMLOCKCLEAR] = 0x45, [SDL_SCANCODE_KP_DIVIDE] = 0xE035,
        [SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_KP_MINUS] = 0x4A, [SDL_SCANCODE_KP_PLUS] = 0x4E,
        [SDL_SCANCODE_KP_ENTER] = 0xE01C, [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50,
        [SDL_SCANCODE_KP_3] = 0x51, [SDL_SCANCODE_KP_4] = 0x4B, [SDL_SCANCODE_KP_5] = 0x4C,
        [SDL_SCANCODE_KP_6] = 0x4D, [SDL_SCANCODE_KP_7] = 0x47, [SDL_SCANCODE_KP_8] = 0x48,
        [SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53,
        [SDL_SCANCODE_NONUSBACKSLASH] = 0x56, [SDL_SCANCODE_INTERNATIONAL1] = 0x73,
        [SDL_SCANCODE_INTERNATIONAL3] = 0x7D,
        [SDL_SCANCODE_LCTRL] = 0x1D, [SDL_SCANCODE_LSHIFT] = 0x2A, [SDL_SCANCODE_LALT] = 0x38,
        [SDL_SCANCODE_RCTRL] = 0xE01D, [SDL_SCANCODE_RSHIFT] = 0x36, [SDL_SCANCODE_RALT] = 0xE038,
    };
    return (unsigned)s < SDL_NUM_SCANCODES ? t[s] : 0;
}

static uint8_t g_keys_down[512]; /* by scancode (+256 for extended keys): what the game holds now */
static int g_buttons;

static int key_slot(uint32_t sc)
{
    return (int)(sc & 0xFFu) + ((sc & 0xE000u) ? 256 : 0);
}

static void key_to_game(uint32_t sc, int down)
{
    int i = key_slot(sc);
    if (!down && !g_keys_down[i]) return; /* a release whose press the game never saw (a shortcut's key) */
    g_keys_down[i] = (uint8_t)down;
    if (g_opt.verbose >= 2) {
        char line[48];
        snprintf(line, sizeof line, "key %04x %s", (unsigned)sc, down ? "down" : "up");
        host_log(line);
    }
    rt_key_event(sc, down);
}

/* Alt (either one) is held back until it is clear it is not one of the window's shortcuts */
static int g_alt_held[2], g_alt_used[2];
static const uint16_t g_alt_sc[2] = {0x38, 0xE038};

static void alt_flush(void)
{
    int a;
    for (a = 0; a < 2; a++)
        if (g_alt_held[a] && !g_alt_used[a]) {
            g_alt_held[a] = 0;
            key_to_game(g_alt_sc[a], 1);
        }
}

static void release_all(void)
{
    int i;
    g_alt_held[0] = g_alt_held[1] = g_alt_used[0] = g_alt_used[1] = 0;
    for (i = 0; i < 512; i++) {
        if (g_keys_down[i]) {
            g_keys_down[i] = 0;
            rt_key_event(i >= 256 ? (0xE000u | (uint32_t)(i - 256)) : (uint32_t)i, 0);
        }
    }
    if (g_buttons) {
        g_buttons = 0;
        rt_mouse_buttons(0);
    }
}

static void toggle_full(void);
static void toggle_crt(void);
static void about(void);

static void key_event(const SDL_KeyboardEvent *k, int down)
{
    SDL_Scancode s = k->keysym.scancode;
    uint32_t sc;
    int alt_down = g_alt_held[0] || g_alt_held[1] || g_keys_down[key_slot(0x38)] || g_keys_down[key_slot(0xE038)];
    if (s == SDL_SCANCODE_LALT || s == SDL_SCANCODE_RALT) {
        int a = s == SDL_SCANCODE_RALT;
        if (down) {
            if (!k->repeat && !g_keys_down[key_slot(g_alt_sc[a])]) {
                g_alt_held[a] = 1;
                g_alt_used[a] = 0;
            }
        } else {
            if (g_alt_held[a] && !g_alt_used[a]) key_to_game(g_alt_sc[a], 1); /* alone: the game gets it now */
            g_alt_held[a] = 0;
            g_alt_used[a] = 0;
            key_to_game(g_alt_sc[a], 0);
        }
        return;
    }
    if (down && alt_down && (s == SDL_SCANCODE_RETURN || s == SDL_SCANCODE_KP_ENTER || s == SDL_SCANCODE_S)) {
        /* Alt+Enter: full screen; Alt+S: the CRT scanline filter.  Neither the key nor the Alt reaches the game */
        g_alt_used[0] = g_alt_held[0];
        g_alt_used[1] = g_alt_held[1];
        if (!k->repeat) {
            if (s == SDL_SCANCODE_S)
                toggle_crt();
            else
                toggle_full();
        }
        return;
    }
    if (down && s == SDL_SCANCODE_F12) {
        if (!k->repeat) about();
        return;
    }
    sc = scan_of(s);
    if (sc == 0) return;
    if (down) {
        alt_flush();
        key_to_game(sc, 1); /* auto-repeat sends the make code again, as a PC keyboard does */
    } else {
        key_to_game(sc, 0);
    }
}

/* ==== the pointer ======================================================================================== */
/* In full screen, while the game has the focus and uses the mouse, the pointer is captured (SDL's relative
 * mode): the motion moves a pointer of our own that cannot leave the picture (the game draws it). */
static int g_captured;
static double g_cap_x, g_cap_y;   /* the captured pointer, in output pixels */
static double g_last_x, g_last_y; /* where the game's pointer was put last, in output pixels */
static Uint32 g_last_motion;

static void send_pos(double x, double y)
{
    SDL_Rect r = picture_rect();
    double fx, fy;
    if (r.w <= 1 || r.h <= 1) return;
    g_last_x = x;
    g_last_y = y;
    fx = (x - r.x) * 65535.0 / (r.w - 1);
    fy = (y - r.y) * 65535.0 / (r.h - 1);
    if (fx < 0) fx = 0;
    if (fx > 65535) fx = 65535;
    if (fy < 0) fy = 0;
    if (fy > 65535) fy = 65535;
    rt_mouse_move((int)(fx + 0.5), (int)(fy + 0.5));
}

static void clamp_to_picture(double *x, double *y)
{
    SDL_Rect r = picture_rect();
    if (*x < r.x) *x = r.x;
    if (*x > r.x + r.w - 1) *x = r.x + r.w - 1;
    if (*y < r.y) *y = r.y;
    if (*y > r.y + r.h - 1) *y = r.y + r.h - 1;
}

static int in_picture(double x, double y)
{
    SDL_Rect r = picture_rect();
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void update_cursor(void)
{
    int mx, my;
    double sx, sy;
    int show = 1;
    if (g_captured) return;
    SDL_GetMouseState(&mx, &my);
    window_scale(&sx, &sy);
    if (in_picture(mx * sx, my * sy)) {
        /* the game draws its own pointer; without one the system's hides after two seconds still */
        if (g_game_mouse || SDL_GetTicks() - g_last_motion > 2000u) show = 0;
    }
    if (g_modal) show = 1;
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

static void update_capture(void)
{
    int want = g_full && g_game_mouse && !g_minimized && !g_modal && g_focus;
    if (g_window == NULL) return;
    if (want && !g_captured) {
        int mx, my;
        double sx, sy, x, y;
        SDL_GetMouseState(&mx, &my);
        window_scale(&sx, &sy);
        x = mx * sx;
        y = my * sy;
        if (!in_picture(x, y)) {
            x = g_last_x;
            y = g_last_y;
        }
        clamp_to_picture(&x, &y);
        g_cap_x = x;
        g_cap_y = y;
        if (SDL_SetRelativeMouseMode(SDL_TRUE) == 0) {
            g_captured = 1;
            send_pos(g_cap_x, g_cap_y);
        }
    } else if (!want && g_captured) {
        double sx, sy;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        window_scale(&sx, &sy);
        SDL_WarpMouseInWindow(g_window, (int)(g_cap_x / sx), (int)(g_cap_y / sy)); /* where the game's was */
        g_captured = 0;
    }
    update_cursor();
}

/* the running program started or stopped using the mouse (checked with every frame) */
static void check_game_mouse(void)
{
    int used = rt_mouse_used() != 0;
    if (used == g_game_mouse) return;
    g_game_mouse = used;
    update_capture();
}

static void mouse_motion(const SDL_MouseMotionEvent *m)
{
    double sx, sy, x, y;
    window_scale(&sx, &sy);
    g_last_motion = SDL_GetTicks();
    if (g_captured) {
        g_cap_x += m->xrel * sx;
        g_cap_y += m->yrel * sy;
        clamp_to_picture(&g_cap_x, &g_cap_y);
        send_pos(g_cap_x, g_cap_y);
        return;
    }
    x = m->x * sx;
    y = m->y * sy;
    send_pos(x, y);
    update_cursor();
}

static void mouse_button(const SDL_MouseButtonEvent *b, int down)
{
    int bit = b->button == SDL_BUTTON_LEFT ? 1 : b->button == SDL_BUTTON_RIGHT ? 2 : b->button == SDL_BUTTON_MIDDLE ? 4 : 0;
    int nb;
    if (bit == 0) return;
    if (down) alt_flush();
    if (!g_captured) {
        double sx, sy;
        window_scale(&sx, &sy);
        send_pos(b->x * sx, b->y * sy);
    }
    nb = down ? (g_buttons | bit) : (g_buttons & ~bit);
    if (nb != g_buttons) {
        g_buttons = nb;
        rt_mouse_buttons(nb);
    }
}

/* ==== the window ========================================================================================= */
static void toggle_full(void)
{
    release_all();
    g_full = !g_full;
    SDL_SetWindowFullscreen(g_window, g_full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    update_capture();
    show_frame();
}

static void toggle_crt(void)
{
    g_crt_on = !g_crt_on;
    ini_set("video", "crt", g_crt_on ? "1" : "0");
    show_frame();
}

static void about(void)
{
    char text[1024];
    release_all();
    snprintf(text, sizeof text,
             "Syndicate Plus 64bit %s\n\n"
             "A static recompilation of Syndicate Plus (Bullfrog, 1993) for 64-bit systems.\n"
             "64bit version by Onur BUYUKCAGLAR, with Anthropic Claude AI assistance.\n\n"
             "Alt+Enter: full screen    Alt+S: CRT scanlines\n\n"
             "The code of this port is under the MIT licence. Syndicate, Syndicate: American Revolt and "
             "Syndicate Plus belong to their owners; they are not part of this software.\n\n%s",
             SYN_VERSION, ABOUT_URL);
    message(SDL_MESSAGEBOX_INFORMATION, text);
}

/* ==== where the program reads and writes ================================================================= */
static char g_exe_dir[PATH_MAX];

/* the folder that holds the program */
static const char *app_dir(void)
{
    if (!g_exe_dir[0]) {
        char *base = SDL_GetBasePath();
        size_t n;
        snprintf(g_exe_dir, sizeof g_exe_dir, "%s", base != NULL ? base : "./");
        SDL_free(base);
        n = strlen(g_exe_dir);
        while (n > 1 && g_exe_dir[n - 1] == '/') g_exe_dir[--n] = 0;
    }
    return g_exe_dir;
}

static void mkdirs(const char *path)
{
    char p[PATH_MAX], *s;
    snprintf(p, sizeof p, "%s", path);
    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

/* the folder exists (made if need be) and a file can be created in it */
static int writable_dir(const char *dir)
{
    char probe[PATH_MAX + 64];
    int fd;
    mkdirs(dir);
    snprintf(probe, sizeof probe, "%s/.syndicate64-%d.tmp", dir, (int)getpid());
    fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return 0;
    close(fd);
    unlink(probe);
    return 1;
}

/* $XDG_DATA_HOME/Syndicate64, else ~/.local/share/Syndicate64 */
static void local_dir(char *out, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (x != NULL && x[0] == '/')
        snprintf(out, n, "%s/" APP_NAME, x);
    else
        snprintf(out, n, "%s/.local/share/" APP_NAME, home != NULL ? home : ".");
    mkdirs(out);
}

/* the saved games and every other file the game writes: Save next to the program, else the local folder */
static void save_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/Save", app_dir());
    if (!writable_dir(out)) local_dir(out, n);
}

/* the settings: Syndicate64.ini next to the program, else in the local folder */
static void settings_file(char *out, size_t n)
{
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s", app_dir());
    if (!writable_dir(d)) local_dir(d, sizeof d);
    snprintf(out, n, "%s/" APP_NAME ".ini", d);
}

/* ==== finding the game =================================================================================== */
static int has_game(const char *dir)
{
    char out[300];
    return dir != NULL && dir[0] && host_find_name(dir, "SYND.EXE", out, sizeof out);
}

/* a folder dialog of the desktop (zenity or kdialog); 0 when there is none or the player cancelled */
static int pick_folder(char *out, size_t n)
{
    static const char *cmds[] = {
        "zenity --file-selection --directory --title='Choose the Syndicate Plus folder (the one with SYND.EXE)' 2>/dev/null",
        "kdialog --getexistingdirectory \"$HOME\" --title 'Choose the Syndicate Plus folder (the one with SYND.EXE)' 2>/dev/null",
        "yad --file --directory --title='Choose the Syndicate Plus folder (the one with SYND.EXE)' 2>/dev/null",
    };
    static const char *tools[] = {"zenity", "kdialog", "yad"};
    size_t i;
    for (i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        char probe[128], line[PATH_MAX];
        FILE *p;
        int rc;
        snprintf(probe, sizeof probe, "command -v %s >/dev/null 2>&1", tools[i]);
        if (system(probe) != 0) continue;
        p = popen(cmds[i], "r");
        if (p == NULL) continue;
        line[0] = 0;
        if (fgets(line, sizeof line, p) == NULL) line[0] = 0;
        rc = pclose(p);
        line[strcspn(line, "\r\n")] = 0;
        if (rc != 0 || line[0] == 0) return 0;
        snprintf(out, n, "%s", line);
        return 1;
    }
    return -1; /* no dialog program */
}

static int find_game(char *out, size_t n)
{
    char dir[PATH_MAX], probe[PATH_MAX + 32];
    int up, r;
    const char *here = app_dir();
    /* next to the program */
    if (has_game(here)) return snprintf(out, n, "%s", here), 1;
    snprintf(probe, sizeof probe, "%s/SYNDPLUS", here);
    if (has_game(probe)) return snprintf(out, n, "%s", probe), 1;
    /* a source checkout: _references/dos in the program's folder or above it */
    snprintf(dir, sizeof dir, "%s", here);
    for (up = 0; up < 5; up++) {
        char *s;
        snprintf(probe, sizeof probe, "%s/_references/dos", dir);
        if (has_game(probe)) return snprintf(out, n, "%s", probe), 1;
        s = strrchr(dir, '/');
        if (s == NULL || s == dir) break;
        *s = 0;
    }
    /* the folder chosen before */
    if (ini_get("game", "folder", dir, sizeof dir) && has_game(dir)) return snprintf(out, n, "%s", dir), 1;
    /* ask */
    for (;;) {
        r = pick_folder(dir, sizeof dir);
        if (r < 0) {
            message(SDL_MESSAGEBOX_ERROR,
                    "The Syndicate Plus game files were not found.\n\n"
                    "Start the program with --game-dir /path/to/game (the folder with SYND.EXE), or put that "
                    "folder next to the program, or install zenity or kdialog to choose it here.");
            return 0;
        }
        if (r == 0) return 0;
        if (has_game(dir)) break;
        message(SDL_MESSAGEBOX_WARNING,
                "That folder does not hold the original Syndicate Plus (SYND.EXE was not found in it). "
                "Choose the folder the game was installed to.");
    }
    ini_set("game", "folder", dir);
    return snprintf(out, n, "%s", dir), 1;
}

/* the game files built into the program (linux_pack.c of a private build), else a syndgame.pak next to it,
 * mapped into memory once; NULL when there are none */
#ifdef SYN_EMBED_GAME
extern const uint8_t syn_pack_start[], syn_pack_end[];
#endif

const uint8_t *host_game_pack(size_t *size)
{
    static const uint8_t *pack;
    static size_t pack_size;
    static int tried;
    if (!tried) {
        tried = 1;
#ifdef SYN_EMBED_GAME
        {
            size_t n = (size_t)((uintptr_t)syn_pack_end - (uintptr_t)syn_pack_start);
            if (n > 0) {
                pack = syn_pack_start;
                pack_size = n;
            }
        }
#endif
        if (pack == NULL) {
            char p[PATH_MAX + 32];
            int fd;
            struct stat st;
            snprintf(p, sizeof p, "%s/syndgame.pak", app_dir());
            fd = open(p, O_RDONLY);
            if (fd >= 0) {
                if (fstat(fd, &st) == 0 && st.st_size > 0) {
                    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (m != MAP_FAILED) {
                        pack = (const uint8_t *)m;
                        pack_size = (size_t)st.st_size;
                    }
                }
                close(fd);
            }
        }
    }
    *size = pack_size;
    return pack;
}

/* ==== start ============================================================================================== */
static void start_game_thread(void)
{
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20); /* as on Windows: the core's guest calls nest deeply */
    if (pthread_create(&g_game_thread, &at, game_thread, NULL) == 0) g_game_started = 1;
    pthread_attr_destroy(&at);
}

static void shutdown_game(void)
{
    atomic_store(&g_quit, 1);
    rt_set_paused(0);
    if (g_game_started) {
        pthread_join(g_game_thread, NULL);
        g_game_started = 0;
    }
}

int main(int argc, char **argv)
{
    static char start[512];
    const char *arg_game = NULL, *arg_user = NULL;
    int i, start_full = 0, scale, running = 1;
    size_t pack_size;
    char v[16];
    SDL_DisplayMode dm;

    memset(&g_opt, 0, sizeof g_opt);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--game-dir")) {
            arg_game = val;
            i++;
        } else if (!strcmp(a, "--user-dir")) {
            arg_user = val;
            i++;
        } else if (!strcmp(a, "--start")) {
            char *c;
            snprintf(start, sizeof start, "%s", val);
            for (c = start; *c && *c != ' '; c++) /* the program's path; its arguments stay as given */
                if (*c == '/') *c = '\\';
            g_opt.start = start;
            i++;
        } else if (!strcmp(a, "--log")) {
            g_logfile = fopen(val, "w");
            i++;
        } else if (!strcmp(a, "--verbose")) {
            g_opt.verbose = atoi(val);
            i++;
        } else if (!strcmp(a, "--fullscreen")) {
            start_full = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("usage: %s [--game-dir DIR] [--user-dir DIR] [--start \"SYNDICAT\\MAIN.EXE /args\"] "
                   "[--log FILE] [--verbose N] [--fullscreen]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option %s (see --help)\n", a);
            return 2;
        }
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        /* sound is optional: try again without it */
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "%s: SDL could not start: %s\n", APP_NAME, SDL_GetError());
            return 1;
        }
    }
    g_frame_lock = SDL_CreateMutex();
    g_aud_lock = SDL_CreateMutex();
    g_fatal_done = SDL_CreateSemaphore(0);
    g_ev_frame = SDL_RegisterEvents(3);
    g_ev_done = g_ev_frame + 1;
    g_ev_fatal = g_ev_frame + 2;

    settings_file(g_ini, sizeof g_ini);
    if (arg_game != NULL || host_game_pack(&pack_size) == NULL) { /* else: the built-in files */
        if (arg_game == NULL) {
            if (!find_game(g_game_dir, sizeof g_game_dir)) {
                SDL_Quit();
                return 1;
            }
        } else {
            snprintf(g_game_dir, sizeof g_game_dir, "%s", arg_game);
        }
        if (!has_game(g_game_dir)) {
            message(SDL_MESSAGEBOX_ERROR, "SYND.EXE was not found in the game folder.");
            SDL_Quit();
            return 1;
        }
    }
    if (arg_user != NULL)
        snprintf(g_user_dir, sizeof g_user_dir, "%s", arg_user);
    else
        save_dir(g_user_dir, sizeof g_user_dir);
    mkdirs(g_user_dir);
    g_crt_on = ini_get("video", "crt", v, sizeof v) && atoi(v) != 0;
    g_opt.game_dir = g_game_dir;
    g_opt.user_dir = g_user_dir;
    if (g_opt.verbose == 0) g_opt.verbose = g_logfile != NULL ? 1 : 0;
    if (g_opt.verbose) {
        char line[PATH_MAX * 2];
        snprintf(line, sizeof line, "game folder: %s", g_game_dir[0] ? g_game_dir : "(built in)");
        host_log(line);
        snprintf(line, sizeof line, "user folder: %s", g_user_dir);
        host_log(line);
        snprintf(line, sizeof line, "settings: %s", g_ini);
        host_log(line);
        snprintf(line, sizeof line, "video driver: %s", SDL_GetCurrentVideoDriver());
        host_log(line);
    }

    /* the largest whole multiple of 320x240 that fits 85% of the screen */
    scale = 3;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0)
        for (scale = 6; scale > 1; scale--)
            if (320 * scale <= dm.w * 85 / 100 && 240 * scale <= dm.h * 85 / 100) break;
    g_window = SDL_CreateWindow(APP_CAPTION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale,
                                240 * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (g_window == NULL) {
        fprintf(stderr, "%s: no window: %s\n", APP_NAME, SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(g_window, 320, 240);
    g_ren = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (g_ren == NULL) g_ren = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (g_ren == NULL) {
        fprintf(stderr, "%s: no renderer: %s\n", APP_NAME, SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (start_full) {
        g_full = 1;
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    show_frame();
    g_last_motion = SDL_GetTicks();

    if (SDL_WasInit(SDL_INIT_AUDIO)) audio_start();
    start_game_thread();

    while (running) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 250)) {
            update_cursor(); /* the idle pointer hides after two seconds */
            continue;
        }
        do {
            if (e.type == g_ev_frame) {
                atomic_store(&g_present_pending, 0);
                show_frame();
            } else if (e.type == g_ev_done) {
                running = 0;
            } else if (e.type == g_ev_fatal) {
                release_all();
                message(SDL_MESSAGEBOX_ERROR, g_fatal_text != NULL ? g_fatal_text : "The game stopped with an error.");
                SDL_SemPost(g_fatal_done);
                for (;;) SDL_Delay(1000); /* the game thread ends the process */
            } else {
                switch (e.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    key_event(&e.key, e.type == SDL_KEYDOWN);
                    break;
                case SDL_MOUSEMOTION:
                    mouse_motion(&e.motion);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    mouse_button(&e.button, e.type == SDL_MOUSEBUTTONDOWN);
                    break;
                case SDL_WINDOWEVENT:
                    switch (e.window.event) {
                    case SDL_WINDOWEVENT_MINIMIZED:
                        g_minimized = 1;
                        release_all();
                        update_pause();
                        break;
                    case SDL_WINDOWEVENT_RESTORED:
                    case SDL_WINDOWEVENT_MAXIMIZED:
                    case SDL_WINDOWEVENT_SHOWN:
                        if (g_minimized) {
                            g_minimized = 0;
                            update_pause();
                        }
                        show_frame();
                        break;
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                        g_focus = 1;
                        update_capture();
                        break;
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        g_focus = 0;
                        release_all();
                        update_capture();
                        break;
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_EXPOSED:
                        if (g_captured) clamp_to_picture(&g_cap_x, &g_cap_y);
                        show_frame();
                        break;
                    case SDL_WINDOWEVENT_CLOSE:
                        running = 0;
                        break;
                    }
                    break;
                }
            }
        } while (running && SDL_PollEvent(&e));
    }

    if (g_captured) SDL_SetRelativeMouseMode(SDL_FALSE);
    shutdown_game();
    audio_stop();
    if (g_crt != NULL) crt_destroy(g_crt);
    if (g_tex != NULL) SDL_DestroyTexture(g_tex);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    if (g_logfile != NULL) fflush(g_logfile);
    return 0;
}
