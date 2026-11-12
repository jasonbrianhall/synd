/* game.h - the Syndicate Plus program: which recompiled DOS programs it contains and how it starts. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameOptions {
    const char *game_dir;  /* folder with SYND.EXE, DATA, SYNDICAT, DATADISK */
    const char *user_dir;  /* writable folder (saves, setup, files the game writes) */
    const char *start;     /* NULL: the original launcher (SYND.EXE); else a DOS command line to run first */
    int verbose;
    int fixed_clock;       /* the guest's date and time start at a fixed point (repeatable test runs) */
} GameOptions;

/* run the game on the calling thread until it exits or the host asks to quit; returns the exit code */
int game_main(const GameOptions *opt);

/* check that `dir` holds the original game files this program was built from; returns NULL when it does,
 * else a sentence describing what is missing or different */
const char *game_check_dir(const char *dir);

#ifdef __cplusplus
}
#endif
