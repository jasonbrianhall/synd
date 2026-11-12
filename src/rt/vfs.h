/* vfs.h - the files the core reads and writes: real files, or the original game files built into the program
 * as a pack (tools/pack_game.py; the platform hands it over with host_game_pack()).
 *
 * Paths are host paths ('/' between components).  A path that starts with VFS_PACK_ROOT names a file in the
 * pack (read only); any other path goes to the host file system.  rt_game_dir is VFS_PACK_ROOT when the game
 * runs from the built-in files. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "host.h"

#define VFS_PACK_ROOT "<built-in>"

typedef struct VFile VFile;

/* the pack (`data` stays valid while the program runs); 0 when it is not a valid pack */
int vfs_set_pack(const uint8_t *data, size_t size);
int vfs_have_pack(void);

/* mode as fopen's ("rb", "wb", "w+b", "r+b"); writing modes fail for pack files */
VFile *vfs_open(const char *path, const char *mode);
size_t vfs_read(VFile *f, void *buf, size_t n);
size_t vfs_write(VFile *f, const void *buf, size_t n);
int vfs_getc(VFile *f); /* -1 at the end */
long vfs_seek(VFile *f, long off, int whence); /* SEEK_SET/CUR/END; returns the new position */
void vfs_close(VFile *f);

/* the host_* directory helpers, aware of the pack */
int vfs_is_dir(const char *path);
int vfs_find_name(const char *dir, const char *name, char *out, size_t out_size);
void *vfs_dir_open(const char *path);
int vfs_dir_next(void *dir, HostDirEntry *e);
void vfs_dir_close(void *dir);
