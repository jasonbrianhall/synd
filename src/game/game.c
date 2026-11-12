/* game.c - the recompiled programs of Syndicate Plus and the start of the game.
 *
 * The CD's launcher SYND.EXE (language, sound card, Syndicate or American Revolt) runs the chosen game's
 * MAIN.EXE from SYNDICAT\ or DATADISK\, which runs INTRO.EXE for the introduction.  Each of these DOS/4GW
 * programs is recompiled (gen/<module>); a DOS EXEC of one of them runs its recompiled code. */
#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../rt/rt.h"
#include "../rt/vfs.h"

extern const RtModuleDef synd_module;       /* SYND.EXE: the launcher */
extern const RtModuleDef syn_main_module;   /* SYNDICAT\MAIN.EXE: Syndicate (English, French, Italian) */
extern const RtModuleDef ar_main_module;    /* DATADISK\MAIN.EXE: American Revolt */
extern const RtModuleDef intro_module;      /* INTRO.EXE (both games) */
extern const RtModuleDef syn_smain_module;  /* SYNDICAT\SMAIN.EXE: Syndicate (Spanish) */
extern const RtModuleDef syn_gmain_module;  /* SYNDICAT\GMAIN.EXE: Syndicate (German) */
extern const RtModuleDef ar_smain_module;   /* DATADISK\SMAIN.EXE: American Revolt (Spanish) */
extern const RtModuleDef ar_gmain_module;   /* DATADISK\GMAIN.EXE: American Revolt (German) */
extern const RtModuleDef sintro_module;     /* SINTRO.EXE (both games, Spanish) */
extern const RtModuleDef gintro_module;     /* GINTRO.EXE (both games, German) */
extern const RtModuleDef drv_dg_module;     /* DATA\GAMEDG.DLL: AIL/32 digital sound driver (Sound Blaster) */
extern const RtModuleDef drv_fm_module;     /* DATA\GAMEFM.DLL: AIL/32 music driver (OPL2/OPL3) */

typedef struct ModuleMap {
    const char *dos_path;
    const RtModuleDef *def;
} ModuleMap;

static const ModuleMap MODULES[] = {
    {"D:\\SYND.EXE", &synd_module},
    {"D:\\SYNDICAT\\MAIN.EXE", &syn_main_module},
    {"D:\\SYNDICAT\\INTRO.EXE", &intro_module},
    {"D:\\DATADISK\\MAIN.EXE", &ar_main_module},
    {"D:\\DATADISK\\INTRO.EXE", &intro_module},
    {"D:\\SYNDICAT\\SMAIN.EXE", &syn_smain_module},
    {"D:\\SYNDICAT\\GMAIN.EXE", &syn_gmain_module},
    {"D:\\DATADISK\\SMAIN.EXE", &ar_smain_module},
    {"D:\\DATADISK\\GMAIN.EXE", &ar_gmain_module},
    {"D:\\SYNDICAT\\SINTRO.EXE", &sintro_module},
    {"D:\\DATADISK\\SINTRO.EXE", &sintro_module},
    {"D:\\SYNDICAT\\GINTRO.EXE", &gintro_module},
    {"D:\\DATADISK\\GINTRO.EXE", &gintro_module},
};

const RtModuleDef *game_find_module(const char *dos_path)
{
    size_t i;
    for (i = 0; i < sizeof MODULES / sizeof MODULES[0]; i++)
        if (strcmp(MODULES[i].dos_path, dos_path) == 0) return MODULES[i].def;
    return NULL;
}

const char *game_check_dir(const char *dir)
{
    static char msg[512];
    char path[1100];
    VFile *f;
    snprintf(path, sizeof path, "%s/SYND.EXE", dir);
    f = vfs_open(path, "rb");
    if (f == NULL) {
        snprintf(msg, sizeof msg, "SYND.EXE was not found in %s.", dir);
        return msg;
    }
    vfs_close(f);
    return NULL;
}

int game_main(const GameOptions *opt)
{
    int rc;
    rt_verbose = opt->verbose;
    rt_fixed_clock = opt->fixed_clock;
    /* without a game folder: the original files built into the program, when it has them */
    if (opt->game_dir == NULL || opt->game_dir[0] == '\0') {
        size_t size = 0;
        const uint8_t *pack = host_game_pack(&size);
        if (pack == NULL || !vfs_set_pack(pack, size)) {
            rt_log("no game folder given and no game files built in");
            return 1;
        }
        snprintf(rt_game_dir, sizeof rt_game_dir, "%s", VFS_PACK_ROOT);
        rt_log("game files: built in (%u KB)", (unsigned)(size / 1024));
    } else {
        snprintf(rt_game_dir, sizeof rt_game_dir, "%s", opt->game_dir);
    }
    snprintf(rt_user_dir, sizeof rt_user_dir, "%s", opt->user_dir);
    host_mkdir(rt_user_dir);
    dos_init();
    vga_init();
    pit_init();
    kbd_init();
    audio_init();
    rt_register_reloc_module(&drv_dg_module);
    rt_register_reloc_module(&drv_fm_module);
    if (opt->start != NULL && opt->start[0]) {
        /* "SYNDICAT\MAIN.EXE /args": run a program directly, in its directory */
        char prog[256], canon[300], *sp;
        const RtModuleDef *def;
        snprintf(prog, sizeof prog, "%s", opt->start);
        sp = strchr(prog, ' ');
        if (sp) *sp = '\0';
        snprintf(canon, sizeof canon, "D:\\%s", prog);
        {
            char *c;
            for (c = canon; *c; c++) *c = (char)(*c == '/' ? '\\' : toupper((unsigned char)*c));
        }
        def = game_find_module(canon);
        if (def == NULL) {
            rt_log("no recompiled program %s", canon);
            return 1;
        }
        {
            /* the current directory is the program's folder, as SYND.EXE leaves it */
            extern void dos_chdir_host(const char *canon_dir);
            char dir[300], *bs;
            snprintf(dir, sizeof dir, "%s", canon);
            bs = strrchr(dir, '\\');
            if (bs) *bs = '\0';
            dos_chdir_host(dir);
        }
        rc = rt_run_module(def, canon, sp ? opt->start + (sp - prog) : "");
    } else {
        rc = rt_run_module(&synd_module, "D:\\SYND.EXE", "");
    }
    return rc;
}
