/* macos_main.m - the macOS front end of Syndicate Plus 64bit: one window that shows the game, feeds it the
 * keyboard and the mouse and plays its sound.  The portable core (game_main) runs on its own thread; this file
 * is the UI (main) thread.  It behaves as the Windows front end (platform/win32/win32_main.c) does, in the
 * ways a Mac does things:
 *
 * - the window and its About box follow the system's dark or light appearance (AppKit does this by itself);
 * - the View menu has "Full Screen" (Ctrl+Cmd+F, native macOS full screen on the window's own display) and
 *   "CRT Scanlines" (Cmd+Shift+S; off at first, remembered in Syndicate64.ini); the app menu has the About box;
 * - in full screen the pointer stays on the picture while the game has the focus (free in menus, dialogs and
 *   when another app is active);
 * - Option is the game's Alt; Cmd shortcuts belong to the app and no key of a shortcut reaches the game;
 * - Ctrl+click is the right button, as on every Mac.
 *
 *   Syndicate64.app/Contents/MacOS/Syndicate64 [--game-dir DIR] [--user-dir DIR] [--start "SYNDICAT\MAIN.EXE /args"]
 *                                             [--log FILE] [--verbose N] [--fullscreen]
 *
 * The original game files are built into a private build (SYN_EMBED_GAME: Contents/Resources/syndgame.pak,
 * mapped into memory) and used unless --game-dir names a folder.  Without them the folder of the original game
 * is looked for next to the app, in _references/dos above it (a source checkout), and in the folder chosen last
 * time; otherwise the player picks it.  Saved games go to a Save folder next to the app and the settings to
 * Syndicate64.ini beside it; where that folder cannot be written to (a read-only volume, or App Translocation of
 * a downloaded app) both go to ~/Library/Application Support/Syndicate64. */
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <fcntl.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../game/game.h"
#include "../../rt/host.h"
#include "../../video/crt.h"

#ifdef SYN_DEVTEST
#include "macos_devtest.h"
#endif

#ifndef SYN_VERSION
#define SYN_VERSION "1.0.0"
#endif
#define APP_CAPTION @"Syndicate Plus 64bit"
#define APP_NAME @"Syndicate64"
#define ABOUT_URL @"https://github.com/Buyukcaglar/synd"

extern FILE *g_logfile; /* posix_sys.c */

static NSWindow *g_window;
static pthread_t g_game_thread;
static int g_game_started;
static atomic_int g_quit;
static atomic_int g_game_done;
static int g_full, g_fs_transition;
static int g_minimized, g_modal, g_in_menu;
static GameOptions g_opt;
static char g_game_dir[PATH_MAX], g_user_dir[PATH_MAX];
static char g_ini[PATH_MAX]; /* Syndicate64.ini: next to the app, else in ~/Library/Application Support/Syndicate64 */

static void update_pause(void);
static void update_capture(void);

/* runs `b` on the main thread in every run loop mode the app uses, a modal dialog's included (GCD's main queue
 * waits while a dialog runs; the frame updates and an error report must not) */
static void on_main(dispatch_block_t b, int wait)
{
    if ([NSThread isMainThread]) {
        b();
        return;
    }
    if (wait) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_block_t inner = b;
        b = ^{
          inner();
          dispatch_semaphore_signal(done);
        };
        CFRunLoopPerformBlock(CFRunLoopGetMain(), (__bridge CFArrayRef) @[ (__bridge NSString *)kCFRunLoopCommonModes, NSModalPanelRunLoopMode ], b);
        CFRunLoopWakeUp(CFRunLoopGetMain());
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        return;
    }
    CFRunLoopPerformBlock(CFRunLoopGetMain(), (__bridge CFArrayRef) @[ (__bridge NSString *)kCFRunLoopCommonModes, NSModalPanelRunLoopMode ], b);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

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

/* rewrites the file with `key` in `section` set (other lines kept as they were) */
static void ini_set(const char *section, const char *key, const char *value)
{
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    NSString *text, *sec = [NSString stringWithUTF8String:section], *k = [NSString stringWithUTF8String:key];
    NSString *entry = [NSString stringWithFormat:@"%s=%s", key, value];
    NSInteger i, sec_at = -1, end_at = -1;
    BOOL done = NO;
    if (!g_ini[0]) return;
    text = [NSString stringWithContentsOfFile:@(g_ini) encoding:NSUTF8StringEncoding error:nil];
    if (text != nil)
        for (NSString *l in [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]])
            [lines addObject:l];
    while (lines.count > 0 && [lines.lastObject length] == 0) [lines removeLastObject];
    for (i = 0; i < (NSInteger)lines.count && !done; i++) {
        NSString *t = [lines[i] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"["]) {
            if (sec_at >= 0 && end_at < 0) end_at = i;
            if ([[t stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"[]"]]
                    caseInsensitiveCompare:sec] == NSOrderedSame) {
                sec_at = i;
                end_at = -1;
            }
        } else if (sec_at >= 0 && end_at < 0) {
            NSRange eq = [t rangeOfString:@"="];
            if (eq.location != NSNotFound &&
                [[[t substringToIndex:eq.location] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]
                    caseInsensitiveCompare:k] == NSOrderedSame) {
                lines[i] = entry;
                done = YES;
            }
        }
    }
    if (!done) {
        if (sec_at < 0) {
            [lines addObject:[NSString stringWithFormat:@"[%@]", sec]];
            [lines addObject:entry];
        } else {
            [lines insertObject:entry atIndex:end_at >= 0 ? (NSUInteger)end_at : lines.count];
        }
    }
    [[[lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"] writeToFile:@(g_ini)
                                                                              atomically:YES
                                                                                encoding:NSUTF8StringEncoding
                                                                                   error:nil];
}

/* ==== the picture ======================================================================================= */
#define FRAME_MAX_W 640
#define FRAME_MAX_H 480
static os_unfair_lock g_frame_lock = OS_UNFAIR_LOCK_INIT;
static uint32_t g_frame[FRAME_MAX_W * FRAME_MAX_H];
static int g_fw, g_fh;
static atomic_int g_present_pending;

@interface SynView : NSView
- (void)showFrame;
- (NSRect)pictureRect;
- (void)idleHide;
@end
static SynView *g_view;

void host_present(const uint8_t *pixels, int w, int h, const uint32_t *palette)
{
    int i;
    if (w <= 0 || h <= 0 || w > FRAME_MAX_W || h > FRAME_MAX_H) return;
    os_unfair_lock_lock(&g_frame_lock);
    for (i = 0; i < w * h; i++) g_frame[i] = palette[pixels[i]];
    g_fw = w;
    g_fh = h;
    os_unfair_lock_unlock(&g_frame_lock);
    /* one update waits on the main thread at most; it shows the newest frame */
    if (atomic_exchange(&g_present_pending, 1) == 0)
        on_main(^{
          atomic_store(&g_present_pending, 0);
          [g_view showFrame];
        }, 0);
}

/* ---- the CRT scanline filter (src/video/crt.c), off unless chosen in the View menu ---- */
static int g_crt_on;
static CrtFilter *g_crt;
#define CRT_CHUNK_ROWS 32

static void release_pixels(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

/* an image of `w` x `h` pixels 0x00RRGGBB that takes over `pixels` (malloc'd) */
static CGImageRef make_image(uint32_t *pixels, int w, int h)
{
    static CGColorSpaceRef cs;
    CGDataProviderRef dp;
    CGImageRef img;
    if (cs == NULL) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    dp = CGDataProviderCreateWithData(NULL, pixels, (size_t)w * (size_t)h * 4u, release_pixels);
    img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4u, cs,
                        kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, dp, NULL, false,
                        kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    return img;
}

/* ==== sound (an AudioQueue fed from a ring the game thread fills) ========================================= */
#define AUD_RATE 44100
#define AUD_RING 32768u /* frames */
#define AUD_BLOCK 1024u /* frames per AudioQueue buffer */
#define AUD_NBUF 4
static os_unfair_lock g_aud_lock = OS_UNFAIR_LOCK_INIT;
static int16_t g_ring[AUD_RING * 2];
static uint32_t g_ring_r, g_ring_w; /* frame counters */
static int g_aud_primed;
static uint32_t g_aud_underruns;
static AudioQueueRef g_aq;
static uint64_t g_aud_t0;

int host_audio_rate(void)
{
    return AUD_RATE;
}

void host_audio_push(const int16_t *frames, size_t count)
{
    size_t i;
    os_unfair_lock_lock(&g_aud_lock);
    for (i = 0; i < count; i++) {
        uint32_t k = g_ring_w % AUD_RING;
        g_ring[k * 2] = frames[i * 2];
        g_ring[k * 2 + 1] = frames[i * 2 + 1];
        g_ring_w++;
    }
    if (g_ring_w - g_ring_r > AUD_RING) g_ring_r = g_ring_w - AUD_RING;
    os_unfair_lock_unlock(&g_aud_lock);
}

static void audio_fill(int16_t *out)
{
    uint32_t i, have, take;
    os_unfair_lock_lock(&g_aud_lock);
    have = g_ring_w - g_ring_r;
    /* after a gap (the start, a pause) wait for two blocks before playing again, so that the small unevenness
     * of the game thread's pace does not empty the ring every other block (a crackle each time) */
    if (!g_aud_primed && have >= AUD_BLOCK * 2u) g_aud_primed = 1;
    /* the game's clock and the sound card's drift apart a little: keep the delay near two blocks */
    if (have > AUD_BLOCK * 6u) {
        g_ring_r = g_ring_w - AUD_BLOCK * 2u;
        have = AUD_BLOCK * 2u;
    }
    take = g_aud_primed ? (have < AUD_BLOCK ? have : AUD_BLOCK) : 0u;
    for (i = 0; i < AUD_BLOCK; i++) {
        if (i < take) {
            uint32_t k = (g_ring_r + i) % AUD_RING;
            out[i * 2] = g_ring[k * 2];
            out[i * 2 + 1] = g_ring[k * 2 + 1];
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    g_ring_r += take;
    if (g_aud_primed && take < AUD_BLOCK) {
        g_aud_primed = 0;
        g_aud_underruns++;
        if (g_opt.verbose >= 2) {
            char line[64];
            snprintf(line, sizeof line, "sound: underrun %.3f s after the start", (double)(host_time_us() - g_aud_t0) / 1e6);
            os_unfair_lock_unlock(&g_aud_lock);
            host_log(line);
            return;
        }
    }
    os_unfair_lock_unlock(&g_aud_lock);
}

static void aq_callback(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
    (void)user;
    audio_fill((int16_t *)b->mAudioData);
    b->mAudioDataByteSize = AUD_BLOCK * 4u;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void audio_start(void)
{
    AudioStreamBasicDescription f;
    int i;
    memset(&f, 0, sizeof f);
    f.mSampleRate = AUD_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    g_aud_t0 = host_time_us();
    /* a NULL run loop: the callbacks come on the queue's own high-priority thread */
    if (AudioQueueNewOutput(&f, aq_callback, NULL, NULL, NULL, 0, &g_aq) != noErr) {
        g_aq = NULL;
        host_log("sound: no output device");
        return;
    }
    for (i = 0; i < AUD_NBUF; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(g_aq, AUD_BLOCK * 4u, &b) != noErr) continue;
        memset(b->mAudioData, 0, AUD_BLOCK * 4u);
        b->mAudioDataByteSize = AUD_BLOCK * 4u;
        AudioQueueEnqueueBuffer(g_aq, b, 0, NULL);
    }
    if (AudioQueueStart(g_aq, NULL) != noErr) host_log("sound: the output did not start");
}

static void audio_stop(void)
{
    if (g_aq == NULL) return;
    AudioQueueStop(g_aq, true);
    AudioQueueDispose(g_aq, true);
    g_aq = NULL;
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

static void alert(NSString *text, NSAlertStyle style)
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = APP_CAPTION;
    a.informativeText = text;
    a.alertStyle = style;
    [a runModal];
}

void host_fatal(const char *text)
{
    NSString *s = [NSString stringWithUTF8String:text] ?: @"The game stopped with an error.";
    void (^show)(void) = ^{
      g_modal++;
      update_pause();
      alert(s, NSAlertStyleCritical);
      exit(3);
    };
    if (g_logfile != NULL) fflush(g_logfile);
    on_main(show, 1); /* from the game thread: it waits here, the process ends after the alert */
    exit(3);
}

static void *game_thread(void *unused)
{
    (void)unused;
    game_main(&g_opt);
    atomic_store(&g_game_done, 1);
    on_main(^{
      [NSApp terminate:nil];
    }, 0);
    return NULL;
}

static void update_pause(void)
{
    rt_set_paused(g_minimized || g_modal);
    update_capture();
}

/* ==== the keyboard ======================================================================================= */
/* macOS virtual key codes (kVK_*, by position on the keyboard, as scancodes are) to PC scancode set 1;
 * 0: not for the game (Command, Fn, media keys) */
static uint16_t g_scan[128] = {
    [0x00] = 0x1E, [0x01] = 0x1F, [0x02] = 0x20, [0x03] = 0x21, [0x04] = 0x23, [0x05] = 0x22, [0x06] = 0x2C,
    [0x07] = 0x2D, [0x08] = 0x2E, [0x09] = 0x2F, [0x0A] = 0x56, [0x0B] = 0x30, [0x0C] = 0x10, [0x0D] = 0x11,
    [0x0E] = 0x12, [0x0F] = 0x13, [0x10] = 0x15, [0x11] = 0x14, [0x12] = 0x02, [0x13] = 0x03, [0x14] = 0x04,
    [0x15] = 0x05, [0x16] = 0x07, [0x17] = 0x06, [0x18] = 0x0D, [0x19] = 0x0A, [0x1A] = 0x08, [0x1B] = 0x0C,
    [0x1C] = 0x09, [0x1D] = 0x0B, [0x1E] = 0x1B, [0x1F] = 0x18, [0x20] = 0x16, [0x21] = 0x1A, [0x22] = 0x17,
    [0x23] = 0x19, [0x24] = 0x1C, [0x25] = 0x26, [0x26] = 0x24, [0x27] = 0x28, [0x28] = 0x25, [0x29] = 0x27,
    [0x2A] = 0x2B, [0x2B] = 0x33, [0x2C] = 0x35, [0x2D] = 0x31, [0x2E] = 0x32, [0x2F] = 0x34, [0x30] = 0x0F,
    [0x31] = 0x39, [0x32] = 0x29, [0x33] = 0x0E, [0x35] = 0x01,
    [0x38] = 0x2A,   /* Shift */
    [0x39] = 0x3A,   /* Caps Lock */
    [0x3A] = 0x38,   /* Option: Alt */
    [0x3B] = 0x1D,   /* Control */
    [0x3C] = 0x36,   /* right Shift */
    [0x3D] = 0xE038, /* right Option: right Alt */
    [0x3E] = 0xE01D, /* right Control */
    [0x41] = 0x53, [0x43] = 0x37, [0x45] = 0x4E,
    [0x47] = 0x45,   /* keypad Clear: Num Lock */
    [0x4B] = 0xE035, [0x4C] = 0xE01C, [0x4E] = 0x4A, [0x52] = 0x52, [0x53] = 0x4F, [0x54] = 0x50,
    [0x55] = 0x51, [0x56] = 0x4B, [0x57] = 0x4C, [0x58] = 0x4D, [0x59] = 0x47, [0x5B] = 0x48, [0x5C] = 0x49,
    [0x5D] = 0x7D, [0x5E] = 0x73, /* JIS Yen and underscore */
    [0x60] = 0x3F, [0x61] = 0x40, [0x62] = 0x41, [0x63] = 0x3D, [0x64] = 0x42, [0x65] = 0x43, [0x67] = 0x57,
    [0x6D] = 0x44, [0x6F] = 0x58,
    [0x72] = 0xE052, /* Help (on older keyboards where Insert is): Insert */
    [0x73] = 0xE047, [0x74] = 0xE049, [0x75] = 0xE053, [0x76] = 0x3E, [0x77] = 0xE04F, [0x78] = 0x3C,
    [0x79] = 0xE051, [0x7A] = 0x3B, [0x7B] = 0xE04B, [0x7C] = 0xE04D, [0x7D] = 0xE050, [0x7E] = 0xE048,
};

static uint8_t g_keys_down[512]; /* by scancode (+256 for extended keys): what the game holds now */
static int g_buttons, g_ctrl_click;

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

/* Shift, Control and Option are passed to the game only once a key comes with them (or alone, on release), so
 * that the keys of a Cmd shortcut never reach the game (on Windows a stray key once skipped the intro).  A
 * modifier held when Command goes down belongs to the shortcut. */
enum { MOD_LSHIFT, MOD_RSHIFT, MOD_LCTRL, MOD_RCTRL, MOD_LALT, MOD_RALT, MOD_COUNT };
static const uint16_t g_mod_key[MOD_COUNT] = {0x38, 0x3C, 0x3B, 0x3E, 0x3A, 0x3D};
static const NSUInteger g_mod_bit[MOD_COUNT] = {0x02, 0x04, 0x01, 0x2000, 0x20, 0x40}; /* NX_DEVICE*KEYMASK */
static int g_mod_down[MOD_COUNT], g_mod_held[MOD_COUNT], g_mod_used[MOD_COUNT];
static int g_cmd_down;

static void mods_flush(void)
{
    int m;
    for (m = 0; m < MOD_COUNT; m++)
        if (g_mod_held[m] && !g_mod_used[m]) {
            g_mod_held[m] = 0;
            key_to_game(g_scan[g_mod_key[m]], 1);
        }
}

static void mods_used(void)
{
    int m;
    for (m = 0; m < MOD_COUNT; m++)
        if (g_mod_held[m]) g_mod_used[m] = 1;
}

static void release_all(void)
{
    int i;
    for (i = 0; i < MOD_COUNT; i++) g_mod_down[i] = g_mod_held[i] = g_mod_used[i] = 0;
    g_cmd_down = 0;
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
    g_ctrl_click = 0;
}

static void key_event(NSEvent *e, int down)
{
    uint32_t sc = e.keyCode < 128 ? g_scan[e.keyCode] : 0u;
    if (sc == 0) return;
    if (down) {
        if (e.modifierFlags & NSEventModifierFlagCommand) return; /* the app's (a shortcut, or nothing) */
        mods_flush();
        key_to_game(sc, 1); /* auto-repeat sends the make code again, as a PC keyboard does */
    } else {
        key_to_game(sc, 0);
    }
}

static void flags_event(NSEvent *e)
{
    NSUInteger f = e.modifierFlags;
    int m;
    int cmd = (f & NSEventModifierFlagCommand) != 0;
    if (cmd && !g_cmd_down) mods_used();
    g_cmd_down = cmd;
    if (e.keyCode == 0x39) { /* Caps Lock: macOS reports its state; the PC gets a press and a release */
        key_to_game(0x3A, 1);
        key_to_game(0x3A, 0);
        return;
    }
    for (m = 0; m < MOD_COUNT; m++) {
        int now = (f & g_mod_bit[m]) != 0;
        if (now == g_mod_down[m]) continue;
        g_mod_down[m] = now;
        if (now) {
            g_mod_held[m] = 1;
            g_mod_used[m] = cmd;
        } else {
            if (g_mod_held[m] && !g_mod_used[m]) key_to_game(g_scan[g_mod_key[m]], 1); /* alone: press now */
            g_mod_held[m] = 0;
            g_mod_used[m] = 0;
            key_to_game(g_scan[g_mod_key[m]], 0);
        }
    }
}

/* ==== the pointer ======================================================================================== */
/* In full screen, while the game has the focus, the pointer is captured: the system cursor is hidden and held
 * in place, the motion moves a pointer of our own that cannot leave the picture (the game draws it).  On a
 * second display the system pointer would otherwise wander there, where a click goes to another app. */
static int g_captured;
static NSPoint g_cap_pos;  /* the captured pointer, in view coordinates (top-left origin) */
static NSPoint g_last_pos; /* where the game's pointer was put last, in view coordinates */

static void send_pos(NSPoint p)
{
    NSRect r = [g_view pictureRect];
    double fx, fy;
    if (r.size.width <= 1 || r.size.height <= 1) return;
    g_last_pos = p;
    fx = (p.x - r.origin.x) * 65535.0 / (r.size.width - 1);
    fy = (p.y - r.origin.y) * 65535.0 / (r.size.height - 1);
    if (fx < 0) fx = 0;
    if (fx > 65535) fx = 65535;
    if (fy < 0) fy = 0;
    if (fy > 65535) fy = 65535;
    rt_mouse_move((int)(fx + 0.5), (int)(fy + 0.5));
}

/* the point of the view at `p` in the global display coordinates CGWarpMouseCursorPosition takes (top-left of
 * the main display) */
static CGPoint view_to_global(NSPoint p)
{
    NSPoint w = [g_view convertPoint:p toView:nil];
    NSPoint s = [g_window convertPointToScreen:w];
    CGFloat top = NSMaxY([NSScreen screens][0].frame);
    return CGPointMake(s.x, top - s.y);
}

static NSPoint clamp_to_picture(NSPoint p)
{
    NSRect r = [g_view pictureRect];
    if (p.x < r.origin.x) p.x = r.origin.x;
    if (p.x > NSMaxX(r) - 1) p.x = NSMaxX(r) - 1;
    if (p.y < r.origin.y) p.y = r.origin.y;
    if (p.y > NSMaxY(r) - 1) p.y = NSMaxY(r) - 1;
    return p;
}

/* whether the running program uses the mouse (rt_mouse_used): then it draws its own pointer and the system's is
 * hidden over the picture.  A program that does not (the launcher, the intro) gets the system pointer, which
 * hides itself after two seconds without motion, and no capture in full screen. */
static int g_game_mouse;

static void update_capture(void)
{
    int want = g_full && g_game_mouse && !g_fs_transition && !g_minimized && !g_modal && !g_in_menu &&
               NSApp.isActive && g_window.isKeyWindow;
    if (want && !g_captured) {
        /* from the system pointer when it is on the picture, else from where the game's pointer is */
        NSPoint m = [g_view convertPoint:[g_window mouseLocationOutsideOfEventStream] fromView:nil];
        g_cap_pos = clamp_to_picture(NSPointInRect(m, [g_view pictureRect]) ? m : g_last_pos);
        CGWarpMouseCursorPosition(view_to_global(g_cap_pos));
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
        g_captured = 1;
        send_pos(g_cap_pos);
    } else if (!want && g_captured) {
        CGWarpMouseCursorPosition(view_to_global(g_cap_pos)); /* the system pointer shows where the game's was */
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
        g_captured = 0;
    }
}

/* ==== the view =========================================================================================== */
@implementation SynView {
    CALayer *_picture;
    NSCursor *_blank;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    NSImage *img;
    self = [super initWithFrame:frame];
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    _picture = [CALayer layer];
    _picture.magnificationFilter = kCAFilterNearest; /* sharp pixels, as Windows' StretchDIBits draws them */
    _picture.minificationFilter = kCAFilterLinear;
    _picture.contentsGravity = kCAGravityResize;
    _picture.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    _picture.actions = @{@"contents" : [NSNull null], @"bounds" : [NSNull null], @"position" : [NSNull null]};
    [self.layer addSublayer:_picture];
    img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
    _blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (void)updateLayer
{
}

/* the largest 4:3 rectangle in the view, centred, on whole device pixels */
- (NSRect)pictureRect
{
    NSRect b = self.bounds, r;
    CGFloat w, h;
    if (b.size.width * 3 >= b.size.height * 4) {
        h = b.size.height;
        w = h * 4 / 3;
    } else {
        w = b.size.width;
        h = w * 3 / 4;
    }
    r = NSMakeRect((b.size.width - w) / 2, (b.size.height - h) / 2, w, h);
    return [self backingAlignedRect:r options:NSAlignAllEdgesNearest];
}

/* --verbose 2: how long showing a frame takes (with the CRT filter it is the most work the UI thread does) */
static uint64_t g_show_us, g_show_max, g_show_from;
static int g_show_n;

static void show_stats(uint64_t t0, int dw, int dh)
{
    uint64_t t = host_time_us(), d = t - t0;
    g_show_us += d;
    if (d > g_show_max) g_show_max = d;
    g_show_n++;
    if (g_show_from == 0) g_show_from = t;
    if (t - g_show_from >= 5000000u) {
        char line[160];
        snprintf(line, sizeof line, "video: %d frames in %.1f s, %.2f ms each (at most %.2f), %dx%d px, crt %d",
                 g_show_n, (double)(t - g_show_from) / 1e6, (double)g_show_us / g_show_n / 1000.0,
                 (double)g_show_max / 1000.0, dw, dh, g_crt_on);
        host_log(line);
        g_show_us = g_show_max = 0;
        g_show_n = 0;
        g_show_from = t;
    }
}

- (void)showFrame
{
    uint64_t t0 = host_time_us();
    [self checkGameMouse];
    NSRect r = [self pictureRect];
    NSRect px = [self convertRectToBacking:r];
    int fw, fh, dw = (int)(px.size.width + 0.5), dh = (int)(fabs(px.size.height) + 0.5);
    uint32_t *src;
    CGImageRef img = NULL;
    os_unfair_lock_lock(&g_frame_lock);
    fw = g_fw;
    fh = g_fh;
    src = fw > 0 ? (uint32_t *)malloc((size_t)fw * (size_t)fh * 4u) : NULL;
    if (src != NULL) memcpy(src, g_frame, (size_t)fw * (size_t)fh * 4u);
    os_unfair_lock_unlock(&g_frame_lock);
    if (src == NULL) return;
    if (g_crt_on && dw > 0 && dh > 0) {
        uint32_t *dst;
        if (g_crt == NULL) g_crt = crt_create();
        if (g_crt != NULL && crt_prepare(g_crt, src, fw, fh, dw, dh) &&
            (dst = (uint32_t *)malloc((size_t)dw * (size_t)dh * 4u)) != NULL) {
            /* the rows in chunks over all cores: about 35 ms a full-screen frame on one */
            size_t chunks = (size_t)(dh + CRT_CHUNK_ROWS - 1) / CRT_CHUNK_ROWS;
            CrtFilter *crt = g_crt;
            dispatch_apply(chunks, DISPATCH_APPLY_AUTO, ^(size_t i) {
              int y0 = (int)i * CRT_CHUNK_ROWS, y1 = y0 + CRT_CHUNK_ROWS < dh ? y0 + CRT_CHUNK_ROWS : dh;
              crt_render_rows(crt, dst + (size_t)y0 * (size_t)dw, dw, y0, y1);
            });
            img = make_image(dst, dw, dh);
            free(src);
            src = NULL;
        }
    }
    if (img == NULL) img = make_image(src, fw, fh);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.frame = r;
    _picture.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img);
    if (g_opt.verbose >= 2) show_stats(t0, dw, dh);
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self.window invalidateCursorRectsForView:self];
    [self showFrame];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self showFrame];
}

/* a game that uses the mouse draws its own pointer over the picture: the system's is hidden there */
- (void)resetCursorRects
{
    [self addCursorRect:[self pictureRect] cursor:g_game_mouse ? _blank : NSCursor.arrowCursor];
}

/* the running program started or stopped using the mouse (checked with every frame) */
- (void)checkGameMouse
{
    int used = rt_mouse_used() != 0;
    if (used == g_game_mouse) return;
    g_game_mouse = used;
    [self.window invalidateCursorRectsForView:self];
    update_capture();
    if (!used) [self performSelector:@selector(idleHide) withObject:nil afterDelay:2.0];
}

/* without the game's pointer the system's shows while it moves and hides after two seconds still */
- (void)idleHide
{
    NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream] fromView:nil];
    if (!g_game_mouse && NSApp.isActive && NSPointInRect(p, [self pictureRect])) [NSCursor setHiddenUntilMouseMoves:YES];
}

/* ---- keyboard ---- */
- (void)keyDown:(NSEvent *)e
{
    key_event(e, 1);
}

- (void)keyUp:(NSEvent *)e
{
    key_event(e, 0);
}

- (void)flagsChanged:(NSEvent *)e
{
    flags_event(e);
}

/* key presses with Control come here before keyDown; they are the game's (Cmd ones go on to the menus) */
- (BOOL)performKeyEquivalent:(NSEvent *)e
{
    if (e.type == NSEventTypeKeyDown && !(e.modifierFlags & NSEventModifierFlagCommand) &&
        self.window.firstResponder == self) {
        key_event(e, 1);
        return YES;
    }
    return NO;
}

/* ---- mouse ---- */
- (void)movedTo:(NSEvent *)e
{
    if (g_captured) {
        g_cap_pos.x += e.deltaX;
        g_cap_pos.y += e.deltaY;
        g_cap_pos = clamp_to_picture(g_cap_pos);
        send_pos(g_cap_pos);
        return;
    }
    if (!g_game_mouse) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(idleHide) object:nil];
        [self performSelector:@selector(idleHide) withObject:nil afterDelay:2.0];
    }
    {
        NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        /* as on Windows: the game follows the pointer over the view, and while a button is held */
        if (NSPointInRect(p, self.bounds) || g_buttons) send_pos(p);
    }
}

- (void)buttons:(int)b event:(NSEvent *)e
{
    [self movedTo:e];
    if (b != g_buttons) {
        g_buttons = b;
        rt_mouse_buttons(b);
    }
}

- (void)mouseMoved:(NSEvent *)e
{
    [self movedTo:e];
}

- (void)mouseDragged:(NSEvent *)e
{
    [self movedTo:e];
}

- (void)rightMouseDragged:(NSEvent *)e
{
    [self movedTo:e];
}

- (void)otherMouseDragged:(NSEvent *)e
{
    [self movedTo:e];
}

- (void)mouseDown:(NSEvent *)e
{
    /* Control+click is the right button; the Control belongs to the click, not the game */
    if (e.modifierFlags & NSEventModifierFlagControl) {
        int m;
        g_ctrl_click = 1;
        for (m = MOD_LCTRL; m <= MOD_RCTRL; m++)
            if (g_mod_held[m]) g_mod_used[m] = 1;
        mods_flush();
        [self buttons:g_buttons | 2 event:e];
        return;
    }
    mods_flush();
    [self buttons:g_buttons | 1 event:e];
}

- (void)mouseUp:(NSEvent *)e
{
    if (g_ctrl_click) {
        g_ctrl_click = 0;
        [self buttons:g_buttons & ~2 event:e];
        return;
    }
    [self buttons:g_buttons & ~1 event:e];
}

- (void)rightMouseDown:(NSEvent *)e
{
    mods_flush();
    [self buttons:g_buttons | 2 event:e];
}

- (void)rightMouseUp:(NSEvent *)e
{
    [self buttons:g_buttons & ~2 event:e];
}

- (void)otherMouseDown:(NSEvent *)e
{
    if (e.buttonNumber != 2) return;
    mods_flush();
    [self buttons:g_buttons | 4 event:e];
}

- (void)otherMouseUp:(NSEvent *)e
{
    if (e.buttonNumber != 2) return;
    [self buttons:g_buttons & ~4 event:e];
}

/* no context menu: a right click is the game's */
- (NSMenu *)menuForEvent:(NSEvent *)e
{
    (void)e;
    return nil;
}
@end

#ifdef SYN_DEVTEST
void macos_state(char *out, size_t n)
{
    snprintf(out, n, "gamemouse=%d full=%d captured=%d modal=%d minimized=%d menu=%d crt=%d buttons=%d active=%d key=%d",
             g_game_mouse, g_full,
             g_captured, g_modal, g_minimized, g_in_menu, g_crt_on, g_buttons, NSApp.isActive ? 1 : 0,
             g_window.isKeyWindow ? 1 : 0);
}
#endif

/* ==== the About box ====================================================================================== */
static NSString *const g_license =
    @"MIT License\n\n"
    @"Copyright (c) 2026 Onur BUYUKCAGLAR\n\n"
    @"Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    @"software and associated documentation files (the \"Software\"), to deal in the Software "
    @"without restriction, including without limitation the rights to use, copy, modify, merge, "
    @"publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    @"to whom the Software is furnished to do so, subject to the following conditions:\n\n"
    @"The above copyright notice and this permission notice shall be included in all copies or "
    @"substantial portions of the Software.\n\n"
    @"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    @"INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    @"PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    @"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    @"OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    @"DEALINGS IN THE SOFTWARE.\n\n"
    @"The licence covers this port's own code (" APP_CAPTION @"). Syndicate, Syndicate: American "
    @"Revolt and Syndicate Plus, their programs, sound drivers and data belong to their owners; they are "
    @"not part of this software, which needs a copy of the original game to run.";

/* the About panel ends its modal run however it is closed: OK, Return, Esc or the close button */
@interface AboutPanel : NSPanel
@end
@implementation AboutPanel
- (void)cancelOperation:(id)sender
{
    (void)sender;
    [NSApp stopModal];
}
- (void)close
{
    [NSApp stopModal];
    [super close];
}
@end

static NSTextField *label(NSString *s, NSRect r)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.frame = r;
    return t;
}

/* the same content and layout as the Windows About box: the icon, the author, the link, the licence and OK */
static void about(void)
{
    NSRect fr = NSMakeRect(0, 0, 460, 360);
    NSPanel *w = [[AboutPanel alloc] initWithContentRect:fr
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    NSView *c = w.contentView;
    NSImageView *icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    NSTextView *link = [[NSTextView alloc] initWithFrame:NSMakeRect(78, 274, 360, 18)];
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 56, 420, 176)];
    NSTextView *lic;
    NSButton *ok = [NSButton buttonWithTitle:@"OK" target:NSApp action:@selector(stopModal)];
    NSDictionary *la = @{
        NSLinkAttributeName : [NSURL URLWithString:ABOUT_URL],
        NSFontAttributeName : [NSFont systemFontOfSize:NSFont.systemFontSize]
    };

    w.title = @"About " APP_CAPTION @" v" @SYN_VERSION;
    icon.frame = NSMakeRect(20, 276, 48, 48);
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [c addSubview:icon];
    [c addSubview:label(@"64bit version by Onur BUYUKCAGLAR", NSMakeRect(80, 312, 360, 18))];
    [c addSubview:label(@"with Anthropic Claude AI assistance", NSMakeRect(80, 294, 360, 18))];
    /* the link: a text view opens it in the browser and shows the pointing hand over it */
    link.editable = NO;
    link.selectable = YES;
    link.drawsBackground = NO;
    link.textContainerInset = NSZeroSize;
    link.textContainer.lineFragmentPadding = 2;
    [link.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:ABOUT_URL attributes:la]];
    [c addSubview:link];
    [c addSubview:label(@"Licenses:", NSMakeRect(20, 238, 420, 18))];
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    lic = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, sv.contentSize.height)];
    lic.editable = NO;
    lic.selectable = YES;
    lic.verticallyResizable = YES;
    lic.autoresizingMask = NSViewWidthSizable;
    lic.textContainer.widthTracksTextView = YES;
    lic.textContainerInset = NSMakeSize(4, 4);
    lic.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    lic.string = g_license;
    lic.textColor = NSColor.textColor;
    lic.backgroundColor = NSColor.textBackgroundColor;
    sv.documentView = lic;
    [c addSubview:sv];
    ok.frame = NSMakeRect(350, 14, 90, 30);
    ok.keyEquivalent = @"\r";
    [c addSubview:ok];

    g_modal++;
    update_pause();
    [w center];
    [w makeFirstResponder:ok];
#ifdef SYN_DEVTEST
    devtest_about(w);
#endif
    [NSApp runModalForWindow:w];
    [w orderOut:nil];
    g_modal--;
    update_pause();
    [g_window makeKeyAndOrderFront:nil];
}

/* ==== the app ============================================================================================ */
@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation>
@property(nonatomic) BOOL startFull;
@end

@implementation AppDelegate

- (void)about:(id)sender
{
    (void)sender;
    about();
}

- (void)toggleFull:(id)sender
{
    (void)sender;
    [g_window toggleFullScreen:nil];
}

- (void)toggleCrt:(id)sender
{
    (void)sender;
    g_crt_on = !g_crt_on;
    ini_set("video", "crt", g_crt_on ? "1" : "0");
    [g_view showFrame];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(toggleFull:)) item.state = g_full ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleCrt:)) item.state = g_crt_on ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

/* ---- the window's state ---- */
- (void)windowWillEnterFullScreen:(NSNotification *)n
{
    g_fs_transition = 1;
    update_capture();
}

- (void)windowDidEnterFullScreen:(NSNotification *)n
{
    g_full = 1;
    g_fs_transition = 0;
    update_capture();
}

- (void)windowWillExitFullScreen:(NSNotification *)n
{
    g_fs_transition = 1;
    update_capture();
}

- (void)windowDidExitFullScreen:(NSNotification *)n
{
    g_full = 0;
    g_fs_transition = 0;
    update_capture();
}

- (void)windowDidFailToEnterFullScreen:(NSWindow *)w
{
    g_fs_transition = 0;
    update_capture();
}

- (void)windowDidFailToExitFullScreen:(NSWindow *)w
{
    g_fs_transition = 0;
    update_capture();
}

- (void)windowDidMiniaturize:(NSNotification *)n
{
    g_minimized = 1;
    update_pause();
}

- (void)windowDidDeminiaturize:(NSNotification *)n
{
    g_minimized = 0;
    update_pause();
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
    update_capture();
}

- (void)windowDidResignKey:(NSNotification *)n
{
    release_all();
    update_capture();
}

- (void)applicationDidBecomeActive:(NSNotification *)n
{
    update_capture();
}

- (void)applicationDidResignActive:(NSNotification *)n
{
    release_all();
    update_capture();
}

- (void)menuBegan:(NSNotification *)n
{
    g_in_menu = 1;
    release_all();
    update_capture();
}

- (void)menuEnded:(NSNotification *)n
{
    g_in_menu = 0;
    update_capture();
}

- (BOOL)windowShouldClose:(NSWindow *)w
{
    [NSApp terminate:nil];
    return NO;
}

/* ---- quitting: the game thread gets three seconds to end ---- */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    atomic_store(&g_quit, 1);
    g_modal = 0;
    g_minimized = 0;
    rt_set_paused(0);
    if (g_captured) {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
        g_captured = 0;
    }
    if (!g_game_started || atomic_load(&g_game_done)) return NSTerminateNow;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      int i;
      for (i = 0; i < 300 && !atomic_load(&g_game_done); i++) usleep(10000);
      dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp replyToApplicationShouldTerminate:YES];
      });
    });
    return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    audio_stop();
    if (g_logfile != NULL) fflush(g_logfile);
}
@end

/* ==== the menus ========================================================================================== */
static NSMenuItem *item(NSString *title, SEL action, NSString *key, NSEventModifierFlags mods, id target)
{
    NSMenuItem *i = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    i.keyEquivalentModifierMask = mods;
    i.target = target;
    return i;
}

static void build_menus(AppDelegate *d)
{
    NSMenu *bar = [[NSMenu alloc] init], *app = [[NSMenu alloc] initWithTitle:APP_NAME];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"], *win = [[NSMenu alloc] initWithTitle:@"Window"];
    NSMenuItem *m;
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;

    [app addItem:item(@"About " APP_NAME, @selector(about:), @"", 0, d)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item(@"Hide " APP_NAME, @selector(hide:), @"h", cmd, NSApp)];
    [app addItem:item(@"Hide Others", @selector(hideOtherApplications:), @"h", cmd | NSEventModifierFlagOption, NSApp)];
    [app addItem:item(@"Show All", @selector(unhideAllApplications:), @"", 0, NSApp)];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:item(@"Quit " APP_NAME, @selector(terminate:), @"q", cmd, NSApp)];
    m = [[NSMenuItem alloc] init];
    m.submenu = app;
    [bar addItem:m];

    [view addItem:item(@"Full Screen", @selector(toggleFull:), @"f", cmd | NSEventModifierFlagControl, d)];
    [view addItem:item(@"CRT Scanlines", @selector(toggleCrt:), @"s", cmd | NSEventModifierFlagShift, d)];
    m = [[NSMenuItem alloc] init];
    m.submenu = view;
    [bar addItem:m];

    [win addItem:item(@"Minimize", @selector(performMiniaturize:), @"m", cmd, nil)];
    [win addItem:item(@"Zoom", @selector(performZoom:), @"", 0, nil)];
    m = [[NSMenuItem alloc] init];
    m.submenu = win;
    [bar addItem:m];
    NSApp.windowsMenu = win;
    NSApp.mainMenu = bar;
}

/* ==== where the program reads and writes ================================================================= */
/* the folder that holds Syndicate64.app (or the program itself, when it runs outside a bundle) */
static NSString *app_dir(void)
{
    NSString *b = NSBundle.mainBundle.bundlePath;
    if ([b.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame) return b.stringByDeletingLastPathComponent;
    return NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent;
}

/* the folder exists (made if need be) and a file can be created in it */
static int writable_dir(NSString *dir)
{
    char probe[PATH_MAX];
    int fd;
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    snprintf(probe, sizeof probe, "%s/.syndicate64-%d.tmp", dir.fileSystemRepresentation, (int)getpid());
    fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return 0;
    close(fd);
    unlink(probe);
    return 1;
}

static NSString *local_dir(void)
{
    NSString *base = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString *d = [base ?: NSHomeDirectory() stringByAppendingPathComponent:APP_NAME];
    [NSFileManager.defaultManager createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d;
}

/* the saved games and every other file the game writes: Save next to the app, else Application Support */
static NSString *save_dir(void)
{
    NSString *d = [app_dir() stringByAppendingPathComponent:@"Save"];
    return writable_dir(d) ? d : local_dir();
}

/* the settings: Syndicate64.ini next to the app, else in Application Support */
static NSString *settings_file(void)
{
    NSString *d = app_dir();
    if (!writable_dir(d)) d = local_dir();
    return [d stringByAppendingPathComponent:APP_NAME @".ini"];
}

/* ==== finding the game =================================================================================== */
static int has_game(NSString *dir)
{
    char out[300];
    return dir.length > 0 && host_find_name(dir.fileSystemRepresentation, "SYND.EXE", out, sizeof out);
}

static NSString *pick_folder(void)
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    NSString *r = nil;
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    p.message = @"Choose the Syndicate Plus folder (the one with SYND.EXE)";
    p.prompt = @"Choose";
    if ([p runModal] == NSModalResponseOK) r = p.URL.path;
    return r;
}

static NSString *find_game(void)
{
    NSString *here = app_dir(), *dir;
    char saved[PATH_MAX];
    int up;
    /* next to the app */
    if (has_game(here)) return here;
    dir = [here stringByAppendingPathComponent:@"SYNDPLUS"];
    if (has_game(dir)) return dir;
    /* a source checkout: _references/dos in the app's folder or above it */
    dir = here;
    for (up = 0; up < 5; up++) {
        NSString *probe = [dir stringByAppendingPathComponent:@"_references/dos"];
        if (has_game(probe)) return probe;
        if ([dir isEqualToString:@"/"]) break;
        dir = dir.stringByDeletingLastPathComponent;
    }
    /* the folder chosen before */
    if (ini_get("game", "folder", saved, sizeof saved) && has_game(@(saved))) return @(saved);
    /* ask */
    for (;;) {
        NSAlert *a;
        dir = pick_folder();
        if (dir == nil) return nil;
        if (has_game(dir)) break;
        a = [[NSAlert alloc] init];
        a.messageText = APP_CAPTION;
        a.informativeText = @"That folder does not hold the original Syndicate Plus (SYND.EXE was not found in it). "
                            @"Choose the folder the game was installed to.";
        a.alertStyle = NSAlertStyleWarning;
        [a addButtonWithTitle:@"OK"];
        [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    }
    ini_set("game", "folder", dir.fileSystemRepresentation);
    return dir;
}

/* the game files built into the app (Contents/Resources/syndgame.pak of a private build), mapped into memory
 * once; NULL when the app has none */
const uint8_t *host_game_pack(size_t *size)
{
    static const uint8_t *pack;
    static size_t pack_size;
    static int tried;
    if (!tried) {
        NSString *p = [NSBundle.mainBundle pathForResource:@"syndgame" ofType:@"pak"];
        int fd = p != nil ? open(p.fileSystemRepresentation, O_RDONLY) : -1;
        struct stat st;
        tried = 1;
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
    *size = pack_size;
    return pack;
}

/* ==== start ============================================================================================== */
static NSString *g_arg_game_dir, *g_arg_user_dir;

static void start_game_thread(void)
{
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20); /* as on Windows: the core's guest calls nest deeply */
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
    if (pthread_create(&g_game_thread, &at, game_thread, NULL) == 0) g_game_started = 1;
    pthread_attr_destroy(&at);
}

/* applicationDidFinishLaunching: the settings, the game files, the window, the sound and the game thread */
@implementation AppDelegate (Start)
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSString *game = g_arg_game_dir, *user = g_arg_user_dir;
    NSRect vis = NSScreen.mainScreen.visibleFrame;
    char v[16];
    size_t pack_size;
    int scale;
    (void)n;
    snprintf(g_ini, sizeof g_ini, "%s", settings_file().fileSystemRepresentation);
    [NSApp activateIgnoringOtherApps:YES];
    if (game != nil || host_game_pack(&pack_size) == NULL) { /* else: the built-in files */
        if (game == nil && (game = find_game()) == nil) {
            [NSApp terminate:nil];
            return;
        }
        if (!has_game(game)) {
            alert(@"SYND.EXE was not found in the game folder.", NSAlertStyleCritical);
            [NSApp terminate:nil];
            return;
        }
    }
    if (user == nil) user = save_dir();
    g_crt_on = ini_get("video", "crt", v, sizeof v) && atoi(v) != 0;
    [NSFileManager.defaultManager createDirectoryAtPath:user withIntermediateDirectories:YES attributes:nil error:nil];
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game != nil ? game.fileSystemRepresentation : "");
    snprintf(g_user_dir, sizeof g_user_dir, "%s", user.fileSystemRepresentation);
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
    }

    /* the largest whole multiple of 320x240 that fits 85% of the screen (in points) */
    for (scale = 6; scale > 1; scale--)
        if (320 * scale <= vis.size.width * 85 / 100 && 240 * scale <= vis.size.height * 85 / 100) break;
    g_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 320 * scale, 240 * scale)
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    g_window.title = APP_CAPTION;
    g_window.contentMinSize = NSMakeSize(320, 240);
    g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    g_window.acceptsMouseMovedEvents = YES; /* the moves go to the view (first responder), once each */
    g_window.delegate = self;
    g_window.backgroundColor = NSColor.blackColor;
    g_view = [[SynView alloc] initWithFrame:NSMakeRect(0, 0, 320 * scale, 240 * scale)];
    g_window.contentView = g_view;
    [g_window makeFirstResponder:g_view];
    [g_window center];
    [g_window makeKeyAndOrderFront:nil];
    [g_view performSelector:@selector(idleHide) withObject:nil afterDelay:2.0]; /* the launcher has no pointer */
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(menuBegan:)
                                               name:NSMenuDidBeginTrackingNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(menuEnded:)
                                               name:NSMenuDidEndTrackingNotification object:nil];
    if (self.startFull) [g_window toggleFullScreen:nil];

    audio_start();
    start_game_thread();
#ifdef SYN_DEVTEST
    devtest_start(g_window, g_view);
#endif
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        static char start[512];
        int i;
        AppDelegate *d;
        memset(&g_opt, 0, sizeof g_opt);
        NSApplication *app = [NSApplication sharedApplication];
        d = [[AppDelegate alloc] init];
        for (i = 1; i < argc; i++) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
            if (!strcmp(a, "--game-dir")) {
                g_arg_game_dir = @(v);
                i++;
            } else if (!strcmp(a, "--user-dir")) {
                g_arg_user_dir = @(v);
                i++;
            } else if (!strcmp(a, "--start")) {
                char *c;
                snprintf(start, sizeof start, "%s", v);
                for (c = start; *c && *c != ' '; c++) /* the program's path; its arguments stay as given */
                    if (*c == '/') *c = '\\';
                g_opt.start = start;
                i++;
            } else if (!strcmp(a, "--log")) {
                g_logfile = fopen(v, "w");
                i++;
            } else if (!strcmp(a, "--verbose")) {
                g_opt.verbose = atoi(v);
                i++;
            } else if (!strcmp(a, "--fullscreen")) {
                d.startFull = YES;
            }
            /* anything else (the -psn_ argument old Finders add, Xcode's -NSDocumentRevisionsDebugMode) is not ours */
        }
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        /* our View menu has its own "Full Screen" item: AppKit is not to add "Enter Full Screen" too */
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"NSFullScreenMenuItemEverywhere"];
        NSWindow.allowsAutomaticWindowTabbing = NO;
        app.delegate = d;
        build_menus(d);
        [app run];
    }
    return 0;
}
