/* win32_sys.c - the host services every Windows front end shares: time, sleeping, the input lock, logging
 * and the file system helpers (UTF-8 paths converted to UTF-16). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt/host.h"

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER g_freq;
FILE *g_logfile;

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    return TRUE;
}

static void init(void)
{
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
}

/* the headless test front end can run on a virtual clock, for runs that repeat exactly: every reading of the
 * clock advances it by g_virtual_step microseconds and a sleep by its length (0: the real clock) */
uint32_t g_virtual_step;
static uint64_t g_virtual_now = 1000000u;

uint64_t host_time_us(void)
{
    LARGE_INTEGER t;
    if (g_virtual_step) return g_virtual_now += g_virtual_step;
    init();
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / g_freq.QuadPart) * 1000000u +
           (uint64_t)(t.QuadPart % g_freq.QuadPart) * 1000000u / (uint64_t)g_freq.QuadPart;
}

void host_sleep_us(uint32_t us)
{
    if (g_virtual_step) {
        g_virtual_now += us;
        return;
    }
    init();
    Sleep(us < 1000u ? 1u : us / 1000u);
}

void host_lock(void)
{
    init();
    EnterCriticalSection(&g_lock);
}

void host_unlock(void)
{
    LeaveCriticalSection(&g_lock);
}

void host_log(const char *line)
{
    init();
    if (g_logfile != NULL) {
        fprintf(g_logfile, "%s\n", line);
        fflush(g_logfile);
    }
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

static void widen(const char *s, wchar_t *w, int n)
{
    int i;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) == 0) w[0] = 0;
    for (i = 0; w[i]; i++)
        if (w[i] == L'/') w[i] = L'\\';
}

static void narrow(const wchar_t *w, char *s, int n)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) == 0) s[0] = 0;
}

int host_mkdir(const char *path)
{
    wchar_t w[1024];
    widen(path, w, 1024);
    if (CreateDirectoryW(w, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

int host_is_dir(const char *path)
{
    wchar_t w[1024];
    DWORD a;
    widen(path, w, 1024);
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

typedef struct Dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int first;
} Dir;

void *host_dir_open(const char *path)
{
    wchar_t w[1100];
    Dir *d;
    widen(path, w, 1024);
    wcscat(w, L"\\*");
    d = (Dir *)calloc(1, sizeof *d);
    if (d == NULL) return NULL;
    d->h = FindFirstFileW(w, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

int host_dir_next(void *dir, HostDirEntry *e)
{
    Dir *d = (Dir *)dir;
    FILETIME lt;
    WORD dd = 0, dt = 0;
    if (!d->first && !FindNextFileW(d->h, &d->fd)) return 0;
    d->first = 0;
    narrow(d->fd.cFileName, e->name, (int)sizeof e->name);
    e->size = d->fd.nFileSizeLow;
    e->is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    FileTimeToLocalFileTime(&d->fd.ftLastWriteTime, &lt);
    FileTimeToDosDateTime(&lt, &dd, &dt);
    e->dos_date = dd;
    e->dos_time = dt;
    return 1;
}

void host_dir_close(void *dir)
{
    Dir *d = (Dir *)dir;
    FindClose(d->h);
    free(d);
}

int host_find_name(const char *dir, const char *name, char *out, size_t out_size)
{
    /* NTFS is case-insensitive: check existence and return the name as stored */
    wchar_t w[1100], wn[300];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    widen(dir, w, 1024);
    widen(name, wn, 300);
    wcscat(w, L"\\");
    wcscat(w, wn);
    if (wcschr(wn, L'*') || wcschr(wn, L'?')) return 0;
    h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    narrow(fd.cFileName, out, (int)out_size);
    return 1;
}
