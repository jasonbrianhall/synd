"""Regenerate the C code of every recompiled program into gen/ (see tools/recomp.py)."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "_references", "dos")
MODULES = [
    ("SYND.EXE", "synd"),
    ("SYNDICAT/MAIN.EXE", "syn_main"),
    ("SYNDICAT/INTRO.EXE", "intro"),
    ("DATADISK/MAIN.EXE", "ar_main"),
    # the Spanish and German programs (SINTRO and GINTRO are the same files in both games)
    ("SYNDICAT/SMAIN.EXE", "syn_smain"),
    ("SYNDICAT/GMAIN.EXE", "syn_gmain"),
    ("DATADISK/SMAIN.EXE", "ar_smain"),
    ("DATADISK/GMAIN.EXE", "ar_gmain"),
    ("SYNDICAT/SINTRO.EXE", "sintro"),
    ("SYNDICAT/GINTRO.EXE", "gintro"),
    # AIL/32 sound drivers (the same files in both games), loaded and relocated by the game at run time
    ("SYNDICAT/DATA/GAMEDG.DLL", "drv_dg", "--reloc"),
    ("SYNDICAT/DATA/GAMEFM.DLL", "drv_fm", "--reloc"),
]

def find(rel):
    """the real path of `rel` under REF, ignoring case (a Linux file system is case-sensitive; installers and
    innoextract may give lowercase names)"""
    path = REF
    for part in rel.split("/"):
        exact = os.path.join(path, part)
        if os.path.exists(exact):
            path = exact
            continue
        try:
            match = next((n for n in sorted(os.listdir(path)) if n.lower() == part.lower()), None)
        except OSError:
            match = None
        if match is None:
            sys.exit(f"{os.path.join(REF, rel)} was not found (in any case): is the whole Syndicate Plus "
                     f"installation in _references/dos?")
        path = os.path.join(path, match)
    return path


if __name__ == "__main__":
    only = set(sys.argv[1:])
    for exe, prefix, *extra in MODULES:
        if only and prefix not in only:
            continue
        # not isolated (-I): Capstone is often installed with "pip install --user", which -I would not see
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "recomp.py"),
                               find(exe), prefix, os.path.join(ROOT, "gen", prefix)] + extra)
