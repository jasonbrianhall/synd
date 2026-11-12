/* vfs.c - real files and the built-in game pack (see vfs.h).
 *
 * The pack (written by tools/pack_game.py), little-endian:
 *   "SYNPAK01", u32 count, then `count` entries: u16 path length, the path (upper case, '\' between
 *   components, relative to the game folder, e.g. "DATA\GAMEDG.DLL"), u32 offset, u32 size, u16 DOS time,
 *   u16 DOS date; the file data at the offsets (from the start of the pack).  Folders are implied by paths. */
#include "vfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PackEntry {
    const char *path; /* not terminated: path_len characters */
    uint16_t path_len, dos_time, dos_date;
    uint32_t offset, size;
} PackEntry;

static const uint8_t *g_pack;
static size_t g_pack_size;
static PackEntry *g_ent;
static uint32_t g_nent;

struct VFile {
    FILE *f;
    const uint8_t *mem; /* a pack file */
    size_t size, pos;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int vfs_set_pack(const uint8_t *data, size_t size)
{
    size_t pos = 12;
    uint32_t n, i;
    if (data == NULL || size < 12 || memcmp(data, "SYNPAK01", 8) != 0) return 0;
    n = rd32(data + 8);
    free(g_ent);
    g_ent = (PackEntry *)calloc(n ? n : 1, sizeof *g_ent);
    if (g_ent == NULL) return 0;
    for (i = 0; i < n; i++) {
        PackEntry *e = &g_ent[i];
        if (pos + 2 > size) return 0;
        e->path_len = rd16(data + pos);
        pos += 2;
        if (pos + e->path_len + 12 > size) return 0;
        e->path = (const char *)data + pos;
        pos += e->path_len;
        e->offset = rd32(data + pos);
        e->size = rd32(data + pos + 4);
        e->dos_time = rd16(data + pos + 8);
        e->dos_date = rd16(data + pos + 10);
        pos += 12;
        if ((size_t)e->offset + e->size > size) return 0;
    }
    g_pack = data;
    g_pack_size = size;
    g_nent = n;
    return 1;
}

int vfs_have_pack(void)
{
    return g_pack != NULL;
}

/* "<built-in>/DATA/X.DAT" -> "DATA\X.DAT" (upper case); NULL for a host path */
static const char *pack_rel(const char *path, char *out, size_t n)
{
    size_t root = strlen(VFS_PACK_ROOT), i;
    if (g_pack == NULL || strncmp(path, VFS_PACK_ROOT, root) != 0) return NULL;
    path += root;
    while (*path == '/' || *path == '\\') path++;
    for (i = 0; path[i] && i + 1 < n; i++) out[i] = path[i] == '/' ? '\\' : (char)toupper((unsigned char)path[i]);
    while (i > 0 && out[i - 1] == '\\') i--;
    out[i] = '\0';
    return out;
}

static const PackEntry *pack_file(const char *rel)
{
    size_t l = strlen(rel);
    uint32_t i;
    for (i = 0; i < g_nent; i++)
        if (g_ent[i].path_len == l && memcmp(g_ent[i].path, rel, l) == 0) return &g_ent[i];
    return NULL;
}

/* a folder: the root, or a prefix "rel\" of some entry */
static int pack_dir(const char *rel)
{
    size_t l = strlen(rel);
    uint32_t i;
    if (l == 0) return 1;
    for (i = 0; i < g_nent; i++)
        if (g_ent[i].path_len > l && memcmp(g_ent[i].path, rel, l) == 0 && g_ent[i].path[l] == '\\') return 1;
    return 0;
}

VFile *vfs_open(const char *path, const char *mode)
{
    char rel[512];
    VFile *v;
    if (pack_rel(path, rel, sizeof rel) != NULL) {
        const PackEntry *e;
        if (mode[0] != 'r' || strchr(mode, '+') != NULL) return NULL;
        e = pack_file(rel);
        if (e == NULL) return NULL;
        v = (VFile *)calloc(1, sizeof *v);
        if (v == NULL) return NULL;
        v->mem = g_pack + e->offset;
        v->size = e->size;
        return v;
    }
    v = (VFile *)calloc(1, sizeof *v);
    if (v == NULL) return NULL;
    v->f = fopen(path, mode);
    if (v->f == NULL) {
        free(v);
        return NULL;
    }
    return v;
}

size_t vfs_read(VFile *f, void *buf, size_t n)
{
    if (f->f != NULL) return fread(buf, 1, n, f->f);
    if (f->pos >= f->size) return 0;
    if (n > f->size - f->pos) n = f->size - f->pos;
    memcpy(buf, f->mem + f->pos, n);
    f->pos += n;
    return n;
}

size_t vfs_write(VFile *f, const void *buf, size_t n)
{
    return f->f != NULL ? fwrite(buf, 1, n, f->f) : 0;
}

int vfs_getc(VFile *f)
{
    if (f->f != NULL) {
        int c = fgetc(f->f);
        return c == EOF ? -1 : c;
    }
    return f->pos < f->size ? f->mem[f->pos++] : -1;
}

long vfs_seek(VFile *f, long off, int whence)
{
    long base;
    if (f->f != NULL) {
        fseek(f->f, off, whence);
        return ftell(f->f);
    }
    base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->pos : (long)f->size;
    if (base + off < 0) off = -base;
    f->pos = (size_t)(base + off); /* past the end reads nothing, as with a file */
    return (long)f->pos;
}

void vfs_close(VFile *f)
{
    if (f == NULL) return;
    if (f->f != NULL) fclose(f->f);
    free(f);
}

int vfs_is_dir(const char *path)
{
    char rel[512];
    if (pack_rel(path, rel, sizeof rel) != NULL) return pack_dir(rel);
    return host_is_dir(path);
}

int vfs_find_name(const char *dir, const char *name, char *out, size_t out_size)
{
    char rel[512], full[600];
    size_t i;
    if (pack_rel(dir, rel, sizeof rel) == NULL) return host_find_name(dir, name, out, out_size);
    snprintf(full, sizeof full, "%s%s%s", rel, rel[0] ? "\\" : "", name);
    for (i = 0; full[i]; i++) full[i] = (char)toupper((unsigned char)full[i]);
    if (pack_file(full) == NULL && !pack_dir(full)) return 0;
    snprintf(out, out_size, "%s", full + strlen(full) - strlen(name));
    return 1;
}

/* a folder listing: the pack's children of a folder, gathered at open; otherwise the host's */
typedef struct PackDir {
    int is_pack;
    void *host;
    HostDirEntry *e;
    int n, i;
} PackDir;

void *vfs_dir_open(const char *path)
{
    char rel[512];
    PackDir *d;
    if (pack_rel(path, rel, sizeof rel) == NULL) {
        void *h = host_dir_open(path);
        if (h == NULL) return NULL;
        d = (PackDir *)calloc(1, sizeof *d);
        if (d == NULL) {
            host_dir_close(h);
            return NULL;
        }
        d->host = h;
        return d;
    }
    if (!pack_dir(rel)) return NULL;
    d = (PackDir *)calloc(1, sizeof *d);
    if (d == NULL) return NULL;
    d->is_pack = 1;
    d->e = (HostDirEntry *)calloc(g_nent ? g_nent : 1, sizeof *d->e);
    if (d->e == NULL) {
        free(d);
        return NULL;
    }
    {
        size_t l = strlen(rel);
        uint32_t i;
        for (i = 0; i < g_nent; i++) {
            const PackEntry *pe = &g_ent[i];
            const char *rest;
            size_t rl, k;
            int is_dir, j, dup = 0;
            if (l > 0 && !(pe->path_len > l && memcmp(pe->path, rel, l) == 0 && pe->path[l] == '\\')) continue;
            rest = pe->path + (l ? l + 1 : 0);
            rl = pe->path_len - (size_t)(rest - pe->path);
            for (k = 0; k < rl && rest[k] != '\\'; k++) {
            }
            is_dir = k < rl;
            for (j = 0; j < d->n && !dup; j++)
                dup = strlen(d->e[j].name) == k && memcmp(d->e[j].name, rest, k) == 0;
            if (dup || k >= sizeof d->e[0].name) continue;
            memcpy(d->e[d->n].name, rest, k);
            d->e[d->n].name[k] = '\0';
            d->e[d->n].is_dir = is_dir;
            d->e[d->n].size = is_dir ? 0 : pe->size;
            d->e[d->n].dos_time = pe->dos_time;
            d->e[d->n].dos_date = pe->dos_date;
            d->n++;
        }
    }
    return d;
}

int vfs_dir_next(void *dir, HostDirEntry *e)
{
    PackDir *d = (PackDir *)dir;
    if (!d->is_pack) return host_dir_next(d->host, e);
    if (d->i >= d->n) return 0;
    *e = d->e[d->i++];
    return 1;
}

void vfs_dir_close(void *dir)
{
    PackDir *d = (PackDir *)dir;
    if (d == NULL) return;
    if (d->host != NULL) host_dir_close(d->host);
    free(d->e);
    free(d);
}
