/* win32_main.c - the Windows front end of Syndicate Plus 64bit: one window that shows the game, feeds it the
 * keyboard and the mouse and plays its sound.  The portable core (game_main) runs on its own thread; this file
 * is the UI thread.
 *
 * The window follows the system's colour preference (dark or light title bar and menus, changing at once when
 * the preference changes); its system menu has "Full screen" (also Alt+Enter), "CRT scanlines" (also Alt+S;
 * off at first, remembered in Syndicate64.ini) and the About box, which has the same layout and behaviour as Civ64's
 * (platform/win32/host_win32.c of that project).
 *
 *   Syndicate64.exe [--game-dir DIR] [--user-dir DIR] [--start "SYNDICAT\MAIN.EXE /args"] [--log FILE]
 *                   [--verbose N] [--fullscreen]
 *
 * The original game files are built into a private build (SYN_EMBED_GAME, the resource GAMEPAK) and used
 * unless --game-dir names a folder.  Without them the folder of the original game is looked for next to the
 * program, in _references\dos above it (a source checkout), and in the folder chosen last time; otherwise the
 * player picks it. */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../game/game.h"
#include "../../rt/host.h"
#include "../../video/crt.h"

#define APP_CAPTIONW L"Syndicate Plus 64bit"
#define APP_VERSIONW L"1.0.0"
#define ABOUT_URL L"https://github.com/Buyukcaglar/synd"
#define SC_APP_ABOUT 0x1080u
#define SC_APP_FULLSCREEN 0x1090u
#define SC_APP_CRT 0x10A0u
#define WM_APP_GAME_DONE (WM_APP + 1)
#define WM_APP_FATAL (WM_APP + 2)

extern FILE *g_logfile;

static HWND g_hwnd;
static HANDLE g_game_thread;
static volatile LONG g_quit;
static volatile LONG g_game_done;
static int g_full;
static WINDOWPLACEMENT g_place = {sizeof(WINDOWPLACEMENT)};
static GameOptions g_opt;
static char g_game_dir_u8[1024], g_user_dir_u8[1024];
static int g_minimized, g_modal;
static int g_in_menu, g_clipped;
static wchar_t g_ini[MAX_PATH * 2]; /* Syndicate64.ini: next to the program, else in %LOCALAPPDATA%/Syndicate64 */

/* ==== the picture ======================================================================================= */
#define FRAME_MAX_W 640
#define FRAME_MAX_H 480
static SRWLOCK g_frame_lock = SRWLOCK_INIT;
static uint32_t g_frame[FRAME_MAX_W * FRAME_MAX_H];
static int g_fw, g_fh;

void host_present(const uint8_t *pixels, int w, int h, const uint32_t *palette)
{
    int i;
    if (w <= 0 || h <= 0 || w > FRAME_MAX_W || h > FRAME_MAX_H) return;
    AcquireSRWLockExclusive(&g_frame_lock);
    for (i = 0; i < w * h; i++) g_frame[i] = palette[pixels[i]];
    g_fw = w;
    g_fh = h;
    ReleaseSRWLockExclusive(&g_frame_lock);
    if (g_hwnd != NULL) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* the largest 4:3 rectangle in the client area, centred */
static RECT picture_rect(HWND h)
{
    RECT c, r;
    int cw, ch, w, hh;
    GetClientRect(h, &c);
    cw = c.right - c.left;
    ch = c.bottom - c.top;
    if (cw * 3 >= ch * 4) {
        hh = ch;
        w = ch * 4 / 3;
    } else {
        w = cw;
        hh = cw * 3 / 4;
    }
    r.left = (cw - w) / 2;
    r.top = (ch - hh) / 2;
    r.right = r.left + w;
    r.bottom = r.top + hh;
    return r;
}

/* ---- the CRT scanline filter (src/video/crt.c), off unless chosen in the system menu ---- */
static int g_crt_on;
static CrtFilter *g_crt;
static uint32_t *g_crt_buf;
static size_t g_crt_cap;
static int g_crt_w, g_crt_h;
static volatile LONG g_crt_next;
static int g_crt_chunks;
static PTP_WORK g_crt_work;
#define CRT_CHUNK_ROWS 32

static void CALLBACK crt_work(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work)
{
    LONG i;
    (void)inst;
    (void)ctx;
    (void)work;
    while ((i = InterlockedIncrement(&g_crt_next) - 1) < g_crt_chunks) {
        int y0 = i * CRT_CHUNK_ROWS, y1 = y0 + CRT_CHUNK_ROWS < g_crt_h ? y0 + CRT_CHUNK_ROWS : g_crt_h;
        crt_render_rows(g_crt, g_crt_buf + (size_t)y0 * g_crt_w, g_crt_w, y0, y1);
    }
}

/* called with g_frame_lock held shared; on success it releases the lock, draws the filtered picture into `r`
 * and returns 1; otherwise returns 0 with the lock still held */
static int crt_paint(HDC dc, RECT r)
{
    int dw = r.right - r.left, dh = r.bottom - r.top, n, i;
    size_t need = (size_t)dw * (size_t)dh;
    BITMAPINFO bi;
    SYSTEM_INFO si;
    if (dw <= 0 || dh <= 0) return 0;
    if (g_crt == NULL && (g_crt = crt_create()) == NULL) return 0;
    if (need > g_crt_cap) {
        uint32_t *b = (uint32_t *)realloc(g_crt_buf, need * sizeof *b);
        if (b == NULL) return 0;
        g_crt_buf = b;
        g_crt_cap = need;
    }
    if (!crt_prepare(g_crt, g_frame, g_fw, g_fh, dw, dh)) return 0;
    ReleaseSRWLockShared(&g_frame_lock);
    /* the rows in chunks over the thread pool */
    if (g_crt_work == NULL) g_crt_work = CreateThreadpoolWork(crt_work, NULL, NULL);
    g_crt_w = dw;
    g_crt_h = dh;
    g_crt_chunks = (dh + CRT_CHUNK_ROWS - 1) / CRT_CHUNK_ROWS;
    g_crt_next = 0;
    GetSystemInfo(&si);
    n = (int)si.dwNumberOfProcessors;
    if (n > 16) n = 16;
    if (g_crt_work != NULL) {
        for (i = 1; i < n; i++) SubmitThreadpoolWork(g_crt_work);
    }
    crt_work(NULL, NULL, NULL); /* this thread helps too */
    if (g_crt_work != NULL) WaitForThreadpoolWorkCallbacks(g_crt_work, FALSE);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, r.left, r.top, (DWORD)dw, (DWORD)dh, 0, 0, 0, (UINT)dh, g_crt_buf, &bi, DIB_RGB_COLORS);
    return 1;
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT c, r = picture_rect(h);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bar;
    GetClientRect(h, &c);
    /* the bars around the picture */
    bar = c;
    bar.bottom = r.top;
    FillRect(dc, &bar, black);
    bar = c;
    bar.top = r.bottom;
    FillRect(dc, &bar, black);
    bar = c;
    bar.top = r.top;
    bar.bottom = r.bottom;
    bar.right = r.left;
    FillRect(dc, &bar, black);
    bar.left = r.right;
    bar.right = c.right;
    FillRect(dc, &bar, black);
    AcquireSRWLockShared(&g_frame_lock);
    if (g_fw > 0 && g_crt_on && crt_paint(dc, r)) {
        /* drawn (crt_paint released the lock) */
        EndPaint(h, &ps);
        return;
    }
    if (g_fw > 0) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g_fw;
        bi.bmiHeader.biHeight = -g_fh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, g_fw, g_fh, g_frame, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        FillRect(dc, &r, black);
    }
    ReleaseSRWLockShared(&g_frame_lock);
    EndPaint(h, &ps);
}

/* ==== sound (waveOut, fed from a ring the game thread fills) ============================================ */
#define AUD_RATE 44100
#define AUD_RING 32768u   /* frames */
#define AUD_BLOCK 1024u   /* frames per waveOut buffer */
#define AUD_NBUF 4
static SRWLOCK g_aud_lock = SRWLOCK_INIT;
static int16_t g_ring[AUD_RING * 2];
static uint32_t g_ring_r, g_ring_w; /* frame counters */
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[AUD_NBUF];
static int16_t g_buf[AUD_NBUF][AUD_BLOCK * 2];
static HANDLE g_aud_event, g_aud_thread;
static volatile LONG g_aud_stop;

int host_audio_rate(void)
{
    return AUD_RATE;
}

void host_audio_push(const int16_t *frames, size_t count)
{
    size_t i;
    AcquireSRWLockExclusive(&g_aud_lock);
    for (i = 0; i < count; i++) {
        uint32_t k = g_ring_w % AUD_RING;
        g_ring[k * 2] = frames[i * 2];
        g_ring[k * 2 + 1] = frames[i * 2 + 1];
        g_ring_w++;
    }
    if (g_ring_w - g_ring_r > AUD_RING) g_ring_r = g_ring_w - AUD_RING;
    ReleaseSRWLockExclusive(&g_aud_lock);
}

static void audio_fill(int16_t *out)
{
    uint32_t i, have;
    AcquireSRWLockExclusive(&g_aud_lock);
    have = g_ring_w - g_ring_r;
    /* the game's clock and the sound card's drift apart a little: keep the delay near two blocks */
    if (have > AUD_BLOCK * 6u) {
        g_ring_r = g_ring_w - AUD_BLOCK * 2u;
        have = AUD_BLOCK * 2u;
    }
    for (i = 0; i < AUD_BLOCK; i++) {
        if (i < have) {
            uint32_t k = (g_ring_r + i) % AUD_RING;
            out[i * 2] = g_ring[k * 2];
            out[i * 2 + 1] = g_ring[k * 2 + 1];
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    g_ring_r += have < AUD_BLOCK ? have : AUD_BLOCK;
    ReleaseSRWLockExclusive(&g_aud_lock);
}

static DWORD WINAPI audio_thread(LPVOID unused)
{
    int i;
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (i = 0; i < AUD_NBUF; i++) {
        memset(g_buf[i], 0, sizeof g_buf[i]);
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof g_buf[i];
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
    }
    while (!g_aud_stop) {
        WaitForSingleObject(g_aud_event, 50);
        for (i = 0; i < AUD_NBUF; i++) {
            if (g_hdr[i].dwFlags & WHDR_DONE) {
                audio_fill(g_buf[i]);
                waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
            }
        }
    }
    return 0;
}

static void audio_start(void)
{
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = AUD_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = AUD_RATE * 4;
    g_aud_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, (DWORD_PTR)g_aud_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_wo = NULL;
        host_log("sound: no output device");
        return;
    }
    g_aud_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
}

static void audio_stop(void)
{
    int i;
    if (g_wo == NULL) return;
    InterlockedExchange(&g_aud_stop, 1);
    SetEvent(g_aud_event);
    WaitForSingleObject(g_aud_thread, 1000);
    waveOutReset(g_wo);
    for (i = 0; i < AUD_NBUF; i++) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
    waveOutClose(g_wo);
    g_wo = NULL;
}

/* ==== the game thread ==================================================================================== */
int host_should_quit(void)
{
    return g_quit != 0;
}

static wchar_t g_fatal_text[1100];

void host_fatal(const char *text)
{
    MultiByteToWideChar(CP_UTF8, 0, text, -1, g_fatal_text, (int)(sizeof g_fatal_text / sizeof g_fatal_text[0]));
    if (g_hwnd != NULL && GetCurrentThreadId() != GetWindowThreadProcessId(g_hwnd, NULL)) {
        /* the game thread: let the UI thread show it, then end here */
        SendMessageW(g_hwnd, WM_APP_FATAL, 0, 0);
        ExitThread(3);
    }
    ClipCursor(NULL);
    MessageBoxW(g_hwnd, g_fatal_text, APP_CAPTIONW, MB_OK | MB_ICONERROR);
    ExitProcess(3);
}

static DWORD WINAPI game_thread(LPVOID unused)
{
    (void)unused;
    game_main(&g_opt);
    InterlockedExchange(&g_game_done, 1);
    if (g_hwnd != NULL) PostMessageW(g_hwnd, WM_APP_GAME_DONE, 0, 0);
    return 0;
}

/* in full screen the pointer stays on the picture while the game has the focus (on several monitors it would
 * otherwise wander onto another one, where a click goes to another program); free in menus and dialogs */
static void update_clip(void)
{
    int want = g_full && !g_minimized && !g_modal && !g_in_menu && GetForegroundWindow() == g_hwnd;
    if (want) {
        RECT r = picture_rect(g_hwnd);
        MapWindowPoints(g_hwnd, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
        g_clipped = 1;
    } else if (g_clipped) {
        ClipCursor(NULL);
        g_clipped = 0;
    }
}

static void update_pause(void)
{
    rt_set_paused(g_minimized || g_modal);
    update_clip();
}

/* ==== the colour preference (as Civ64: DWM's title bar attribute, uxtheme's ordinals for the menus) ===== */
#define THEME_DARK_BG RGB(32, 32, 32)
#define THEME_DARK_FIELD RGB(43, 43, 43)
#define THEME_DARK_TEXT RGB(255, 255, 255)
#define THEME_DARK_LINK RGB(96, 205, 255)

typedef int(WINAPI *ThemeModeFn)(int);
typedef BOOL(WINAPI *ThemeWindowFn)(HWND, BOOL);
typedef void(WINAPI *ThemeVoidFn)(void);
static ThemeModeFn g_ux_mode;
static ThemeWindowFn g_ux_window;
static ThemeVoidFn g_ux_refresh;
static ThemeVoidFn g_ux_flush;
static int g_ux_tried;
static DWORD g_os_build;
static int g_dark;

static DWORD os_build(void)
{
    typedef LONG(WINAPI * VersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    VersionFn fn = nt != NULL ? (VersionFn)GetProcAddress(nt, "RtlGetVersion") : NULL;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (fn == NULL || fn(&v) != 0 || v.dwMajorVersion < 10u) return 0u;
    return v.dwBuildNumber;
}

static void theme_load(void)
{
    HMODULE ux;
    if (g_ux_tried) return;
    g_ux_tried = 1;
    g_os_build = os_build();
    if (g_os_build < 18362u || (ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) == NULL)
        return;
    g_ux_mode = (ThemeModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_ux_window = (ThemeWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_ux_refresh = (ThemeVoidFn)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_ux_flush = (ThemeVoidFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (g_ux_mode == NULL || g_ux_window == NULL || g_ux_refresh == NULL || g_ux_flush == NULL) {
        g_ux_mode = NULL;
        g_ux_window = NULL;
        g_ux_refresh = NULL;
        g_ux_flush = NULL;
    }
}

/* 1 when the system's app mode is dark (light when it records none) */
static int theme_wanted(void)
{
    DWORD v = 1u, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS)
        return 0;
    return v == 0u;
}

static int theme_setting(LPARAM l)
{
    return l != 0 && CompareStringOrdinal((LPCWCH)l, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
}

static void theme_window(HWND h, int dark)
{
    BOOL on = dark ? TRUE : FALSE;
    theme_load();
    if (g_ux_window != NULL) (void)g_ux_window(h, on);
    if (g_os_build >= 17763u) (void)DwmSetWindowAttribute(h, g_os_build >= 18985u ? 20u : 19u, &on, sizeof on);
}

static void theme_apply(int redraw)
{
    theme_load();
    g_dark = theme_wanted();
    if (g_ux_mode != NULL) {
        (void)g_ux_mode(g_dark ? 2 : 3); /* ForceDark / ForceLight: the menus follow the title bar */
        g_ux_refresh();
        g_ux_flush();
    }
    if (g_hwnd != NULL) {
        theme_window(g_hwnd, g_dark);
        if (redraw)
            SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

/* ==== the About box (Civ64's layout: icon, credits, link, licences, an owner-drawn OK) ================= */
#define IDC_ABOUT_ICON 201
#define IDC_ABOUT_LINK 202
#define IDC_ABOUT_LICENSE 203

static const wchar_t g_license[] =
    L"MIT License\r\n\r\n"
    L"Copyright (c) 2026 Onur BUYUKCAGLAR\r\n\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    L"software and associated documentation files (the \"Software\"), to deal in the Software "
    L"without restriction, including without limitation the rights to use, copy, modify, merge, "
    L"publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    L"to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\n"
    L"The above copyright notice and this permission notice shall be included in all copies or "
    L"substantial portions of the Software.\r\n\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    L"INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    L"PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    L"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    L"OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    L"DEALINGS IN THE SOFTWARE.\r\n\r\n"
    L"The licence covers this port's own code (" APP_CAPTIONW L"). Syndicate, Syndicate: American "
    L"Revolt and Syndicate Plus, their programs, sound drivers and data belong to their owners; they are "
    L"not part of this software, which needs a copy of the original game to run.";

typedef struct Tmpl {
    WORD buf[512];
    size_t n;
} Tmpl;

static void t_word(Tmpl *t, WORD w)
{
    if (t->n < 512u) t->buf[t->n++] = w;
}

static void t_str(Tmpl *t, const wchar_t *s)
{
    do {
        t_word(t, (WORD)*s);
    } while (*s++ != L'\0');
}

static void t_item(Tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls,
                   const wchar_t *text)
{
    if (t->n & 1u) t_word(t, 0);
    t_word(t, LOWORD(style));
    t_word(t, HIWORD(style));
    t_word(t, 0);
    t_word(t, 0);
    t_word(t, (WORD)x);
    t_word(t, (WORD)y);
    t_word(t, (WORD)cx);
    t_word(t, (WORD)cy);
    t_word(t, id);
    t_word(t, 0xFFFF);
    t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);
}

static int g_about_dark;
static HBRUSH g_about_bg, g_about_field;

static void about_theme(HWND dlg)
{
    HWND c = GetDlgItem(dlg, IDC_ABOUT_LICENSE);
    g_about_dark = theme_wanted();
    theme_window(dlg, g_about_dark);
    if (c != NULL) {
        if (g_ux_window != NULL) (void)g_ux_window(c, g_about_dark ? TRUE : FALSE);
        SetWindowTheme(c, g_about_dark ? L"DarkMode_Explorer" : NULL, NULL);
        SendMessageW(c, WM_THEMECHANGED, 0u, 0);
    }
    if ((c = GetDlgItem(dlg, IDOK)) != NULL) InvalidateRect(c, NULL, TRUE);
}

/* the OK button: no Windows theme draws a dark push button, so it is owner-drawn (as Civ64's) */
static void about_button(const DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem;
    wchar_t text[32];
    int pressed = (di->itemState & ODS_SELECTED) != 0u;
    int focus = (di->itemState & ODS_FOCUS) != 0u;
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0u, 0);
    HGDIOBJ old = font != NULL ? SelectObject(di->hDC, font) : NULL;
    text[0] = L'\0';
    GetWindowTextW(di->hwndItem, text, (int)(sizeof text / sizeof text[0]));
    if (g_about_dark) {
        HBRUSH face = CreateSolidBrush(pressed ? RGB(45, 45, 45) : RGB(60, 60, 60));
        HBRUSH edge = CreateSolidBrush(focus ? THEME_DARK_LINK : RGB(100, 100, 100));
        if (g_about_bg != NULL) FillRect(di->hDC, &r, g_about_bg);
        if (face != NULL) {
            FillRect(di->hDC, &r, face);
            DeleteObject(face);
        }
        if (edge != NULL) {
            FrameRect(di->hDC, &r, edge);
            DeleteObject(edge);
        }
        SetTextColor(di->hDC, THEME_DARK_TEXT);
    } else {
        HTHEME th = OpenThemeData(di->hwndItem, L"BUTTON");
        if (th != NULL) {
            FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
            DrawThemeBackground(th, di->hDC, BP_PUSHBUTTON, pressed ? PBS_PRESSED : PBS_DEFAULTED, &r, NULL);
            CloseThemeData(th);
        } else {
            DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0u));
        }
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
        if (focus && (di->itemState & ODS_NOFOCUSRECT) == 0u) {
            RECT f = r;
            InflateRect(&f, -3, -3);
            DrawFocusRect(di->hDC, &f);
        }
    }
    SetBkMode(di->hDC, TRANSPARENT);
    if (pressed) OffsetRect(&r, 1, 1);
    DrawTextW(di->hDC, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((di->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0u));
    if (old != NULL) SelectObject(di->hDC, old);
}

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        if (icon != NULL) SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)icon, 0);
        SetDlgItemTextW(dlg, IDC_ABOUT_LICENSE, g_license);
        g_about_bg = CreateSolidBrush(THEME_DARK_BG);
        g_about_field = CreateSolidBrush(THEME_DARK_FIELD);
        about_theme(dlg);
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_DESTROY:
        if (g_about_bg != NULL) DeleteObject(g_about_bg);
        if (g_about_field != NULL) DeleteObject(g_about_field);
        g_about_bg = g_about_field = NULL;
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) {
            about_theme(dlg);
            RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        break;
    case WM_DRAWITEM:
        if (w == IDOK) {
            about_button((const DRAWITEMSTRUCT *)l);
            return TRUE;
        }
        break;
    case WM_CTLCOLORDLG:
        if (g_about_dark && g_about_bg != NULL) return (INT_PTR)g_about_bg;
        break;
    case WM_CTLCOLORSTATIC: {
        int dark = g_about_dark && g_about_bg != NULL && g_about_field != NULL;
        if ((HWND)l == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            SetTextColor((HDC)w, dark ? THEME_DARK_LINK : RGB(0, 0, 204));
            SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)(dark ? g_about_bg : GetSysColorBrush(COLOR_BTNFACE));
        }
        if (dark) {
            int field = (HWND)l == GetDlgItem(dlg, IDC_ABOUT_LICENSE);
            SetTextColor((HDC)w, THEME_DARK_TEXT);
            SetBkColor((HDC)w, field ? THEME_DARK_FIELD : THEME_DARK_BG);
            return (INT_PTR)(field ? g_about_field : g_about_bg);
        }
        break;
    }
    case WM_SETCURSOR:
        if ((HWND)w == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649))); /* IDC_HAND */
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_ABOUT_LINK && HIWORD(w) == STN_CLICKED) {
            ShellExecuteW(dlg, L"open", ABOUT_URL, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            EndDialog(dlg, 1);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

static const wchar_t g_about_title[] = L"About " APP_CAPTIONW L" v" APP_VERSIONW;

static void about(void)
{
    Tmpl t;
    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    memset(&t, 0, sizeof t);
    t_word(&t, LOWORD(style));
    t_word(&t, HIWORD(style));
    t_word(&t, 0);
    t_word(&t, 0);
    t_word(&t, 7); /* items */
    t_word(&t, 0);
    t_word(&t, 0);
    t_word(&t, 260);
    t_word(&t, 196);
    t_word(&t, 0); /* no menu */
    t_word(&t, 0); /* the dialog class */
    t_str(&t, g_about_title);
    t_word(&t, 9);
    t_str(&t, L"Segoe UI");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_ICON, 10, 10, 21, 20, IDC_ABOUT_ICON, 0x0082, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 42, 10, 208, 9, 0xFFFF, 0x0082, L"64bit version by Onur BUYUKCAGLAR");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 42, 21, 208, 9, 0xFFFF, 0x0082, L"with Anthropic Claude AI assistance");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_LEFT | SS_NOTIFY, 42, 32, 208, 9, IDC_ABOUT_LINK, 0x0082,
           ABOUT_URL);
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 48, 240, 9, 0xFFFF, 0x0082, L"Licenses:");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 59,
           240, 108, IDC_ABOUT_LICENSE, 0x0081, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 200, 174, 50, 15, IDOK, 0x0080, L"OK");
    g_modal++;
    update_pause();
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, g_hwnd, about_proc, 0);
    g_modal--;
    update_pause();
}

/* ==== full screen ======================================================================================== */
static void set_fullscreen(int on)
{
    DWORD style = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (on == g_full) return;
    if (on) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(g_hwnd, &g_place);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi); /* the window's own monitor */
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_hwnd, &g_place);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_full = on;
    CheckMenuItem(GetSystemMenu(g_hwnd, FALSE), SC_APP_FULLSCREEN, on ? MF_CHECKED : MF_UNCHECKED);
    update_clip();
}

/* ==== input ============================================================================================== */
static uint8_t g_keys_down[512]; /* by scancode (+256 for extended keys) */
static int g_buttons;

static void key(UINT vk, LPARAM l, int down)
{
    uint32_t sc = (uint32_t)((l >> 16) & 0xFF);
    int ext = (int)((l >> 24) & 1);
    if (vk == VK_PAUSE) return; /* its scancode sequence is unlike any other key's */
    if (vk == VK_NUMLOCK) ext = 0; /* Windows reports Num Lock as an extended key, the keyboard does not */
    if (sc == 0) return;
    g_keys_down[sc + (ext ? 256u : 0u)] = (uint8_t)down;
    rt_key_event(ext ? (0xE000u | sc) : sc, down);
}

/* Alt is passed to the game only once another key comes with it (or alone on release), so that the window's
 * Alt shortcuts never reach the game */
static int g_alt_held, g_alt_used, g_alt_sent;
static LPARAM g_alt_l;
static uint8_t g_swallow[256]; /* key releases that belong to a shortcut */

static void alt_release_held(void)
{
    if (g_alt_held && !g_alt_sent) {
        key(VK_MENU, g_alt_l, 1);
        g_alt_sent = 1;
    }
}

static void release_all(void)
{
    int i;
    g_alt_held = g_alt_sent = 0;
    memset(g_swallow, 0, sizeof g_swallow);
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

static void mouse_pos(HWND h, LPARAM l)
{
    RECT r = picture_rect(h);
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    int w = r.right - r.left, hh = r.bottom - r.top;
    int fx, fy;
    if (w <= 1 || hh <= 1) return;
    fx = (int)((long long)(x - r.left) * 65535 / (w - 1));
    fy = (int)((long long)(y - r.top) * 65535 / (hh - 1));
    if (fx < 0) fx = 0;
    if (fx > 65535) fx = 65535;
    if (fy < 0) fy = 0;
    if (fy > 65535) fy = 65535;
    rt_mouse_move(fx, fy);
}

static void mouse_buttons(HWND h, WPARAM w)
{
    int b = ((w & MK_LBUTTON) ? 1 : 0) | ((w & MK_RBUTTON) ? 2 : 0) | ((w & MK_MBUTTON) ? 4 : 0);
    if (b && !g_buttons) SetCapture(h);
    if (!b && g_buttons) ReleaseCapture();
    g_buttons = b;
    rt_mouse_buttons(b);
}

static void toggle_crt(HWND h)
{
    g_crt_on = !g_crt_on;
    CheckMenuItem(GetSystemMenu(h, FALSE), SC_APP_CRT, g_crt_on ? MF_CHECKED : MF_UNCHECKED);
    if (g_ini[0]) WritePrivateProfileStringW(L"video", L"crt", g_crt_on ? L"1" : L"0", g_ini);
    InvalidateRect(h, NULL, FALSE);
}

/* ==== the window ========================================================================================= */
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        HMENU sm = GetSystemMenu(h, FALSE);
        g_hwnd = h;
        theme_apply(0);
        AppendMenuW(sm, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sm, MF_STRING, SC_APP_FULLSCREEN, L"&Full screen\tAlt+Enter");
        AppendMenuW(sm, MF_STRING | (g_crt_on ? MF_CHECKED : MF_UNCHECKED), SC_APP_CRT, L"&CRT scanlines\tAlt+S");
        AppendMenuW(sm, MF_STRING, SC_APP_ABOUT, L"&About " APP_CAPTIONW L"\x2026");
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) theme_apply(1);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_SIZE:
        g_minimized = w == SIZE_MINIMIZED;
        update_pause();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 320;
        mm->ptMinTrackSize.y = 240;
        return 0;
    }
    case WM_ACTIVATEAPP:
        if (!w) release_all();
        update_clip();
        return 0;
    case WM_ACTIVATE:
        update_clip(); /* Windows drops the clip when another window had the focus */
        break;
    case WM_ENTERMENULOOP:
    case WM_EXITMENULOOP:
        g_in_menu = msg == WM_ENTERMENULOOP;
        update_clip();
        break;
    case WM_KILLFOCUS:
        release_all();
        return 0;
    case WM_SYSCOMMAND:
        switch (w & 0xFFF0u) {
        case SC_APP_ABOUT:
            about();
            return 0;
        case SC_APP_FULLSCREEN:
            set_fullscreen(!g_full);
            return 0;
        case SC_APP_CRT:
            toggle_crt(h);
            return 0;
        case SC_KEYMENU:
            if (l != ' ') return 0; /* F10 and Alt alone go to the game; Alt+Space opens the system menu */
            break;
        default:
            break;
        }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_MENU) { /* Alt: held back until it is clear it is not one of the shortcuts */
            if (!g_alt_held) {
                g_alt_held = 1;
                g_alt_used = 0;
                g_alt_sent = 0;
                g_alt_l = l;
            }
            return 0;
        }
        if (w == VK_RETURN || w == 'S') {
            /* Alt+Enter: full screen; Alt+S: the CRT scanline filter.  Neither the key nor the Alt reaches the
             * game (a key press would skip the intro). */
            g_alt_used = 1;
            g_swallow[w & 0xFF] = 1;
            if (!(l & (1 << 30))) {
                if (w == VK_RETURN)
                    set_fullscreen(!g_full);
                else
                    toggle_crt(h);
            }
            return 0;
        }
        if (w == VK_F4) break; /* Alt+F4: close */
        alt_release_held();
        key((UINT)w, l, 1);
        if (w == VK_SPACE) break;
        return 0;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (w == VK_MENU) {
            if (g_alt_held && !g_alt_sent && !g_alt_used) {
                key(VK_MENU, g_alt_l, 1); /* Alt alone: the game gets the press now */
                g_alt_sent = 1;
            }
            if (g_alt_sent) key(VK_MENU, l, 0);
            g_alt_held = 0;
            g_alt_sent = 0;
            return 0;
        }
        if (g_swallow[w & 0xFF]) {
            g_swallow[w & 0xFF] = 0;
            return 0;
        }
        key((UINT)w, l, 0);
        if (msg == WM_SYSKEYUP && (w == VK_SPACE || w == VK_F4)) break;
        return 0;
    case WM_KEYDOWN:
        alt_release_held();
        key((UINT)w, l, 1);
        return 0;
    case WM_SYSCHAR:
        if (w == ' ') break;
        return 0;
    case WM_CHAR:
        return 0;
    case WM_MOUSEMOVE:
        mouse_pos(h, l);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        mouse_pos(h, l);
        mouse_buttons(h, w);
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        mouse_pos(h, l);
        mouse_buttons(h, w);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p;
            RECT r = picture_rect(h);
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            /* the game draws its own pointer over the picture */
            SetCursor(PtInRect(&r, p) ? NULL : LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        InterlockedExchange(&g_quit, 1);
        g_modal = 0;
        g_minimized = 0;
        update_pause();
        if (g_game_done || g_game_thread == NULL) {
            DestroyWindow(h);
        } else {
            SetTimer(h, 1, 3000, NULL); /* the game thread gets three seconds to end */
        }
        return 0;
    case WM_TIMER:
        if (w == 1) DestroyWindow(h);
        return 0;
    case WM_APP_GAME_DONE:
        DestroyWindow(h);
        return 0;
    case WM_APP_FATAL:
        g_modal++;
        update_clip();
        MessageBoxW(h, g_fatal_text, APP_CAPTIONW, MB_OK | MB_ICONERROR);
        ExitProcess(3);
        return 0;
    case WM_DESTROY:
        if (g_clipped) ClipCursor(NULL); /* a clip outlives the window that set it */
        g_clipped = 0;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ==== finding the game =================================================================================== */
static int has_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, sizeof p / sizeof p[0], L"%s\\SYND.EXE", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* ---- where the program writes: next to the program when it may write there (a portable copy on a USB stick or
 * in a synced folder keeps its saves with it), else in %LOCALAPPDATA%\Syndicate64 ---- */
static void exe_dir(wchar_t *out, size_t n)
{
    wchar_t *s;
    GetModuleFileNameW(NULL, out, (DWORD)n);
    s = wcsrchr(out, L'\\');
    if (s != NULL) *s = 0;
}

/* the folder exists (made if need be) and a file can be created in it */
static int writable_dir(const wchar_t *dir)
{
    wchar_t probe[MAX_PATH * 2];
    HANDLE h;
    CreateDirectoryW(dir, NULL);
    _snwprintf(probe, sizeof probe / sizeof probe[0], L"%s\\~syndicate64-%lu.tmp", dir, GetCurrentProcessId());
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void local_dir(wchar_t *out, size_t n)
{
    wchar_t *base = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &base))) {
        _snwprintf(out, n, L"%s\\Syndicate64", base);
        CoTaskMemFree(base);
        CreateDirectoryW(out, NULL);
    }
}

/* the saved games and every other file the game writes: Save next to the program, else %LOCALAPPDATA%\Syndicate64 */
static void save_dir(wchar_t *out, size_t n)
{
    wchar_t dir[MAX_PATH * 2];
    exe_dir(dir, sizeof dir / sizeof dir[0]);
    _snwprintf(out, n, L"%s\\Save", dir);
    if (writable_dir(out)) return;
    local_dir(out, n);
}

/* the settings (Syndicate64.ini): next to the program, else in %LOCALAPPDATA%\Syndicate64 */
static void settings_file(wchar_t *out, size_t n)
{
    wchar_t dir[MAX_PATH * 2];
    exe_dir(dir, sizeof dir / sizeof dir[0]);
    if (!writable_dir(dir)) local_dir(dir, sizeof dir / sizeof dir[0]);
    _snwprintf(out, n, L"%s\\Syndicate64.ini", dir);
}

static int pick_folder(wchar_t *out, size_t n)
{
    IFileOpenDialog *d = NULL;
    IShellItem *it = NULL;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    {
        DWORD opts = 0;
        IFileOpenDialog_GetOptions(d, &opts);
        IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(d, L"Choose the Syndicate Plus folder (the one with SYND.EXE)");
    }
    if (SUCCEEDED(IFileOpenDialog_Show(d, NULL)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &it)) &&
        SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
        _snwprintf(out, n, L"%s", p);
        CoTaskMemFree(p);
        ok = 1;
    }
    if (it != NULL) IShellItem_Release(it);
    IFileOpenDialog_Release(d);
    return ok;
}

static int find_game(wchar_t *out, size_t n, const wchar_t *ini)
{
    wchar_t exe[MAX_PATH], dir[MAX_PATH * 2];
    wchar_t *s;
    int up;
    /* next to the program */
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    s = wcsrchr(exe, L'\\');
    if (s != NULL) *s = 0;
    if (has_game(exe)) {
        _snwprintf(out, n, L"%s", exe);
        return 1;
    }
    _snwprintf(dir, sizeof dir / sizeof dir[0], L"%s\\SYNDPLUS", exe);
    if (has_game(dir)) {
        _snwprintf(out, n, L"%s", dir);
        return 1;
    }
    /* a source checkout: _references\dos in the program's folder or above it */
    _snwprintf(dir, sizeof dir / sizeof dir[0], L"%s", exe);
    for (up = 0; up < 5; up++) {
        wchar_t probe[MAX_PATH * 2];
        _snwprintf(probe, sizeof probe / sizeof probe[0], L"%s\\_references\\dos", dir);
        if (has_game(probe)) {
            _snwprintf(out, n, L"%s", probe);
            return 1;
        }
        s = wcsrchr(dir, L'\\');
        if (s == NULL) break;
        *s = 0;
    }
    /* the folder chosen before */
    if (GetPrivateProfileStringW(L"game", L"folder", L"", dir, (DWORD)(sizeof dir / sizeof dir[0]), ini) > 0 &&
        has_game(dir)) {
        _snwprintf(out, n, L"%s", dir);
        return 1;
    }
    /* ask */
    for (;;) {
        if (!pick_folder(dir, sizeof dir / sizeof dir[0])) return 0;
        if (has_game(dir)) break;
        if (MessageBoxW(NULL,
                        L"That folder does not hold the original Syndicate Plus (SYND.EXE was not found in it). "
                        L"Choose the folder the game was installed to.",
                        APP_CAPTIONW, MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return 0;
    }
    WritePrivateProfileStringW(L"game", L"folder", dir, ini);
    _snwprintf(out, n, L"%s", dir);
    return 1;
}

/* the game files built into the program (the GAMEPAK resource of a private build), or NULL */
const uint8_t *host_game_pack(size_t *size)
{
    HRSRC r = FindResourceW(NULL, L"GAMEPAK", RT_RCDATA);
    HGLOBAL g;
    *size = 0;
    if (r == NULL || (g = LoadResource(NULL, r)) == NULL) return NULL;
    *size = SizeofResource(NULL, r);
    return (const uint8_t *)LockResource(g);
}

/* ==== start ============================================================================================== */
static void to_utf8(const wchar_t *w, char *out, int n)
{
    int i;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL) == 0) out[0] = 0;
    for (i = 0; out[i]; i++)
        if (out[i] == '\\') out[i] = '/';
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    int argc = 0, i, full = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t game_dir[MAX_PATH * 2] = L"", user_dir[MAX_PATH * 2] = L"";
    static char start_u8[512];
    WNDCLASSEXW wc;
    RECT wr;
    MSG m;
    int scale;
    (void)prev;
    (void)cmd;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    theme_apply(0);
    settings_file(g_ini, sizeof g_ini / sizeof g_ini[0]);
    memset(&g_opt, 0, sizeof g_opt);
    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : L"";
        if (!wcscmp(a, L"--game-dir")) {
            _snwprintf(game_dir, sizeof game_dir / sizeof game_dir[0], L"%s", v);
            i++;
        } else if (!wcscmp(a, L"--user-dir")) {
            _snwprintf(user_dir, sizeof user_dir / sizeof user_dir[0], L"%s", v);
            i++;
        } else if (!wcscmp(a, L"--start")) {
            to_utf8(v, start_u8, (int)sizeof start_u8);
            for (char *c = start_u8; *c && *c != ' '; c++) /* the program's path; its arguments stay as given */
                if (*c == '/') *c = '\\';
            g_opt.start = start_u8;
            i++;
        } else if (!wcscmp(a, L"--log")) {
            g_logfile = _wfopen(v, L"w");
            i++;
        } else if (!wcscmp(a, L"--verbose")) {
            g_opt.verbose = _wtoi(v);
            i++;
        } else if (!wcscmp(a, L"--fullscreen")) {
            full = 1;
        }
    }
    {
        size_t pack_size;
        if (game_dir[0] != 0 || host_game_pack(&pack_size) == NULL) { /* else: the built-in files */
            if (game_dir[0] == 0 && !find_game(game_dir, sizeof game_dir / sizeof game_dir[0], g_ini)) return 1;
            if (!has_game(game_dir)) {
                MessageBoxW(NULL, L"SYND.EXE was not found in the game folder.", APP_CAPTIONW, MB_OK | MB_ICONERROR);
                return 1;
            }
        }
    }
    if (user_dir[0] == 0) save_dir(user_dir, sizeof user_dir / sizeof user_dir[0]);
    g_crt_on = GetPrivateProfileIntW(L"video", L"crt", 0, g_ini) != 0;
    CreateDirectoryW(user_dir, NULL);
    to_utf8(game_dir, g_game_dir_u8, (int)sizeof g_game_dir_u8);
    to_utf8(user_dir, g_user_dir_u8, (int)sizeof g_user_dir_u8);
    g_opt.game_dir = g_game_dir_u8;
    g_opt.user_dir = g_user_dir_u8;
    if (g_opt.verbose == 0) g_opt.verbose = g_logfile != NULL ? 1 : 0;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"Syndicate64Window";
    RegisterClassExW(&wc);

    /* the largest whole multiple of 320x240 that fits 85% of the work area */
    {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        for (scale = 6; scale > 1; scale--)
            if (320 * scale <= (work.right - work.left) * 85 / 100 && 240 * scale <= (work.bottom - work.top) * 85 / 100)
                break;
    }
    wr.left = 0;
    wr.top = 0;
    wr.right = 320 * scale;
    wr.bottom = 240 * scale;
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_CAPTIONW, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    if (g_hwnd == NULL) return 1;
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    if (full) set_fullscreen(1);

    audio_start();
    g_game_thread = CreateThread(NULL, 16u << 20, game_thread, NULL, 0, NULL);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    InterlockedExchange(&g_quit, 1);
    rt_set_paused(0);
    if (g_game_thread != NULL) WaitForSingleObject(g_game_thread, 2000);
    audio_stop();
    if (g_logfile != NULL) fclose(g_logfile);
    LocalFree(argv);
    CoUninitialize();
    ExitProcess(0);
}
