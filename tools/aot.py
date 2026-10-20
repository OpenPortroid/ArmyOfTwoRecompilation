"""aot.py - start the game and drive it to a place, reading the UE3 UI state from memory.

    python tools/aot.py solo [--checkpoint 05_04] [--difficulty recruit] [--inst A]
    python tools/aot.py coop [--map somalia|afghanistan|iraq] [--difficulty recruit] [--secs 30]
    python tools/aot.py host [--map carrier] [--difficulty recruit]   host for a partner on another PC:
                                                       lobby, Start Game as soon as the partner is in
    python tools/aot.py start [--inst A] [-- game args]   start + skip movies + stop at the main menu
    python tools/aot.py status [--inst A]                  scene stack, focused button, in-game flag
    python tools/aot.py nav --inst A step [step ...]       menu steps (below)
    python tools/aot.py shot [--inst A] [--out file.png]   front buffer as PNG
    python tools/aot.py autopilot [--inst A] [--stop]      run along the level's GPS route (gps_path.py)
    python tools/aot.py autopilot --route NAME             run a recorded route
    python tools/aot.py record NAME [--pid N]              record your way to tools/routes/NAME.path
    python tools/aot.py mark [a|b|x|y] [--note ...]        the autopilot presses that button where you stand now
    python tools/aot.py stop [--inst A|all]

Steps for `nav`:  a / b / down / start / skip ...  press and wait for the screen to change
                  a:Scene       press, wait until Scene is the top scene (retries the press)
                  @Item         move the cursor onto the button named Item
                  ~Opt=N        set option widget Opt to index N (reads the value from memory)
                  ?Scene        wait for Scene            wait500  sleep ms
Every instance has its own folder tools/coop/local/<name> (input.script, capture.trigger, logs,
`user` = a copy of the game profile, so the real saves are never touched; the first start copies
it from ~/Documents/armyoftworecomp_tu2).  Instance A is the co-op host (127.0.0.1, EA server),
B joins (127.0.0.2).  State of the UI comes from tools/menustate.py (GameUISceneClient stack).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import menustate  # noqa: E402

REPO = os.path.dirname(HERE)
GAME = os.path.join(REPO, 'aot', 'out', 'build', 'win-amd64-release')
EXE = os.environ.get('AOT_EXE') or os.path.join(GAME, 'armyoftworecomp.exe')  # AOT_EXE: another build in GAME (A/B)
BASE = os.path.join(HERE, 'coop', 'local')
def _documents():
    """The user's Documents folder (may be redirected, e.g. to OneDrive), as the game finds it."""
    import ctypes
    buf = ctypes.create_unicode_buffer(260)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0:  # CSIDL_PERSONAL
        return buf.value
    return os.path.join(os.path.expanduser('~'), 'Documents')


PROFILE = os.path.join(_documents(), 'armyoftworecomp_tu2')
STATE = os.path.join(BASE, 'aot_state.json')
IPS = {'A': '127.0.0.1', 'B': '127.0.0.2'}

# Difficulty buttons of DifficultySelectScene in cursor order (down = next).
DIFFICULTY = {'recruit': 0, 'soldier': 1, 'veteran': 2, 'contractor': 3}
# CoopHostScene: UIOptionButton_1 = Map, UIOptionButton_0 = Difficulty; value = index (read from memory).
MAPS = {'somalia': 0, 'afghanistan': 1, 'iraq': 2, 'carrier': 3, 'china': 4, 'miami': 5}
MAP_OPT, DIFF_OPT = 'UIOptionButton_1', 'UIOptionButton_0'


def log(*a):
    print('[%s]' % time.strftime('%H:%M:%S'), *a, flush=True)


def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(st):
    os.makedirs(BASE, exist_ok=True)
    with open(STATE, 'w') as f:
        json.dump(st, f)


def alive(pid):
    out = subprocess.run(['tasklist', '/fi', 'PID eq %d' % pid, '/nh'], capture_output=True, text=True, errors='replace').stdout or ''
    return os.path.splitext(os.path.basename(EXE))[0].lower() in out.lower()  # also an AOT_EXE build


class Inst:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(BASE, name)
        self.script = os.path.join(self.dir, 'input.script')
        self._ui = None

    @property
    def pid(self):
        pid = load_state().get(self.name)
        return pid if pid and alive(pid) else None

    def ui(self):
        if self._ui is None or self._ui.pid != self.pid:
            self._ui = menustate.State(self.pid)
            self._ui.pid = self.pid
        return self._ui

    def scenes(self):
        try:
            return self.ui().scenes()
        except (SystemExit, TypeError, struct_error):
            self._ui = None
            return []

    def focused(self):
        try:
            return self.ui().focused()
        except (SystemExit, TypeError, struct_error):
            return None

    def press(self, text):
        """input.script step(s); waits until the game has taken the previous file."""
        for _ in range(100):
            if not os.path.exists(self.script):
                break
            time.sleep(0.05)
        with open(self.script, 'w') as f:
            f.write(text)

    def skip(self):
        """Cutscene / movie skip (what Enter and Space do in the game) through the file skip.trigger."""
        open(os.path.join(self.dir, 'skip.trigger'), 'w').close()

    def console(self, line):
        with open(os.path.join(self.dir, 'console.txt'), 'w') as f:
            f.write(line + '\n')

    # ---- starting / stopping
    def start(self, extra=(), host=None, ip=None, test_window=True):
        """`ip` '' = no --net_bind_ip (reachable from other PCs); `test_window` False = the window and
        resolution of the settings files, shown normally (a game the user plays)."""
        if self.pid:
            self.stop()
        os.makedirs(self.dir, exist_ok=True)
        user = os.path.join(self.dir, 'user')
        if not os.path.isdir(user):
            log('%s: copying the profile (first start)' % self.name)
            shutil.copytree(PROFILE, user)
            shutil.rmtree(os.path.join(user, 'cache'), ignore_errors=True)
        cache = os.path.join(self.dir, 'shader_cache')
        if not os.path.isdir(cache):
            shutil.copytree(os.path.join(GAME, 'shader_cache'), cache)
        for f in ('input.script', 'console.txt', 'capture.trigger'):
            try:
                os.remove(os.path.join(self.dir, f))
            except OSError:
                pass
        if ip is None:
            ip = IPS.get(self.name, '127.0.0.%d' % (2 + ord(self.name[0]) - ord('A')))
        args = [EXE, '--log_to_files=true', '--net_gamertag=Player' + self.name,
                '--user_data_root=' + user, '--cache_root=' + os.path.join(self.dir, 'cache')]
        if test_window:
            # Defaults of a test window; a flag given in `extra` replaces its default.
            given = {e.split('=')[0] for e in extra}
            args += [d for d in ('--fullscreen=false', '--window_width=1280', '--window_height=720',
                                 '--ao2_native_gpu_resolution_scale=1') if d.split('=')[0] not in given]
        if ip:
            args.append('--net_bind_ip=' + ip)
        if host is True:
            args.append('--ao2_coop_server=true')
        elif host:
            args += ['--ao2_coop_server=false', '--ao2_coop_host=' + host]
        args += list(extra)
        si = subprocess.STARTUPINFO()  # open the window without activating it: nothing pops up over the user's work
        env = dict(os.environ)
        if test_window:
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 4  # SW_SHOWNOACTIVE
            env['SDL_WINDOW_ACTIVATE_WHEN_SHOWN'] = '0'  # SDL3 hint: do not take the focus when shown
        p = subprocess.Popen(args, cwd=self.dir, creationflags=0x00000008 | 0x00000200, startupinfo=si, env=env)  # DETACHED | NEW_GROUP
        st = load_state()
        st[self.name] = p.pid
        save_state(st)
        log('%s: started pid %d (%s)' % (self.name, p.pid, ip or 'all addresses'))

    def stop(self):
        pid = self.pid
        if pid:
            subprocess.run(['taskkill', '/pid', str(pid), '/f'], capture_output=True)
            time.sleep(1.5)
        self._ui = None

    # ---- waiting for screens
    def wait_scene(self, name, timeout=60, nudge=None):
        """Wait until `name` is the top scene; `nudge` = key pressed every 2 s meanwhile."""
        end, last = time.time() + timeout, 0
        while time.time() < end:
            sc = self.scenes()
            if sc and sc[-1] == name:
                return sc
            if nudge and time.time() - last > 2:
                self.press(nudge)
                last = time.time()
            if not self.pid:
                raise SystemExit('%s: game exited' % self.name)
            time.sleep(0.2)
        raise SystemExit('%s: timeout waiting for %s, now: %s' % (self.name, name, ' > '.join(self.scenes())))

    def go(self, key, scene, timeout=40):
        """Press `key` and wait for `scene` on top. A transition takes several seconds (the stack
        passes through other states), so the press is repeated only when the stack did not change at
        all for 6 s (a press made too early is dropped by the menus)."""
        end = time.time() + timeout
        while time.time() < end:
            before = self.scenes()
            self.press(key)
            t = time.time() + 6
            while time.time() < t:
                sc = self.scenes()
                if sc and sc[-1] == scene:
                    time.sleep(0.4)  # let the screen settle before the next press
                    return sc
                if sc != before:
                    t = time.time() + 6  # moving: keep waiting, do not press again
                    before = sc
                time.sleep(0.15)
        raise SystemExit('%s: %s did not lead to %s (now %s)' % (self.name, key, scene, ' > '.join(self.scenes())))

    def focus(self, item):
        """Cursor onto button `item` (down first, then up)."""
        ui = self.ui()
        for key in ('down', 'up'):
            for _ in range(len(ui.items()) + 1):
                if self.focused() == item:
                    return
                self.press(key)
                time.sleep(0.5)
        if self.focused() != item:
            raise SystemExit('%s: no button %s (have %s, focused %s)' % (self.name, item, ui.items(), self.focused()))

    def set_option(self, name, value):
        """Cursor onto option `name`, then right/left until its index (memory) is `value`."""
        self.focus(name)
        for key in ('right', 'left'):
            for _ in range(8):
                cur = self.ui().option(name)
                if cur == value:
                    return
                self.press(key)
                t = time.time() + 2
                while time.time() < t and self.ui().option(name) == cur:
                    time.sleep(0.1)
        raise SystemExit('%s: option %s did not reach %s (is %s)' % (self.name, name, value, self.ui().option(name)))

    def cinematic(self):
        try:
            return self.ui().cinematic()
        except (SystemExit, TypeError, struct_error):
            return False

    def in_game(self):
        try:
            return 'HelpTextUIScene' not in self.scenes() and self.ui().in_game()
        except (SystemExit, TypeError, struct_error):
            return False

    def boot_to_menu(self, timeout=90):
        """Skip the startup movies, press Start; returns at MainMenuScene."""
        log('%s: booting (skipping movies)' % self.name)
        end, last, n = time.time() + timeout, 0, 0
        while time.time() < end:
            sc = self.scenes()
            top = sc[-1] if sc else None
            if top == 'MainMenuScene':
                time.sleep(0.5)
                return
            if time.time() - last > (4.0 if top else 1.2):  # a press needs time to change the screen
                n += 1
                if top in ('StartMenuUIScene', 'AttractLauncher'):
                    self.press('start' if n % 2 else 'skip')  # Press Start / attract movie
                elif top is None:
                    self.press('skip')  # startup movies
                last = time.time()
            if not self.pid:
                raise SystemExit('%s: game exited during boot' % self.name)
            time.sleep(0.2)
        raise SystemExit('%s: boot timeout, scenes: %s' % (self.name, ' > '.join(self.scenes())))


try:
    from struct import error as struct_error
except ImportError:  # pragma: no cover
    struct_error = ValueError


def cmd_start(a):
    i = Inst(a.inst)
    i.start(a.game_args)
    i.boot_to_menu()
    log('at the main menu: ' + ' > '.join(i.scenes()))


def solo_menu(i, difficulty):
    """MainMenu -> Operations -> Single Player -> Continue From Checkpoint -> difficulty."""
    i.go('a', 'MissionSetupScene')
    i.focus('SinglePlayer')
    i.go('a', 'SinglePlayerGameScene')
    i.focus('Continue')
    i.go('a', 'DifficultySelectScene')
    btns = i.ui().items()
    for _ in range(DIFFICULTY[difficulty]):
        i.press('down')
        time.sleep(0.5)
    i.press('a')


def cmd_solo(a):
    i = Inst(a.inst)
    extra = list(a.game_args)
    if a.checkpoint:  # the game itself loads the checkpoint, no menus
        extra += ['--ao2_start_checkpoint=' + a.checkpoint, '--ao2_start_difficulty=%d' % DIFFICULTY[a.difficulty]]
    i.start(extra)
    if a.checkpoint:
        log('%s: loading checkpoint %s on its own' % (i.name, a.checkpoint))
    else:
        i.boot_to_menu()
        solo_menu(i, a.difficulty)
    wait_game(i, a.timeout)


def wait_game(i, timeout, skipper=True):
    """Until the level is playable: loaded (pawn), no menu scenes, no cutscene/movie playing for 2.5 s.
    `skipper`: this instance sends the game's own skip (skip.trigger = Enter/Space) for a movie or a cutscene; in co-op only one
    machine may do it (the skip is carried to the partner through the lockstep). A prompt over the level
    (Mid-mission Shopping...) is cancelled with B - never Enter, which would accept it."""
    log('%s: loading...' % i.name)
    end, last, clear = time.time() + timeout, 0, None
    trace = os.environ.get('AOT_TRACE')
    while time.time() < end:
        loaded = i.in_game()  # pawn, no menu scenes, no movie
        sc = i.scenes()
        try:
            what = i.ui().cutscene()
        except (SystemExit, TypeError, struct_error):
            what = None
        if trace:
            print('  %s %s loaded=%s scenes=%s cutscene=%s' % (time.strftime('%H:%M:%S'), i.name, loaded, sc, what), flush=True)
        if time.time() - last > 2:
            if loaded and sc:
                i.press('b')  # a prompt over the level
                last = time.time()
            elif not sc and what and skipper:
                i.skip()  # the movie / cutscene: the game's own skip, no window focus needed
                last = time.time()
        if loaded and not sc and not what:
            clear = clear or time.time()
            if time.time() - clear > 2.5:  # stays clear: nothing pops up any more
                log('%s: in game' % i.name)
                return
        else:
            clear = None
        if not i.pid:
            raise SystemExit('%s: game exited while loading' % i.name)
        time.sleep(0.4)
    raise SystemExit('%s: not in game after %ds, scenes: %s' % (i.name, timeout, ' > '.join(i.scenes())))


def parallel(*fns):
    """Run the callables at the same time (threads); the first exception (SystemExit too) is re-raised."""
    import threading
    errs = []

    def run(fn):
        try:
            fn()
        except BaseException as e:  # noqa: BLE001
            errs.append(e)

    ts = [threading.Thread(target=run, args=(f,)) for f in fns]
    for th in ts:
        th.start()
    for th in ts:
        th.join()
    if errs:
        raise errs[0]


def wait_any(i, names, timeout=90):
    """Wait until one of `names` is the top scene; returns it."""
    end = time.time() + timeout
    while time.time() < end:
        sc = i.scenes()
        if sc and sc[-1] in names:
            time.sleep(0.4)
            return sc[-1]
        if not i.pid:
            raise SystemExit('%s: game exited' % i.name)
        time.sleep(0.2)
    raise SystemExit('%s: none of %s, now: %s' % (i.name, names, ' > '.join(i.scenes())))


def public_coop(i):
    """MainMenu -> MissionSetupScene -> "Public Co-op Game" (searches for a game itself).
    NB the widget names in memory are swapped against the labels: widget `Public` is the label
    "Private Co-op Game" (3rd item), widget `Private` is "Public Co-op Game" (4th, three downs)."""
    i.go('a', 'MissionSetupScene')
    if (i.scenes() or [''])[-1] != 'MissionSetupScene':
        return  # a repeated press already went on (to the map / difficulty screen)
    i.focus('Private')
    i.press('a')


LOBBY = ('CoopNetworkLobbyScene', 'CoopNetworkLobbyClientScene')  # host / partner lobby


def enter_public_lobby(i, mapname, difficulty):
    """Public Co-op Game: the map/difficulty screen (CoopFilterLobbyScene), then the game itself searches
    for a game and joins it, or creates one. Returns True when this instance created the game (is the host)."""
    if (i.scenes() or [''])[-1] not in ('CoopFilterLobbyScene', 'CoopHostScene') + LOBBY:
        public_coop(i)
    first = wait_any(i, ('CoopFilterLobbyScene', 'CoopHostScene') + LOBBY, 90)
    created = first not in LOBBY
    if created:
        i.set_option(MAP_OPT, MAPS[mapname])
        i.set_option(DIFF_OPT, DIFFICULTY[difficulty])
        i.press('a')
        end = time.time() + 90
        while time.time() < end and (i.scenes() or [''])[-1] not in LOBBY:
            if (i.scenes() or [''])[-1] == 'CoopHostScene':  # asks to host: not a joiner
                created = True
                i.press('a')
                time.sleep(2)
            time.sleep(0.3)
        wait_any(i, LOBBY, 5)
    return created


def cmd_coop(a):
    """A = host (EA server; searches, finds nothing, creates the game), B = partner (same menu, joins A)."""
    A, B = Inst('A'), Inst('B')
    # Game args are for both; "A:--flag=x" / "B:--flag=x" for one of them.
    own = lambda name: [g[2:] if g[:2] == name + ':' else g for g in a.game_args if g[:2] in (name + ':', '--')]
    A.start(own('A'), host=True)
    B.start(own('B'), host='127.0.0.1')  # both boot at the same time
    parallel(A.boot_to_menu, B.boot_to_menu)
    enter_public_lobby(A, a.map, a.difficulty)  # the host first: B must find its game
    log('A: in the lobby (%s/%s)' % (a.map, a.difficulty))
    enter_public_lobby(B, a.map, a.difficulty)
    log('B: in the lobby')
    time.sleep(3)
    A.press('a')  # Start Game
    parallel(lambda: wait_game(A, a.timeout), lambda: wait_game(B, a.timeout, skipper=False))


def cmd_host(a):
    """Instance H hosts for a partner on another PC (Join co-op.bat there): EA server, reachable on
    every address, the settings files' window. Lobby with --map / --difficulty, then Start Game once
    the partner is in (its join, EGRS pid=2, in coop_debug.log), and on into the level."""
    H = Inst(a.inst)
    debug = os.path.join(H.dir, 'coop_debug.log')
    H.start(['--ao2_coop_debug=true'] + list(a.game_args), host=True, ip='', test_window=False)
    H.boot_to_menu()
    if not enter_public_lobby(H, a.map, a.difficulty):
        # Public Co-op found the partner's game first: the partner hosts and starts it.
        log("%s: joined the partner's game - waiting for its Start Game" % H.name)
        wait_game(H, a.wait, skipper=False)
        log('%s: logs in %s' % (H.name, H.dir))
        return
    log('%s: in the lobby (%s/%s) - waiting for the partner (Join co-op.bat on the other PC)' %
        (H.name, a.map, a.difficulty))
    end = time.time() + a.wait
    while time.time() < end:
        try:
            with open(debug, errors='replace') as f:
                if any('EGRS' in l and 'allowed=1' in l and 'pid=2' in l for l in f):
                    break
        except OSError:
            pass
        if not H.pid:
            raise SystemExit('%s: game exited in the lobby' % H.name)
        time.sleep(1)
    else:
        raise SystemExit('%s: no partner after %ds; the lobby stays open' % (H.name, a.wait))
    log('%s: partner joined, starting' % H.name)
    time.sleep(5)  # the partner's lobby settles
    H.press('a')  # Start Game
    wait_game(H, a.timeout)
    log('%s: logs in %s' % (H.name, H.dir))


def cmd_wait(a):
    wait_game(Inst(a.inst), a.timeout)


def cmd_status(a):
    i = Inst(a.inst)
    if not i.pid:
        raise SystemExit('%s: not running' % i.name)
    print('pid %d | %s | focused: %s | in game: %s' % (i.pid, ' > '.join(i.scenes()), i.focused(), i.in_game()))
    ui = i.ui()
    if ui.client:
        print('buttons:', ui.items())


def cmd_nav(a):
    i = Inst(a.inst)
    for st in a.steps:
        if st.startswith('~'):  # ~UIOptionButton_1=2
            name, _, val = st[1:].partition('=')
            i.set_option(name, int(val))
        elif st.startswith('@'):
            i.focus(st[1:])
        elif st.startswith('?'):
            i.wait_scene(st[1:])
        elif st.startswith('wait'):
            time.sleep(int(st[4:]) / 1000)
            continue
        elif ':' in st:
            key, scene = st.split(':', 1)
            i.go(key, scene)
        else:
            before = i.scenes()
            i.press(st)
            end = time.time() + 1.5
            while time.time() < end and i.scenes() == before:
                time.sleep(0.1)
        print('%-22s %s [%s]' % (st, ' > '.join(i.scenes()), i.focused()))


def cmd_shot(a):
    i = Inst(a.inst)
    cap = os.path.join(i.dir, 'capture')
    shutil.rmtree(cap, ignore_errors=True)
    open(os.path.join(i.dir, 'capture.trigger'), 'w').close()
    for _ in range(60):
        if os.path.exists(os.path.join(cap, 'index.txt')):
            break
        time.sleep(0.25)
    time.sleep(1.5)
    subprocess.run([sys.executable, os.path.join(HERE, 'capture_to_png.py'), cap], capture_output=True)
    pngs = sorted(f for f in os.listdir(cap) if f.endswith('_1F5F8000_color.png'))
    if not pngs:
        raise SystemExit('no front buffer in the capture')
    out = a.out or os.path.join(i.dir, 'shot.png')
    shutil.copy(os.path.join(cap, pngs[-1]), out)
    print(out)


def cmd_autopilot(a):
    """See _autopilot. On the way out (end, error, Ctrl+C) the autopilot is switched off - it would go on aiming and
    shooting while you play - unless --keep."""
    try:
        _autopilot(a)
    finally:
        if not a.stop and not a.keep:
            with open(os.path.join(Inst(a.inst).dir, 'autopilot.path'), 'w') as f:
                f.write('stop\n')


def _autopilot(a):
    """The player runs along a route (autopilot.path -> autopilot.cpp):
    - the GPS route the level shows now (gps_path.py). The level shows the pieces of its route as the mission goes on
      (Kismet); their visibility is read every second and the route is rebuilt from the player whenever it changes,
      so the run follows the current objective (--all: the whole route at once);
    - where the level shows none: a route recorded by hand (`aot.py record`, tools/routes/*.path) that passes near
      the player, from its nearest point on;
    - --route NAME|FILE: only that recorded route.
    Ends after --wait s with nothing to run, after --stall s without progress, or at the timeout. Prompts over the
    level are closed with B; on GAME OVER the checkpoint is restarted. Movies and cutscenes are skipped by the game."""
    import gps_path
    i = Inst(a.inst)
    if a.stop:
        with open(os.path.join(i.dir, 'autopilot.path'), 'w') as f:
            f.write('stop\n')
        return
    route = gps_path.read_route(i.pid)
    fixed = None
    if a.route:
        file = a.route if os.path.exists(a.route) else os.path.join(gps_path.ROUTES, a.route + '.path')
        fixed = gps_path.load_path(file)
        if len(fixed) < 2:
            raise SystemExit('%s: fewer than 2 points' % file)
    st = {'using': None, 'done': set(), 'last_try': 0}
    logf = os.path.join(i.dir, 'autopilot.log')
    start = os.path.getsize(logf) if os.path.exists(logf) else 0
    end, last_press, last_report, last_vis = time.time() + a.timeout, 0, 0, 0
    best, best_time, pos, shown, waiting = -1, time.time(), '', None, False

    def send(what, path):
        log('%s: %s -> %d points, %.0f units' % (i.name, what, len(path), gps_path.write_path(i.dir, path)))

    def try_recorded():
        """A recorded route near the player; True if one was sent."""
        st['last_try'] = time.time()
        name, rest = gps_path.recorded_route(route['player'], exclude=st['done'])
        if name:
            st['using'] = name
            send('recorded route ' + name, rest)
        return bool(name)

    while time.time() < end:
        if not i.pid:
            raise SystemExit('%s: the game exited' % i.name)
        now = time.time()
        if now - last_vis > 1:
            last_vis = now
            before = route['player']
            vis = gps_path.refresh(route)
            here = route['player']
            jumped = before and here and (here[0] - before[0]) ** 2 + (here[1] - before[1]) ** 2 > 3000 ** 2
            if fixed is None and (jumped or (waiting and now - st.get('rescan', 0) > 15)):
                # Parts of a level stream in with their own arrows (after a transition the player is moved there):
                # read the list of route meshes again.
                st['rescan'] = now
                log('%s: %s - reading the route meshes again' % (i.name, 'moved' if jumped else 'waiting'))
                route = gps_path.read_route(i.pid)
                vis = gps_path.refresh(route)
                shown = None
            if fixed is not None:
                if shown is None and route['player']:
                    shown = True
                    rests = [gps_path.from_nearest(piece, route['player']) for piece in gps_path.pieces(fixed)]
                    send('route ' + a.route, min(rests, key=lambda r: r[1])[0])
            elif vis != shown:
                shown = vis
                names = [m['mesh'] for m in route['meshes'] if m['in_gps'] and not m['hidden']]
                path = gps_path.chain(route, visible_only=not a.all) if route['player'] else []
                if len(path) >= 2:
                    st['using'] = None
                    send('route shown: ' + ', '.join(names), path)
                    best, best_time, waiting = -1, now, False
                elif st['using']:
                    pass  # running a recorded route already
                elif try_recorded():
                    best, best_time, waiting = -1, now, False
                else:
                    log('%s: route shown: %s - nothing to run, waiting' % (i.name, ', '.join(names) or 'none'))
                    best_time, waiting = now, True
            elif waiting and now - st['last_try'] > 5 and try_recorded():
                best, best_time, waiting = -1, now, False
        lines = []
        if os.path.exists(logf):
            if os.path.getsize(logf) < start:
                start = 0  # the game opens the log anew when it first writes to it
            with open(logf) as f:
                f.seek(start)
                lines = f.read().splitlines()
                start = f.tell()
        for ln in lines:
            if ln.startswith('stop: arrived'):
                if fixed is not None or a.all:
                    log('%s: autopilot arrived' % i.name)
                    return
                if st['using']:
                    log('%s: end of recorded route %s' % (i.name, st['using']))
                    st['done'].add(st['using'])
                    st['using'] = None
                if try_recorded():
                    best, best_time = -1, time.time()
                else:
                    log('%s: end of the route, waiting for the level to show more' % i.name)
                    best_time, waiting = time.time(), True
            elif ln.startswith('fight: ') or ln.startswith('aim: target'):
                best_time = time.time()  # fighting (the aimbot) is not a stall
                if time.time() - last_report > 10:
                    last_report = time.time()
                    log('%s: %s' % (i.name, ln))
            elif ln.startswith('pos ') or ln.startswith('drive ('):  # on foot / at the wheel
                pos = ln
                seg = int(ln.split(' seg ')[1].split('/')[0])
                if seg > best:
                    best, best_time = seg, time.time()
        sc = i.scenes()
        if now - last_press > 2 and sc:
            if 'GameOverUIScene' in sc:
                log('%s: GAME OVER - restart from the checkpoint' % i.name)
                i.press('a')
                best_time = now + 30  # the reload
            elif i.in_game():
                i.press('b')  # a prompt over the level (Mid-mission Shopping...)
            last_press = now
        if now - last_report > 10 and pos and not waiting:
            last_report = now
            log('%s: %s' % (i.name, pos))
        if waiting and now - best_time > a.wait:
            raise SystemExit('%s: nothing to run for %ds at %s' % (i.name, a.wait, route['player']))
        if not waiting and now - best_time > a.stall:
            with open(os.path.join(i.dir, 'autopilot.path'), 'w') as f:
                f.write('stop\n')
            raise SystemExit('%s: no progress along the route for %ds: %s' % (i.name, a.stall, pos))
        time.sleep(0.5)
    raise SystemExit('%s: autopilot timeout: %s' % (i.name, pos))


def cmd_mark(a):
    """An action point where the player stands now: the autopilot presses BUTTON when it gets there
    (tools/routes/actions.txt)."""
    import struct
    import gps_path
    pid = a.pid or Inst(a.inst).pid
    if not pid:
        raise SystemExit('no game running')
    s = menustate.State(pid)
    c = menustate.player(s)
    pawn = s.m.u32(c + 452) if c else 0
    if not pawn:
        raise SystemExit('no player pawn (not in a level?)')
    p = struct.unpack('>3f', s.m.read(pawn + 228, 12))
    os.makedirs(gps_path.ROUTES, exist_ok=True)
    with open(gps_path.ACTIONS, 'a') as f:
        f.write('%.1f %.1f %.1f %s%s\n' % (p + (a.button, '  # ' + a.note if a.note else '')))
    log('action %s at (%.0f %.0f %.0f) -> %s' % ((a.button,) + p + (gps_path.ACTIONS,)))


def cmd_record(a):
    """Records the player's way (a point every --step units) into tools/routes/NAME.path while you play, until Ctrl+C
    or --secs. Works on a tools instance (--inst) or any running game (--pid). The autopilot runs such routes where the
    level shows no GPS arrows (or with --route NAME)."""
    import struct
    import gps_path
    pid = a.pid or Inst(a.inst).pid
    if not pid:
        raise SystemExit('no game running')
    s = menustate.State(pid)
    os.makedirs(gps_path.ROUTES, exist_ok=True)
    out = os.path.join(gps_path.ROUTES, a.name + '.path')
    if os.path.exists(out) and not a.force:
        raise SystemExit('%s exists (--force to overwrite)' % out)
    last, n, end = None, 0, time.time() + (a.secs or 1e9)
    log('recording to %s (Ctrl+C to stop)' % out)
    with open(out, 'w') as f:
        f.write('# recorded %s\n' % time.strftime('%Y-%m-%d %H:%M'))
        try:
            while time.time() < end:
                c = menustate.player(s)
                pawn = s.m.u32(c + 452) if c else 0
                p = struct.unpack('>3f', s.m.read(pawn + 228, 12)) if pawn else None
                if p and (last is None or (p[0] - last[0]) ** 2 + (p[1] - last[1]) ** 2 >= a.step ** 2):
                    if last is not None and (p[0] - last[0]) ** 2 + (p[1] - last[1]) ** 2 > 2000 ** 2:
                        log('jump of %.0f units (respawn / load?) - recorded as is' % (
                            ((p[0] - last[0]) ** 2 + (p[1] - last[1]) ** 2) ** 0.5))
                    f.write('%.1f %.1f %.1f\n' % p)
                    f.flush()
                    last, n = p, n + 1
                    if n % 25 == 0:
                        log('%d points, at (%.0f %.0f %.0f)' % ((n,) + p))
                time.sleep(0.1)
        except KeyboardInterrupt:
            pass
    log('%d points -> %s' % (n, out))


def cmd_stop(a):
    for n in (['A', 'B'] if a.inst == 'all' else [a.inst]):
        Inst(n).stop()
    log('stopped')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    def add(name, fn, inst='A'):
        p = sub.add_parser(name)
        p.add_argument('--inst', default=inst)
        p.set_defaults(fn=fn)
        return p

    p = add('start', cmd_start)
    p.add_argument('game_args', nargs='*')
    p = add('solo', cmd_solo)
    p.add_argument('--checkpoint', help='e.g. 05_04: load it directly (no menus)')
    p.add_argument('--difficulty', default='recruit', choices=sorted(DIFFICULTY))
    p.add_argument('--timeout', type=int, default=120)
    p.add_argument('game_args', nargs='*')
    p = add('coop', cmd_coop)
    p.add_argument('--map', default='iraq', choices=sorted(MAPS))
    p.add_argument('--difficulty', default='recruit', choices=sorted(DIFFICULTY))
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('game_args', nargs='*')
    p = add('host', cmd_host, inst='H')
    p.add_argument('--map', default='carrier', choices=sorted(MAPS))
    p.add_argument('--difficulty', default='recruit', choices=sorted(DIFFICULTY))
    p.add_argument('--wait', type=int, default=900, help='seconds to wait for the partner in the lobby')
    p.add_argument('--timeout', type=int, default=180)
    p.add_argument('game_args', nargs='*')
    p = add('wait', cmd_wait)
    p.add_argument('--timeout', type=int, default=180)
    add('status', cmd_status)
    p = add('nav', cmd_nav)
    p.add_argument('steps', nargs='+')
    p = add('shot', cmd_shot)
    p.add_argument('--out')
    p = add('autopilot', cmd_autopilot)
    p.add_argument('--timeout', type=int, default=900)
    p.add_argument('--stall', type=int, default=120, help='give up after this many seconds without progress')
    p.add_argument('--wait', type=int, default=120, help='at the end of the shown route: wait this long for more')
    p.add_argument('--all', action='store_true', help='the whole route, also the pieces not shown yet')
    p.add_argument('--route', help='run this recorded route (tools/routes/NAME.path or a file) instead of the GPS')
    p.add_argument('--stop', action='store_true')
    p.add_argument('--keep', action='store_true', help='leave the autopilot (and aimbot) on at the end')
    p = add('mark', cmd_mark)
    p.add_argument('button', nargs='?', default='a', choices=['a', 'b', 'x', 'y'])
    p.add_argument('--note', help='a comment kept in the file')
    p.add_argument('--pid', type=int, help='a game not started by this tool')
    p = add('record', cmd_record)
    p.add_argument('name', help='tools/routes/NAME.path')
    p.add_argument('--pid', type=int, help='a game not started by this tool')
    p.add_argument('--step', type=float, default=100.0, help='a point every this many units')
    p.add_argument('--secs', type=float)
    p.add_argument('--force', action='store_true')
    add('stop', cmd_stop)
    a = ap.parse_args()
    a.fn(a)


main()
