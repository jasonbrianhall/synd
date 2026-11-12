/* posix_sys.c - the host services every POSIX front end shares (the macOS app and the macOS/Linux build of
 * synd_headless): time, sleeping, the input lock, logging and the file system helpers.  The counterpart of
 * win32_sys.c, with the same behaviour:
 *
 * - a monotonic clock in microseconds, and the virtual clock of the tests (g_virtual_step);
 * - folder listings in the order NTFS gives them (by name, ignoring case), so a DOS directory search sees the
 *   same files in the same order on every platform;
 * - host_find_name ignores case, as on Windows (a Mac volume may be case-sensitive). */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "../../rt/host.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
FILE *g_logfile;

/* the headless test front end can run on a virtual clock, for runs that repeat exactly: every reading of the
 * clock advances it by g_virtual_step microseconds and a sleep by its length (0: the real clock) */
uint32_t g_virtual_step;
static uint64_t g_virtual_now = 1000000u;

uint64_t host_time_us(void)
{
    struct timespec t;
    if (g_virtual_step) return g_virtual_now += g_virtual_step;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void host_sleep_us(uint32_t us)
{
    struct timespec t;
    if (g_virtual_step) {
        g_virtual_now += us;
        return;
    }
    /* Windows sleeps at least a millisecond (timeBeginPeriod(1)); do the same so the game's pacing loops
     * behave alike */
    if (us < 1000u) us = 1000u;
    t.tv_sec = (time_t)(us / 1000000u);
    t.tv_nsec = (long)(us % 1000000u) * 1000L;
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

void host_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

void host_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

void host_log(const char *line)
{
    if (g_logfile != NULL) {
        fprintf(g_logfile, "%s\n", line);
        fflush(g_logfile);
    }
}

int host_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    return errno == EEXIST && host_is_dir(path) ? 0 : -1;
}

int host_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* a file's modification time as DOS packs it (local time, two-second steps, 1980 at the earliest) */
static void dos_stamp(time_t t, uint16_t *dos_time, uint16_t *dos_date)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80) {
        *dos_time = 0;
        *dos_date = (uint16_t)((1u << 5) | 1u); /* 1 January 1980 */
        return;
    }
    *dos_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *dos_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

/* a listing is read whole at open and sorted, as NTFS returns names (ordered, ignoring case) */
typedef struct Dir {
    HostDirEntry *e;
    int n, i;
} Dir;

static int by_name(const void *a, const void *b)
{
    const HostDirEntry *x = (const HostDirEntry *)a, *y = (const HostDirEntry *)b;
    int c = strcasecmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->name, y->name);
}

void *host_dir_open(const char *path)
{
    DIR *dp = opendir(path);
    struct dirent *de;
    Dir *d;
    int cap = 0;
    if (dp == NULL) return NULL;
    d = (Dir *)calloc(1, sizeof *d);
    if (d == NULL) {
        closedir(dp);
        return NULL;
    }
    while ((de = readdir(dp)) != NULL) {
        char full[1400];
        struct stat st;
        HostDirEntry *e;
        if (strlen(de->d_name) >= sizeof d->e[0].name) continue;
        if (de->d_name[0] == '.' && strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            continue; /* hidden files (.DS_Store and the like) are not the game's */
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        if (stat(full, &st) != 0) continue;
        if (d->n == cap) {
            int nc = cap ? cap * 2 : 32;
            HostDirEntry *ne = (HostDirEntry *)realloc(d->e, (size_t)nc * sizeof *ne);
            if (ne == NULL) break;
            d->e = ne;
            cap = nc;
        }
        e = &d->e[d->n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->is_dir = S_ISDIR(st.st_mode);
        e->size = e->is_dir ? 0u : (uint32_t)st.st_size;
        dos_stamp(st.st_mtime, &e->dos_time, &e->dos_date);
    }
    closedir(dp);
    if (d->n > 1) qsort(d->e, (size_t)d->n, sizeof *d->e, by_name);
    return d;
}

int host_dir_next(void *dir, HostDirEntry *e)
{
    Dir *d = (Dir *)dir;
    if (d->i >= d->n) return 0;
    *e = d->e[d->i++];
    return 1;
}

void host_dir_close(void *dir)
{
    Dir *d = (Dir *)dir;
    if (d == NULL) return;
    free(d->e);
    free(d);
}

int host_find_name(const char *dir, const char *name, char *out, size_t out_size)
{
    /* as on NTFS, case does not matter: an exact match first, else the first name equal ignoring case */
    char full[1400];
    struct stat st;
    DIR *dp;
    struct dirent *de;
    int found = 0;
    if (strchr(name, '*') || strchr(name, '?') || name[0] == 0) return 0;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    dp = opendir(dir);
    if (dp == NULL) return 0;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, name) == 0) {
            found = 2;
            break;
        }
        if (!found && strcasecmp(de->d_name, name) == 0) {
            snprintf(out, out_size, "%s", de->d_name);
            found = 1;
        }
    }
    closedir(dp);
    if (found == 2) {
        if (stat(full, &st) != 0) return 0;
        snprintf(out, out_size, "%s", name);
    }
    return found != 0;
}
