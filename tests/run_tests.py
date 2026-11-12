"""Automated game tests: play the games headless and check what they show, write and play.

    python tests/run_tests.py            all tests
    python tests/run_tests.py --quick    without the slow ones (the full intro)
    python tests/run_tests.py save_load  only the named tests
    python tests/run_tests.py --list

Every run uses the virtual clock (see frames.run), so a run repeats exactly and inputs land on the same frame
each time.  The frames, logs and user folder of each test go to out/tests/<test>/; a failed check names the
frame to look at.  Needs build/synd_headless(.exe) and the game files in _references/dos (the built-in-files
test also needs build/syndgame.pak, which the private build writes).

Mouse positions in inputs are 0..65535 across the screen (x;y), whatever the game's resolution.
"""
import concurrent.futures
import math
import os
import shutil
import struct
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frames  # noqa: E402

OUT = os.path.join(frames.ROOT, 'out', 'tests')
PACK = os.path.join(frames.ROOT, 'build', 'syndgame.pak')

SYN = r'SYNDICAT\MAIN.EXE /c0 /s'
SKIP_INTRO = ['3000:esc', '7000:esc']                    # the intro, then the title screen

# Syndicate, from the main menu to a mission with agent 3 selected (about 27 s in)
TO_MISSION = SKIP_INTRO + [
    '10000:move@29500;28800', '11000:click@29500;28800',   # main menu: begin mission -> world map
    '14000:click@8500;58500',                               # world map: brief
    '17000:click@8500;58700',                               # brief: accept -> team selection
    '20000:click@8500;58700',                               # team selection: accept -> the mission
    '26000:click@6550;18000',                               # select agent 3
]


class Failure(Exception):
    pass


def check(cond, text):
    if not cond:
        raise Failure(text)


TESTS = []


def test(slow=False):
    def wrap(fn):
        TESTS.append((fn.__name__, fn, slow))
        return fn
    return wrap


class Ctx:
    """one test's folder: runs go to subfolders of it, each with its own frames and log"""

    def __init__(self, name):
        self.dir = os.path.join(OUT, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.user = os.path.join(self.dir, 'user')

    def run(self, name, seconds, start=None, inputs=(), every=1000, user=None, extra=(), codes=(0,)):
        user = user or self.user
        rc, fr, log = frames.run(start=start, seconds=seconds, inputs=list(inputs),
                                 dump_dir=os.path.join(self.dir, name), dump_every=every, user_dir=user,
                                 extra=extra, log=os.path.join(self.dir, name + '.log'))
        check(rc in codes, '%s: exit code %d (see %s.log)' % (name, rc, name))
        return Run(name, fr, log)


class Run:
    def __init__(self, name, fr, log):
        self.name, self.frames, self.log = name, fr, log

    def ran(self, program):
        return ('run D:\\' + program) in self.log

    def at(self, ms):
        """the first frame dumped at or after `ms`"""
        for t, p in self.frames:
            if t >= ms:
                f = frames.read_bmp(p)
                f.path = p
                return f
        raise Failure('%s: no frame at %d ms (the run has %d frames)' % (self.name, ms, len(self.frames)))

    def last(self):
        return self.at(self.frames[-1][0]) if self.frames else self.at(0)


def colour_mean(f, x0=0.0, y0=0.0, x1=1.0, y1=1.0):
    a, b, c, d = int(x0 * f.w), int(x1 * f.w), int(y0 * f.h), int(y1 * f.h)
    tot = [0, 0, 0]
    for y in range(c, d):
        row = f.pix[y * f.w + a:y * f.w + b]
        for p in row:
            r, g, bl = f.pal[p]
            tot[0] += r
            tot[1] += g
            tot[2] += bl
    n = max(1, (b - a) * (d - c))
    return [t / n for t in tot]


def is_green_screen(f):
    """the menus of Syndicate are green on black"""
    r, g, b = colour_mean(f)
    return g > 20 and g > 1.6 * r and g > 1.6 * b


def is_world_map(f):
    """the world map: countries in orange, blue, white and so on over the green screen"""
    n = 0
    for p in f.pix:
        r, g, b = f.pal[p]
        if r > g + 30 or b > g + 30:
            n += 1
    return n > 0.08 * len(f.pix)


def same_pixels(f1, f2):
    return f1.w == f2.w and f1.h == f2.h and f1.pix == f2.pix and f1.pal == f2.pal


def name(f):
    return os.path.relpath(getattr(f, 'path', '?'), OUT)


# ---- the launcher ----

@test()
def launcher(ctx):
    """SYND.EXE shows the two games"""
    r = ctx.run('first', 6)
    check(r.ran('SYND.EXE'), 'SYND.EXE did not start')
    f = r.last()
    check((f.w, f.h) == (320, 200), 'launcher frame is %dx%d' % (f.w, f.h))
    check(f.colour_count() > 64 and f.mean_brightness() > 15, 'launcher looks empty: ' + name(f))


@test()
def launcher_syndicate(ctx):
    """Enter in the launcher starts Syndicate and its intro"""
    r = ctx.run('first', 8, inputs=['4000:enter'])
    check(r.ran(r'SYNDICAT\MAIN.EXE'), 'SYNDICAT\\MAIN.EXE did not start')
    check(r.ran(r'SYNDICAT\INTRO.EXE'), 'the intro did not start')


@test()
def launcher_revolt(ctx):
    """Right, Enter in the launcher starts American Revolt and its intro"""
    r = ctx.run('first', 8, inputs=['2500:right', '4000:enter'])
    check(r.ran(r'DATADISK\MAIN.EXE'), 'DATADISK\\MAIN.EXE did not start')
    check(r.ran(r'DATADISK\INTRO.EXE'), 'the intro did not start')


def launcher_language(ctx, prefix, lang):
    """the installer writes the program name for the language to SYND\\SYND.INF; the launcher follows it"""
    os.makedirs(os.path.join(ctx.user, 'C', 'SYND'))
    with open(os.path.join(ctx.user, 'C', 'SYND', 'SYND.INF'), 'w') as f:
        f.write(prefix + 'main /c0 /iirq7 /idma1 /iio220')
    r = ctx.run('run', 8, inputs=['2500:right', '4000:enter'])
    check(r.ran('DATADISK\\%sMAIN.EXE' % prefix.upper()), 'American Revolt (%s) did not start' % lang)
    check(r.ran('DATADISK\\%sINTRO.EXE' % prefix.upper()), 'the %s intro did not start' % lang)


@test()
def launcher_german(ctx):
    """with "gmain" in SYND.INF the launcher starts the German programs"""
    launcher_language(ctx, 'g', 'German')


@test()
def launcher_spanish(ctx):
    """with "smain" in SYND.INF the launcher starts the Spanish programs"""
    launcher_language(ctx, 's', 'Spanish')


# ---- the main menus, in every language ----

def main_menus(ctx, folder):
    titles = {}
    for prefix in ('', 'G', 'S'):
        r = ctx.run('menu' + (prefix or 'E'), 14, start='%s\\%sMAIN.EXE /c0 /s' % (folder, prefix),
                    inputs=SKIP_INTRO)
        check(r.ran('%s\\%sINTRO.EXE' % (folder, prefix)), '%s\\%sINTRO.EXE did not start' % (folder, prefix))
        f = r.last()
        check(is_green_screen(f), 'no main menu: ' + name(f))
        titles[prefix] = f.region_signature(0.2, 0.05, 0.8, 0.17)
    check(len(set(titles.values())) == 3, 'the menu titles are not different in the three languages')


@test()
def menus_syndicate(ctx):
    """Syndicate in English, German and Spanish reaches its main menu, each with its own title"""
    main_menus(ctx, 'SYNDICAT')


@test()
def menus_revolt(ctx):
    """American Revolt in English, German and Spanish reaches its main menu"""
    main_menus(ctx, 'DATADISK')


# ---- playing ----

@test()
def mission(ctx):
    """into a mission; an agent walks where it is sent; Esc withdraws to the debriefing; then the world map"""
    walk = TO_MISSION + ['27000:click@36000;42000', '31000:esc', '35000:enter']
    r = ctx.run('first', 40, start=SYN, inputs=walk)
    f0, f1 = r.at(27000), r.at(30000)
    check((f0.w, f0.h) == (640, 480), 'the mission is not on screen at 27 s: ' + name(f0))
    # the agent walks across the middle of the view
    check(f0.region_signature(0.45, 0.4, 0.75, 0.75) != f1.region_signature(0.45, 0.4, 0.75, 0.75),
          'nothing moved in the mission between %s and %s' % (name(f0), name(f1)))
    d = r.at(33500)
    check(is_green_screen(d), 'no debriefing after Esc: ' + name(d))
    m = r.last()
    check(is_world_map(m) and not is_world_map(d), 'no world map after the debriefing: ' + name(m))


@test()
def combat(ctx):
    """agent 3 draws the pistol and fires: the debriefing counts the shots ("0%" instead of "no bullets fired")"""
    calm = ctx.run('calm', 37, start=SYN, inputs=TO_MISSION + ['31000:esc'])
    fire = ctx.run('fire', 37, start=SYN, inputs=TO_MISSION + [
        '28000:click@1640;35500',                               # the pistol
        '29000:move@58368;42600', '29500:rclick@58368;42600', '30500:rclick@58368;42600',
        '31500:rclick@58368;42600', '34000:esc'])
    a, b = calm.last(), fire.last()
    check(is_green_screen(a) and is_green_screen(b), 'no debriefing: %s %s' % (name(a), name(b)))
    acc = (0.5, 0.635, 0.95, 0.685)
    check(a.region_signature(*acc) != b.region_signature(*acc),
          'the hit accuracy line is the same with and without shooting: %s %s' % (name(a), name(b)))


@test()
def save_load(ctx):
    """save a game named ABC, then load it in a new run: the funds stored in the save show up"""
    save = ctx.run('first', 19, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;18000',                 # load/save, slot 1
        '13000:del', '13300:del', '13600:del', '13900:del', '14200:del',   # "EMPTY"
        '14500:a', '14800:b', '15100:c', '15500:enter',
        '16500:click@19600;58700'])                             # SAVE
    path = os.path.join(ctx.user, 'Syndicate', '00.GAM')                   # the game's C:\SYND\SAVE
    check(os.path.exists(path), 'no save file Syndicate/00.GAM')
    data = bytearray(open(path, 'rb').read())
    check(data[:4] == b'ABC\0', 'the save is named %r' % bytes(data[:8]))
    check(struct.unpack_from('<I', data, 0x14)[0] == 30000, 'the funds in the save are not 30000')
    struct.pack_into('<I', data, 0x14, 12345)
    with open(path, 'wb') as f:
        f.write(data)
    team = ['click@29500;28800', 'click@8500;58500', 'click@8500;58700']   # map, brief, accept
    fresh = ctx.run('fresh', 24, start=SYN, user=os.path.join(ctx.dir, 'fresh_user'),
                    inputs=SKIP_INTRO + ['%d:%s' % (t, e) for t, e in zip((10000, 14000, 17000), team)])
    loaded = ctx.run('loaded', 30, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;18000', '13000:click@8500;58700'] +   # slot 1, LOAD
        ['%d:%s' % (t, e) for t, e in zip((16000, 20000, 23000), team)])
    a, b = fresh.last(), loaded.last()
    funds = (0.76, 0.19, 0.97, 0.26)
    check(a.region_signature(0.2, 0.0, 0.8, 0.12) == b.region_signature(0.2, 0.0, 0.8, 0.12),
          'not the team selection in both runs: %s %s' % (name(a), name(b)))
    check(a.region_signature(*funds) != b.region_signature(*funds),
          'the loaded funds look the same as a new game\'s: %s %s' % (name(a), name(b)))


@test()
def save_revolt(ctx):
    """American Revolt saves into a folder of its own, apart from Syndicate's (both name their saves 00.GAM...)"""
    ctx.run('first', 19, start=r'DATADISK\MAIN.EXE /c0 /s', inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;18000', '13000:a', '15500:enter', '16500:click@19600;58700'])
    check(os.path.exists(os.path.join(ctx.user, 'American Revolt', '00.GAM')), 'no save file American Revolt/00.GAM')
    check(not os.path.exists(os.path.join(ctx.user, 'Syndicate')), 'American Revolt wrote into Syndicate/')


# ---- sound, built-in files, repeatability ----

@test()
def audio(ctx):
    """the intro music is not silent"""
    wav = os.path.join(ctx.dir, 'intro.wav')
    ctx.run('first', 12, start=SYN, extra=['--wav', wav])
    with open(wav, 'rb') as f:
        d = f.read()
    check(len(d) > 44 + 4 * 44100 * 5, 'the WAV holds %d bytes' % len(d))
    n = (len(d) - 44) // 2
    s = struct.unpack_from('<%dh' % n, d, 44)
    tail = s[4 * 44100 * 2:]                                    # after the first 4 seconds (stereo)
    rms = math.sqrt(sum(v * v for v in tail[::7]) / max(1, len(tail[::7])))
    check(rms > 300, 'the intro is (nearly) silent: RMS %.0f' % rms)


@test()
def builtin_files(ctx):
    """the game files read from the pack (as Syndicate64.exe reads its built-in ones) give the same game"""
    if not os.path.exists(PACK):
        print('    (no %s: skipped)' % os.path.relpath(PACK, frames.ROOT))
        return
    inputs = TO_MISSION + ['27000:click@36000;42000']
    a = ctx.run('folder', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u1'))
    b = ctx.run('pack', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u2'), extra=['--builtin', PACK])
    check('game files: built in' in b.log, 'the pack was not used')
    fa, fb = a.last(), b.last()
    check(same_pixels(fa, fb), 'the pack run differs from the folder run: %s %s' % (name(fa), name(fb)))


@test()
def repeatable(ctx):
    """two runs with the same input end on the same picture"""
    inputs = TO_MISSION + ['27000:click@36000;42000']
    a = ctx.run('a', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u1'))
    b = ctx.run('b', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u2'))
    check(same_pixels(a.last(), b.last()), 'the two runs differ: %s %s' % (name(a.last()), name(b.last())))


@test(slow=True)
def intro_length(ctx):
    """the Syndicate intro runs its full length, as on a real PC (about 2 minutes 6 seconds)"""
    r = ctx.run('first', 170, start=r'SYNDICAT\INTRO.EXE', every=5000, codes=(255,))   # the intro ends with 255
    tail = [l for l in r.log.splitlines() if l.startswith('exit ')]
    check(tail, 'no exit line in the log')
    ms = int(tail[-1].split()[3])
    check(120000 <= ms <= 140000, 'the intro took %.1f s' % (ms / 1000))


def main():
    args = sys.argv[1:]
    if '--list' in args:
        for n, fn, slow in TESTS:
            print('%-20s %s%s' % (n, (fn.__doc__ or '').strip().splitlines()[0] if fn.__doc__ else '',
                                  ' (slow)' if slow else ''))
        return 0
    quick = '--quick' in args
    names = [a for a in args if not a.startswith('--')]
    todo = [(n, fn) for n, fn, slow in TESTS if (n in names if names else not (quick and slow))]
    unknown = set(names) - {n for n, _, _ in TESTS}
    if unknown:
        print('unknown tests:', ', '.join(sorted(unknown)))
        return 2
    if not os.path.exists(frames.HEADLESS):
        print('%s is missing: build first' % os.path.relpath(frames.HEADLESS, frames.ROOT))
        return 2

    def one(item):
        n, fn = item
        t = time.time()
        try:
            fn(Ctx(n))
            return n, None, time.time() - t
        except Failure as e:
            return n, str(e), time.time() - t
        except Exception:
            return n, traceback.format_exc(), time.time() - t

    t0 = time.time()
    failed = 0
    workers = max(1, min(len(todo), (os.cpu_count() or 4) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for n, err, secs in pool.map(one, todo):
            print('%-20s %s  (%.0f s)' % (n, 'ok' if err is None else 'FAILED', secs))
            if err:
                failed += 1
                print('    ' + err.replace('\n', '\n    '))
    print('%d of %d passed in %.0f s; frames and logs in %s' % (len(todo) - failed, len(todo), time.time() - t0,
                                                               os.path.relpath(OUT, frames.ROOT)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
