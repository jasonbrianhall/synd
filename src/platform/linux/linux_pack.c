/* linux_pack.c - the original game files built into a private Linux build (SYN_EMBED_GAME): the pack that
 * tools/pack_game.py writes, linked into the program's read-only data (the counterpart of the GAMEPAK resource
 * on Windows and Contents/Resources/syndgame.pak on macOS).  SYN_PAK_PATH is set by CMakeLists.txt. */
#ifndef SYN_PAK_PATH
#error "SYN_PAK_PATH must name the pack file"
#endif

__asm__(".section .rodata\n"
        ".global syn_pack_start\n"
        ".global syn_pack_end\n"
        ".balign 16\n"
        "syn_pack_start:\n"
        ".incbin \"" SYN_PAK_PATH "\"\n"
        "syn_pack_end:\n"
        ".previous\n");
