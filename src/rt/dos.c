/* dos.c - the DOS services (int 21h, int 2Fh) a DOS/4GW program uses, on a virtual file system:
 *
 *   D:  the original game files (rt_game_dir: SYND.EXE, DATA, SYNDICAT, DATADISK; a folder, or the files
 *       built into the program, see vfs.h); the CD-ROM drive.
 *       Files the game writes there go to an overlay (rt_user_dir/D) and are read back from it first,
 *       so the original files are never changed.
 *   C:  rt_user_dir/C, the hard disk (C:\SYND: the setup file); reads that miss fall back to rt_game_dir
 *       (the reference copy has SYND\SYND.INF there).  The saved games have folders of their own:
 *       C:\SYND\SAVE (Syndicate) is rt_user_dir/Syndicate, C:\SYND\SAVE2 (American Revolt) is
 *       rt_user_dir/American Revolt.
 *
 * DOS EXEC (4Bh) runs another recompiled program nested (game_find_module); COMMAND.COM /C is understood
 * for the commands the games give it ("cd <dir>", "<program> <args>", output redirection ignored). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"
#include "vfs.h"

#define MAX_HANDLES 64

typedef struct DosFile {
    VFile *f;
    int used;
    int device; /* stdin/stdout/... */
    char *cow;  /* opened for writing but still the original: the DOS path, copied to the overlay on the first write */
} DosFile;

typedef struct DosState {
    int drive;          /* 2 = C, 3 = D */
    char cwd[26][128];  /* per drive, without the drive and leading '\', e.g. "SYNDICAT" */
    uint32_t dta;
    DosFile files[MAX_HANDLES];
} DosState;

static DosState g_dos;
static char g_progpath[128];

/* ---- paths ---------------------------------------------------------------------------------------------- */
static void str_upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* canonical form "X:\DIR\FILE" (upper case, no "." or "..") */
static void dos_canon(const char *in, char *out, size_t n)
{
    char tmp[300], *parts[32];
    int drive = g_dos.drive, np = 0, i;
    const char *p = in;
    char *s, *tok;
    while (*p == ' ') p++;
    if (p[0] && p[1] == ':') {
        drive = toupper((unsigned char)p[0]) - 'A';
        p += 2;
    }
    if (*p == '\\' || *p == '/') {
        snprintf(tmp, sizeof tmp, "%s", p + 1);
    } else if (g_dos.cwd[drive][0]) {
        snprintf(tmp, sizeof tmp, "%s\\%s", g_dos.cwd[drive], p);
    } else {
        snprintf(tmp, sizeof tmp, "%s", p);
    }
    for (s = tmp; *s; s++)
        if (*s == '/') *s = '\\';
    str_upper(tmp);
    s = tmp;
    while ((tok = strtok(s, "\\")) != NULL) {
        s = NULL;
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (np > 0) np--;
            continue;
        }
        /* trailing spaces / a colon device suffix ("NUL:") */
        if (np < 32) parts[np++] = tok;
    }
    snprintf(out, n, "%c:", 'A' + drive);
    for (i = 0; i < np; i++) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "\\%s", parts[i]);
    }
    if (np == 0) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "\\");
    }
}

/* join `root` and the components of `rel` ("A\B\C"), matching each component case-insensitively; returns 1
 * when the whole path exists (`out` is then the real path), 0 otherwise (`out` the path as given) */
static int host_path(const char *root, const char *rel, char *out, size_t n)
{
    char comp[260], real[260];
    const char *p = rel;
    int found = 1;
    snprintf(out, n, "%s", root);
    while (*p) {
        size_t k = 0;
        while (*p == '\\') p++;
        if (!*p) break;
        while (*p && *p != '\\' && k + 1 < sizeof comp) comp[k++] = *p++;
        comp[k] = '\0';
        {
            size_t l = strlen(out);
            if (found && vfs_find_name(out, comp, real, sizeof real))
                snprintf(out + l, n - l, "/%s", real);
            else {
                found = 0;
                snprintf(out + l, n - l, "/%s", comp);
            }
        }
    }
    return found;
}

/* the saved games' folders: rt_user_dir/<folder> instead of rt_user_dir/C/SYND/SAVE... */
static const struct {
    const char *dos; /* below C:\ */
    const char *folder;
} g_save_dirs[] = {{"SYND\\SAVE2", "American Revolt"}, {"SYND\\SAVE", "Syndicate"}};

/* the folders a canonical path ("C:\...") lives in: `overlay` (where writes go, read first) and `base` (the
 * original files, read when the overlay misses, relative path canon + 3); returns the path relative to
 * `overlay` */
static const char *drive_roots(const char *canon, char *base, size_t nb, char *overlay, size_t no)
{
    int drive = canon[0] - 'A';
    if (strlen(canon) < 3) canon = "?:\\"; /* "C:" alone: the root */
    if (drive == 2) {
        size_t i;
        for (i = 0; i < sizeof g_save_dirs / sizeof g_save_dirs[0]; i++) {
            size_t l = strlen(g_save_dirs[i].dos);
            if (strncmp(canon + 3, g_save_dirs[i].dos, l) == 0 && (canon[3 + l] == '\\' || canon[3 + l] == '\0')) {
                snprintf(overlay, no, "%s/%s", rt_user_dir, g_save_dirs[i].folder);
                snprintf(base, nb, "%s", rt_game_dir);
                return canon + 3 + l;
            }
        }
        snprintf(overlay, no, "%s/C", rt_user_dir);
        snprintf(base, nb, "%s", rt_game_dir); /* read fallback: the reference SYND folder */
    } else if (drive == 3) {
        snprintf(overlay, no, "%s/D", rt_user_dir);
        snprintf(base, nb, "%s", rt_game_dir);
    } else {
        snprintf(overlay, no, "%s/%c", rt_user_dir, 'A' + drive);
        base[0] = '\0';
    }
    return canon + 3;
}

int dos_resolve_read(const char *dos_path, char *host, size_t n)
{
    char canon[300], base[1024], overlay[1024];
    const char *rel;
    dos_canon(dos_path, canon, sizeof canon);
    rel = drive_roots(canon, base, sizeof base, overlay, sizeof overlay);
    if (host_path(overlay, rel, host, n)) return 1;
    if (base[0] && host_path(base, canon + 3, host, n)) return 1;
    return 0;
}

/* the overlay path of a DOS path, without creating anything */
static void overlay_path(const char *dos_path, char *host, size_t n)
{
    char canon[300], base[1024], overlay[1024];
    const char *rel;
    dos_canon(dos_path, canon, sizeof canon);
    rel = drive_roots(canon, base, sizeof base, overlay, sizeof overlay);
    host_path(overlay, rel, host, n);
}

int dos_resolve_write(const char *dos_path, char *host, size_t n)
{
    char canon[300], base[1024], overlay[1024], dir[1024], *slash;
    const char *rel;
    dos_canon(dos_path, canon, sizeof canon);
    rel = drive_roots(canon, base, sizeof base, overlay, sizeof overlay);
    host_path(overlay, rel, host, n);
    /* create the overlay directories on the way */
    snprintf(dir, sizeof dir, "%s", host);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        char *q;
        *slash = '\0';
        for (q = dir + strlen(rt_user_dir) + 1; *q; q++) {
            if (*q == '/') {
                *q = '\0';
                host_mkdir(dir);
                *q = '/';
            }
        }
        host_mkdir(dir);
    }
    return 1;
}

/* set the current drive and directory from a canonical "X:\DIR" (the game layer's direct start) */
void dos_chdir_host(const char *canon_dir)
{
    g_dos.drive = canon_dir[0] - 'A';
    snprintf(g_dos.cwd[g_dos.drive], sizeof g_dos.cwd[0], "%s", canon_dir[2] == '\\' ? canon_dir + 3 : "");
}

void dos_get_cwd(char *out, size_t n)
{
    snprintf(out, n, "%c:\\%s", 'A' + g_dos.drive, g_dos.cwd[g_dos.drive]);
}

static void read_guest_str(uint32_t addr, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        out[i] = (char)RD8(addr + (uint32_t)i);
        if (out[i] == '\0') break;
    }
    out[i < n ? i : n - 1] = '\0';
}

/* ---- state ------------------------------------------------------------------------------------------------ */
void dos_init(void)
{
    int i;
    memset(&g_dos, 0, sizeof g_dos);
    g_dos.drive = 3; /* D: */
    for (i = 0; i < 5; i++) {
        g_dos.files[i].used = 1;
        g_dos.files[i].device = 1;
    }
}

void dos_save_state(void **state)
{
    uint32_t *s = (uint32_t *)malloc(sizeof(uint32_t));
    *s = g_dos.dta;
    *state = s;
}

void dos_restore_state(void *state)
{
    g_dos.dta = *(uint32_t *)state;
    free(state);
}

void dos_close_all(void)
{
    int i;
    for (i = 5; i < MAX_HANDLES; i++) {
        if (g_dos.files[i].used && g_dos.files[i].f) vfs_close(g_dos.files[i].f);
        free(g_dos.files[i].cow);
        memset(&g_dos.files[i], 0, sizeof g_dos.files[i]);
    }
}

void dos_set_psp(uint32_t psp, const char *cmdtail, const char *progpath)
{
    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0DOS4G=QUIET\0\0";
    size_t len = strlen(cmdtail), envlen = sizeof env - 1, i;
    char canon[300];
    memset(M + psp, 0, 256);
    M[psp] = 0xCD;
    M[psp + 1] = 0x20;
    M[psp + 2] = 0x00; /* top of memory paragraph 0x9FFF */
    M[psp + 3] = 0xA0;
    WR16(psp + 0x2C, SEL_ENV);
    if (len > 126) len = 126;
    M[psp + 0x80] = (uint8_t)len;
    memcpy(M + psp + 0x81, cmdtail, len);
    M[psp + 0x81 + len] = 0x0D;
    memcpy(M + LOW_ENV, env, envlen);
    /* after the environment: a word count and the program's full path */
    WR16(LOW_ENV + (uint32_t)envlen, 1u);
    dos_canon(progpath, canon, sizeof canon);
    for (i = 0; canon[i]; i++) M[LOW_ENV + envlen + 2 + i] = (uint8_t)canon[i];
    M[LOW_ENV + envlen + 2 + i] = 0;
    snprintf(g_progpath, sizeof g_progpath, "%s", canon);
    g_dos.dta = psp + 0x80;
}

/* ---- handles ---------------------------------------------------------------------------------------------- */
static int alloc_handle(VFile *f)
{
    int i;
    for (i = 5; i < MAX_HANDLES; i++) {
        if (!g_dos.files[i].used) {
            g_dos.files[i].used = 1;
            g_dos.files[i].f = f;
            g_dos.files[i].device = 0;
            return i;
        }
    }
    return -1;
}

static void fail(uint16_t code)
{
    R.eax = (R.eax & 0xFFFF0000u) | code;
    R.cf = 1;
}

static void ok(void)
{
    R.cf = 0;
}

static void set_ax(uint32_t v)
{
    R.eax = (R.eax & 0xFFFF0000u) | (v & 0xFFFFu);
}

static int is_nul_device(const char *path)
{
    const char *b = strrchr(path, '\\');
    char name[16];
    size_t k;
    b = b ? b + 1 : path;
    for (k = 0; b[k] && b[k] != ':' && b[k] != '.' && k < 15; k++) name[k] = (char)toupper((unsigned char)b[k]);
    name[k] = '\0';
    return strcmp(name, "NUL") == 0 || strcmp(name, "CON") == 0;
}

static void op_open(int create, int mode)
{
    char path[300], host[1024];
    VFile *f = NULL;
    int h, cow = 0;
    read_guest_str(R.edx, path, sizeof path);
    if (is_nul_device(path)) {
        h = alloc_handle(NULL);
        g_dos.files[h].device = 1;
        set_ax((uint32_t)h);
        ok();
        return;
    }
    if (create) {
        dos_resolve_write(path, host, sizeof host);
        f = vfs_open(host, "w+b");
    } else {
        int acc = mode & 3;
        if (acc == 0) {
            if (dos_resolve_read(path, host, sizeof host)) f = vfs_open(host, "rb");
        } else {
            /* writing an original file: it is read from the original until the first write copies it into the
             * overlay (the games open many data files for writing and only read them) */
            int have = dos_resolve_read(path, host, sizeof host);
            char ov[1024];
            overlay_path(path, ov, sizeof ov);
            if (have && strcmp(host, ov) != 0) {
                f = vfs_open(host, "rb");
                cow = 1;
            } else if (have) {
                f = vfs_open(host, "r+b");
            }
        }
    }
    rt_trace("dos %s \"%s\" -> %s", create ? "create" : "open", path, f ? host : "(not found)");
    if (f == NULL) {
        fail(2);
        return;
    }
    h = alloc_handle(f);
    if (h < 0) {
        vfs_close(f);
        fail(4);
        return;
    }
    if (cow) {
        char canon[300];
        dos_canon(path, canon, sizeof canon); /* the full path: the current directory may change */
        g_dos.files[h].cow = strdup(canon);
    }
    set_ax((uint32_t)h);
    ok();
}

/* before the first write to a file still read from the original: copy it into the overlay and continue there */
static void cow_copy(DosFile *d)
{
    char src[1024], dst[1024];
    long pos;
    VFile *b;
    if (d->cow == NULL) return;
    pos = vfs_seek(d->f, 0, SEEK_CUR);
    dos_resolve_read(d->cow, src, sizeof src);
    dos_resolve_write(d->cow, dst, sizeof dst);
    b = vfs_open(dst, "wb");
    if (b != NULL) {
        char buf[8192];
        size_t k;
        vfs_seek(d->f, 0, SEEK_SET);
        while ((k = vfs_read(d->f, buf, sizeof buf)) > 0) vfs_write(b, buf, k);
        vfs_close(b);
        b = vfs_open(dst, "r+b");
    }
    rt_trace("dos copy on write %s -> %s", d->cow, b ? dst : "(failed)");
    if (b != NULL) {
        vfs_close(d->f);
        d->f = b;
        vfs_seek(d->f, pos, SEEK_SET);
    }
    free(d->cow);
    d->cow = NULL;
}

static DosFile *handle(uint32_t h)
{
    if (h >= MAX_HANDLES || !g_dos.files[h].used) return NULL;
    return &g_dos.files[h];
}

/* ---- find first / next ------------------------------------------------------------------------------------ */
static int wild_match(const char *pat, const char *name)
{
    /* DOS 8.3 matching: split both into name and extension */
    char pn[16] = "", pe[8] = "", nn[16] = "", ne[8] = "";
    const char *d;
    int i;
    d = strchr(pat, '.');
    if (d) {
        snprintf(pn, sizeof pn, "%.*s", (int)(d - pat), pat);
        snprintf(pe, sizeof pe, "%s", d + 1);
    } else {
        snprintf(pn, sizeof pn, "%s", pat);
    }
    d = strchr(name, '.');
    if (d) {
        snprintf(nn, sizeof nn, "%.*s", (int)(d - name), name);
        snprintf(ne, sizeof ne, "%s", d + 1);
    } else {
        snprintf(nn, sizeof nn, "%s", name);
    }
    for (i = 0; i < 2; i++) {
        const char *p = i ? pe : pn, *q = i ? ne : nn;
        if (!d && i && strcmp(pe, "*") != 0 && pe[0] && !(pe[0] == '?' || pe[0] == '*')) return 0;
        while (*p) {
            if (*p == '*') break;
            if (*p != '?' && toupper((unsigned char)*p) != toupper((unsigned char)*q)) return 0;
            if (*q) q++;
            p++;
        }
        if (!*p && *q) return 0;
    }
    return 1;
}

/* the DTA keeps the search: bytes 0..20 reserved (we store an index and the pattern's hash slot) */
#define FIND_SLOTS 8
static struct {
    char dir[1024], dir2[1024];
    char pat[64];
    int attr, pos, used;
} g_find[FIND_SLOTS];

static int find_fill(int slot)
{
    /* enumerate the overlay and the base directory, return the pos-th match */
    HostDirEntry e;
    int idx = 0, pass;
    for (pass = 0; pass < 2; pass++) {
        const char *d = pass ? g_find[slot].dir2 : g_find[slot].dir;
        void *h;
        if (!d[0] || (pass == 1 && strcmp(g_find[slot].dir, g_find[slot].dir2) == 0)) continue;
        h = vfs_dir_open(d);
        if (h == NULL) continue;
        while (vfs_dir_next(h, &e)) {
            char up[260];
            if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
            if (e.is_dir && !(g_find[slot].attr & 0x10)) continue;
            if (strlen(e.name) > 12) continue;
            snprintf(up, sizeof up, "%s", e.name);
            str_upper(up);
            if (!wild_match(g_find[slot].pat, up)) continue;
            if (idx++ == g_find[slot].pos) {
                uint32_t dta = g_dos.dta;
                int k;
                vfs_dir_close(h);
                g_find[slot].pos++;
                M[dta + 0x15] = (uint8_t)(e.is_dir ? 0x10 : 0x20);
                WR16(dta + 0x16, e.dos_time);
                WR16(dta + 0x18, e.dos_date);
                WR32(dta + 0x1A, e.size);
                for (k = 0; up[k] && k < 12; k++) M[dta + 0x1E + k] = (uint8_t)up[k];
                M[dta + 0x1E + k] = 0;
                return 1;
            }
        }
        vfs_dir_close(h);
    }
    return 0;
}

static void op_findfirst(void)
{
    char path[300], canon[300], base[1024], overlay[1024], *slash;
    int slot;
    const char *rel;
    read_guest_str(R.edx, path, sizeof path);
    dos_canon(path, canon, sizeof canon);
    slash = strrchr(canon, '\\');
    slot = (int)((g_dos.dta >> 4) % FIND_SLOTS);
    memset(&g_find[slot], 0, sizeof g_find[slot]);
    snprintf(g_find[slot].pat, sizeof g_find[slot].pat, "%s", slash + 1);
    *slash = '\0';
    rel = drive_roots(canon, base, sizeof base, overlay, sizeof overlay);
    host_path(overlay, rel, g_find[slot].dir, sizeof g_find[slot].dir);
    if (base[0]) host_path(base, canon + 3, g_find[slot].dir2, sizeof g_find[slot].dir2);
    g_find[slot].attr = (int)(R.ecx & 0xFFFF);
    g_find[slot].used = 1;
    M[g_dos.dta] = (uint8_t)slot;
    rt_trace("dos findfirst %s", path);
    if (find_fill(slot))
        ok();
    else
        fail(18);
}

static void op_findnext(void)
{
    int slot = M[g_dos.dta] % FIND_SLOTS;
    if (g_find[slot].used && find_fill(slot))
        ok();
    else
        fail(18);
}

/* ---- exec ------------------------------------------------------------------------------------------------- */
static int exec_program(const char *prog, const char *args)
{
    char canon[300], tryp[300];
    const RtModuleDef *def;
    snprintf(tryp, sizeof tryp, "%s", prog);
    if (strchr(tryp, '.') == NULL) snprintf(tryp + strlen(tryp), sizeof tryp - strlen(tryp), ".EXE");
    dos_canon(tryp, canon, sizeof canon);
    def = game_find_module(canon);
    if (def == NULL) {
        rt_log("exec: no recompiled program for %s", canon);
        return -1;
    }
    return rt_run_module(def, canon, args);
}

static int run_command(const char *line)
{
    char cmd[300], *p, *q, *redir;
    snprintf(cmd, sizeof cmd, "%s", line);
    redir = strchr(cmd, '>');
    if (redir) *redir = '\0';
    for (p = cmd; *p == ' '; p++) {
    }
    q = p + strlen(p);
    while (q > p && (q[-1] == ' ' || q[-1] == '\r')) *--q = '\0';
    if (!strncmp(p, "cd ", 3) || !strncmp(p, "CD ", 3)) {
        char canon[300];
        dos_canon(p + 3, canon, sizeof canon);
        g_dos.drive = canon[0] - 'A';
        snprintf(g_dos.cwd[g_dos.drive], sizeof g_dos.cwd[0], "%s", canon + 3);
        return 0;
    }
    {
        char prog[300], *sp = strchr(p, ' ');
        if (sp) {
            snprintf(prog, sizeof prog, "%.*s", (int)(sp - p), p);
            return exec_program(prog, sp);
        }
        return exec_program(p, "");
    }
}

static void op_exec(void)
{
    char prog[300], tail[200], base[64], *b;
    uint32_t pb = R.es_base + R.ebx, tail_addr;
    int len, rc, i;
    read_guest_str(R.edx, prog, sizeof prog);
    /* DOS/4GW's parameter block: environment offset (dword) and selector (word), command tail offset (dword)
     * and selector (word) */
    tail_addr = rt_sel_base(RD16(pb + 10)) + RD32(pb + 6);
    len = RD8(tail_addr);
    for (i = 0; i < len && i < (int)sizeof tail - 1; i++) tail[i] = (char)RD8(tail_addr + 1u + (uint32_t)i);
    tail[i] = '\0';
    rt_log("exec \"%s\" \"%s\"", prog, tail);
    b = strrchr(prog, '\\');
    if (!b) b = strrchr(prog, '/');
    snprintf(base, sizeof base, "%s", b ? b + 1 : prog);
    str_upper(base);
    if (strcmp(base, "COMMAND.COM") == 0 || strcmp(base, "CMD.EXE") == 0) {
        char *c = tail;
        while (*c == ' ') c++;
        if ((c[0] == '/' || c[0] == '-') && toupper((unsigned char)c[1]) == 'C') c += 2;
        rc = run_command(c);
    } else {
        rc = exec_program(prog, tail);
    }
    if (rc < 0) {
        fail(2);
        return;
    }
    rt_exit_code = rc;
    ok();
}

/* ---- int 21h -------------------------------------------------------------------------------------------- */
static uint32_t g_last_exit;

void dos_int21(void)
{
    uint8_t ah = (uint8_t)(R.eax >> 8), al = (uint8_t)R.eax;
    rt_trace("int21 ah=%02x al=%02x ebx=%08x ecx=%08x edx=%08x", ah, al, R.ebx, R.ecx, R.edx);
    switch (ah) {
    case 0xFF:
        if (al == 0x00 && (uint16_t)R.edx == 0x0078u) { /* DOS/4GW's presence check: AL != 0 */
            R.eax = 0xFFFF3447u;
            return;
        }
        fail(1);
        return;
    case 0x02: {
        char s[2] = {(char)R.edx, 0};
        (void)s;
        ok();
        return;
    }
    case 0x09: {
        char s[512];
        size_t i;
        for (i = 0; i < sizeof s - 1; i++) {
            s[i] = (char)RD8(R.edx + (uint32_t)i);
            if (s[i] == '$') break;
        }
        s[i] = '\0';
        rt_log("dos print: %s", s);
        return;
    }
    case 0x06:
    case 0x07:
    case 0x08: {
        uint16_t k;
        if (ah == 0x06 && (uint8_t)R.edx != 0xFF) return;
        while (!kbd_bios_peek(&k)) {
            if (ah == 0x06) {
                R.zf = 1;
                return;
            }
            rt_poll();
            host_sleep_us(1000);
        }
        k = kbd_bios_get();
        R.zf = 0;
        R.eax = (R.eax & 0xFFFFFF00u) | (k & 0xFFu);
        return;
    }
    case 0x0B: {
        uint16_t k;
        R.eax = (R.eax & 0xFFFFFF00u) | (kbd_bios_peek(&k) ? 0xFFu : 0u);
        return;
    }
    case 0x0C:
        while (1) {
            uint16_t k;
            if (!kbd_bios_peek(&k)) break;
            kbd_bios_get();
        }
        if (al == 0x01 || al == 0x06 || al == 0x07 || al == 0x08) {
            R.eax = (R.eax & 0xFFFF00FFu) | ((uint32_t)al << 8);
            dos_int21();
        }
        return;
    case 0x0E:
        g_dos.drive = (int)(R.edx & 0xFF) % 26;
        R.eax = (R.eax & 0xFFFFFF00u) | 26u;
        return;
    case 0x19:
        R.eax = (R.eax & 0xFFFFFF00u) | (uint32_t)g_dos.drive;
        return;
    case 0x1A:
        g_dos.dta = R.edx;
        return;
    case 0x2F:
        rt_load_seg(SEG_ES, SEL_DATA);
        R.ebx = g_dos.dta;
        return;
    case 0x25:
        dpmi_set_pm_vector(al, R.edx);
        return;
    case 0x35:
        rt_load_seg(SEG_ES, SEL_CODE);
        R.ebx = dpmi_pm_vector_get(al);
        return;
    case 0x2A: {
        struct tm tmv, *tm = &tmv;
        rt_wall_time(&tmv);
        R.ecx = (R.ecx & 0xFFFF0000u) | (uint32_t)(tm->tm_year + 1900);
        R.edx = (R.edx & 0xFFFF0000u) | ((uint32_t)(tm->tm_mon + 1) << 8) | (uint32_t)tm->tm_mday;
        R.eax = (R.eax & 0xFFFFFF00u) | (uint32_t)tm->tm_wday;
        return;
    }
    case 0x2C: {
        /* as DOS does it: from the BIOS tick count (18.2 Hz), so the hundredths move in steps of about 5.5.
         * The intros time their frames with this call; a finer clock makes them run fast. */
        static uint32_t last_ticks;
        static int repeats;
        uint32_t cs;
        if (RD32(0x46C) == last_ticks) {
            rt_clock_wait(++repeats);
        } else {
            repeats = 0;
        }
        last_ticks = RD32(0x46C);
        cs = (uint32_t)((uint64_t)last_ticks * 6553600u / 1193180u);
        R.ecx = (R.ecx & 0xFFFF0000u) | ((cs / 360000u) << 8) | (cs / 6000u % 60u);
        R.edx = (R.edx & 0xFFFF0000u) | ((cs / 100u % 60u) << 8) | (cs % 100u);
        return;
    }
    case 0x30:
        if (R.eax == 0xFF00u) {
            /* DOS/4GW's presence check is int 21h AX=FF00h with DX=0078h; unknown, return as DOS does */
        }
        R.eax = (R.eax & 0xFFFF0000u) | 0x0005u; /* DOS 5.0 */
        R.ebx = (R.ebx & 0xFFFF0000u) | 0xFF00u;
        R.ecx &= 0xFFFF0000u;
        return;
    case 0x33:
        if (al == 0x00) R.edx &= 0xFFFFFF00u;
        return;
    case 0x36: /* free space: 32 KiB clusters, plenty */
        set_ax(64);
        R.ebx = (R.ebx & 0xFFFF0000u) | 0x7FFFu;
        R.ecx = (R.ecx & 0xFFFF0000u) | 512u;
        R.edx = (R.edx & 0xFFFF0000u) | 0xFFFFu;
        return;
    case 0x39: {
        char path[300], host[1024];
        read_guest_str(R.edx, path, sizeof path);
        dos_resolve_write(path, host, sizeof host);
        host_mkdir(host);
        ok();
        return;
    }
    case 0x3A:
        ok();
        return;
    case 0x3B: {
        char path[300], canon[300], host[1024];
        read_guest_str(R.edx, path, sizeof path);
        dos_canon(path, canon, sizeof canon);
        if (!dos_resolve_read(canon, host, sizeof host) || !vfs_is_dir(host)) {
            if (strlen(canon) > 3) {
                fail(3);
                return;
            }
        }
        snprintf(g_dos.cwd[canon[0] - 'A'], sizeof g_dos.cwd[0], "%s", canon + 3);
        rt_trace("dos chdir %s", canon);
        ok();
        return;
    }
    case 0x3C:
        op_open(1, 2);
        return;
    case 0x3D:
        op_open(0, al);
        return;
    case 0x3E: {
        DosFile *f = handle(R.ebx & 0xFFFF);
        if (f == NULL) {
            fail(6);
            return;
        }
        if ((R.ebx & 0xFFFF) >= 5) {
            if (f->f) vfs_close(f->f);
            free(f->cow);
            memset(f, 0, sizeof *f);
        }
        ok();
        return;
    }
    case 0x3F: {
        DosFile *f = handle(R.ebx & 0xFFFF);
        uint32_t n = R.ecx, got = 0;
        if (f == NULL) {
            fail(6);
            return;
        }
        if (f->f) {
            /* read straight into guest memory unless it touches the video window */
            uint32_t dst = R.edx;
            if (!RT_IS_VGA(dst) && !RT_IS_VGA(dst + n) && dst + n <= RT_MEM_SIZE)
                got = (uint32_t)vfs_read(f->f, M + dst, n);
            else {
                uint32_t i;
                for (i = 0; i < n; i++) {
                    int c = vfs_getc(f->f);
                    if (c < 0) break;
                    WR8(dst + i, (uint8_t)c);
                    got++;
                }
            }
        }
        R.eax = got;
        ok();
        return;
    }
    case 0x40: {
        DosFile *f = handle(R.ebx & 0xFFFF);
        uint32_t n = R.ecx;
        if (f == NULL) {
            fail(6);
            return;
        }
        if (f->f) {
            cow_copy(f);
            if (n == 0) { /* truncate at the current position */
                ok();
                R.eax = 0;
                return;
            }
            R.eax = (uint32_t)vfs_write(f->f, M + R.edx, n);
        } else {
            if ((R.ebx & 0xFFFF) == 1 || (R.ebx & 0xFFFF) == 2) {
                char buf[256];
                uint32_t k = n < 255 ? n : 255;
                memcpy(buf, M + R.edx, k);
                buf[k] = '\0';
                rt_log("dos out: %s", buf);
            }
            R.eax = n;
        }
        ok();
        return;
    }
    case 0x41: {
        char path[300], host[1024], canon[300], ov[1024], base[1024];
        const char *rel;
        read_guest_str(R.edx, path, sizeof path);
        dos_canon(path, canon, sizeof canon);
        rel = drive_roots(canon, base, sizeof base, ov, sizeof ov);
        if (host_path(ov, rel, host, sizeof host) && remove(host) == 0)
            ok();
        else
            fail(2);
        return;
    }
    case 0x42: {
        DosFile *f = handle(R.ebx & 0xFFFF);
        long off = (long)(int32_t)(((R.ecx & 0xFFFF) << 16) | (R.edx & 0xFFFF));
        long pos;
        if (f == NULL || f->f == NULL) {
            if (f) {
                R.eax = 0;
                R.edx &= 0xFFFF0000u;
                ok();
                return;
            }
            fail(6);
            return;
        }
        pos = vfs_seek(f->f, off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END);
        set_ax((uint32_t)pos);
        R.edx = (R.edx & 0xFFFF0000u) | (((uint32_t)pos >> 16) & 0xFFFFu);
        ok();
        return;
    }
    case 0x43: {
        char path[300], host[1024];
        read_guest_str(R.edx, path, sizeof path);
        if (al == 0) {
            if (!dos_resolve_read(path, host, sizeof host)) {
                fail(2);
                return;
            }
            R.ecx = (R.ecx & 0xFFFF0000u) | (vfs_is_dir(host) ? 0x10u : 0x20u);
        }
        ok();
        return;
    }
    case 0x44: {
        uint32_t h = R.ebx & 0xFFFF;
        DosFile *f = handle(h);
        if (al == 0x00) {
            if (f == NULL) {
                fail(6);
                return;
            }
            R.edx = (R.edx & 0xFFFF0000u) | (f->device ? (h == 0 ? 0x81u : h == 1 ? 0x82u : 0x80u) : 0x0000u);
            ok();
            return;
        }
        if (al == 0x08) { /* removable? drive in BL: C fixed, D CD-ROM (removable) */
            set_ax((R.ebx & 0xFF) == 4 ? 0u : 1u);
            ok();
            return;
        }
        if (al == 0x09) {
            R.edx = (R.edx & 0xFFFF0000u) | ((R.ebx & 0xFF) == 4 ? 0x1000u : 0u);
            ok();
            return;
        }
        ok();
        return;
    }
    case 0x45: {
        DosFile *f = handle(R.ebx & 0xFFFF);
        int h;
        if (f == NULL) {
            fail(6);
            return;
        }
        h = alloc_handle(NULL);
        g_dos.files[h].device = 1;
        set_ax((uint32_t)h);
        ok();
        return;
    }
    case 0x47: {
        int d = (int)(R.edx & 0xFF);
        const char *c;
        size_t i;
        d = d == 0 ? g_dos.drive : d - 1;
        c = g_dos.cwd[d];
        for (i = 0; c[i]; i++) WR8(R.esi + (uint32_t)i, (uint8_t)c[i]);
        WR8(R.esi + (uint32_t)i, 0);
        ok();
        return;
    }
    case 0x48: {
        uint32_t largest = 0, seg = dpmi_dos_alloc(R.ebx & 0xFFFF, &largest);
        if (seg == 0) {
            R.ebx = (R.ebx & 0xFFFF0000u) | largest;
            fail(8);
            return;
        }
        set_ax(seg);
        ok();
        return;
    }
    case 0x49:
    case 0x4A:
        ok();
        return;
    case 0x37: /* switch character */
        if (al == 0x00) R.edx = (R.edx & 0xFFFFFF00u) | '/';
        R.eax &= 0xFFFFFF00u;
        return;
    case 0x4B:
        op_exec();
        return;
    case 0x4C:
        g_last_exit = al;
        rt_exit_process(al);
    case 0x4D:
        set_ax((uint32_t)rt_exit_code & 0xFF);
        ok();
        return;
    case 0x4E:
        op_findfirst();
        return;
    case 0x4F:
        op_findnext();
        return;
    case 0x56: {
        char a[300], b[300], ha[1024], hb[1024];
        read_guest_str(R.edx, a, sizeof a);
        read_guest_str(R.es_base + R.edi, b, sizeof b);
        if (!dos_resolve_read(a, ha, sizeof ha)) {
            fail(2);
            return;
        }
        dos_resolve_write(b, hb, sizeof hb);
        if (rename(ha, hb) == 0)
            ok();
        else
            fail(5);
        return;
    }
    case 0x57:
        R.ecx &= 0xFFFF0000u;
        R.edx = (R.edx & 0xFFFF0000u) | 0x1B21u; /* 1993-09-01 */
        ok();
        return;
    case 0x59:
        set_ax(2);
        ok();
        return;
    case 0x62:
        R.ebx = (R.ebx & 0xFFFF0000u) | SEL_PSP;
        return;
    default:
        rt_log("int 21h: unhandled AH=%02x AL=%02x", ah, al);
        fail(1);
        return;
    }
}

void dos_int2f(void)
{
    uint16_t ax = (uint16_t)R.eax;
    if (ax == 0x1500) { /* MSCDEX: one CD-ROM drive, D: */
        R.ebx = (R.ebx & 0xFFFF0000u) | 1u;
        R.ecx = (R.ecx & 0xFFFF0000u) | 3u;
        return;
    }
    if (ax == 0x150B) { /* is drive CX a CD-ROM drive */
        set_ax((R.ecx & 0xFFFF) == 3 ? 0xFFFFu : 0u);
        R.ebx = (R.ebx & 0xFFFF0000u) | 0xADADu;
        return;
    }
    if (ax == 0x1686) { /* in protected mode */
        set_ax(0);
        return;
    }
    rt_trace("int 2Fh AX=%04x", ax);
}
