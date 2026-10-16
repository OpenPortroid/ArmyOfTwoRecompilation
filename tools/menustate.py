"""Current UE3 UI scene stack of a running game (guest memory).

python tools/menustate.py <pid>            -> one line: scenes bottom..top
python tools/menustate.py <pid> --wait NAME [timeout_s]   -> exit 0 once NAME is the top scene
GameUISceneClient (class name ends with SceneClient) ActiveScenes TArray at +0xC0.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3net  # noqa: E402

ACTIVE = 0xC0
MOVIE_PLAYER = 0x831E6E64  # cutscene_skip.cpp kMoviePlayer: pointer to the full-screen movie player (+88 playing)
WIDGETS = ('AO2UILabelButton', 'UIOptionButton')  # cursor-focusable menu widgets (+0x8C == 2 when focused)
OPTION_INDEX = 0x3CC  # UIOptionButton: selected value


class State:
    def __init__(self, pid):
        self.m = ue3net.Mem(pid)
        self.names = ue3net.Names(self.m)
        self.client = 0

    def nm(self, o):
        n = self.names(self.m.u32(o + 44))
        num = self.m.u32(o + 48)
        return n + ('_%d' % (num - 1) if num and num < 0x10000 else '')

    def of_class(self, name):
        """Live objects of class `name` (one pass over GObjObjects, cached until the object count changes)."""
        count = self.m.u32(ue3net.ue3probe.GOBJ + 4)
        if count is None:
            return getattr(self, '_idx', {}).get(name, [])
        if getattr(self, '_idx_count', None) != count:
            idx = {}
            for o in ue3net.objects(self.m):
                if o:
                    cls = self.m.u32(o + 52)
                    if cls:
                        idx.setdefault(self.names(self.m.u32(cls + 44)), []).append(o)
            self._idx, self._idx_count = idx, count
        return self._idx.get(name, [])

    def owners(self, o):
        r = []
        for _ in range(4):
            o = self.m.u32(o + 40) if o else 0
            r.append(o)
        return r

    def find_client(self):
        for o in ue3net.objects(self.m):
            if o and self.names(self.m.u32(self.m.u32(o + 52) + 44)).endswith('SceneClient') \
                    and not self.nm(o).startswith('Default'):
                self.client = o
                return
        raise SystemExit('no scene client')

    def scenes(self):
        if not self.client or self.m.u32(self.client + 44) is None:
            self.find_client()
        d, n = self.m.u32(self.client + ACTIVE), self.m.u32(self.client + ACTIVE + 4)
        if not d or n is None or n > 64:
            self.find_client()
            d, n = self.m.u32(self.client + ACTIVE), self.m.u32(self.client + ACTIVE + 4)
        if d is None or n is None:
            return []
        raw = self.m.read(d, n * 4) or b''
        return [self.names(self.m.u32(p + 44)) for p in struct.unpack('>%dI' % (len(raw) // 4), raw) if p]


def _buttons(self, scene):
    """AO2UILabelButton widgets owned by `scene` (cached per scene address)."""
    cache = self.__dict__.setdefault('_btn', {})
    if scene not in cache:
        cache[scene] = [o for w in WIDGETS for o in self.of_class(w) if scene in self.owners(o)]
    return cache[scene]


def _top(self):
    d, n = self.m.u32(self.client + ACTIVE), self.m.u32(self.client + ACTIVE + 4)
    return self.m.u32(d + (n - 1) * 4) if n else 0


def focused(self):
    """Name of the focused button of the top scene (+0x8C == 2), or None."""
    self.scenes()
    top = _top(self)
    for b in (_buttons(self, top) if top else []):
        if self.m.u32(b + 0x8C) == 2:
            return self.nm(b)
    return None


def items(self):
    """Button names of the top scene in widget (object) order."""
    self.scenes()
    top = _top(self)
    return [self.nm(b) for b in _buttons(self, top)] if top else []


def player(self):
    """Live AO2PlayerController (cached; re-searched when it went away) or 0."""
    c = getattr(self, '_pc', 0)
    if c and self.names(self.m.u32(self.m.u32(c + 52) + 44) or 0) == 'AO2PlayerController'             and not self.nm(c).startswith('Default'):
        return c
    self._pc = 0
    for o in self.of_class('AO2PlayerController'):
        if not self.nm(o).startswith('Default'):
            self._pc = o
            break
    return self._pc


def cutscene(self):
    """What can be skipped now: 'movie' (full-screen movie, e.g. the loading screen), 'matinee' (controller in
    cinematic mode) or None."""
    movie = self.m.u32(MOVIE_PLAYER)
    if movie and self.m.u32(movie + 88):
        return 'movie'
    return 'matinee' if self.cinematic() else None


def in_game(self):
    """A level is running: the player controller has a pawn (Controller.Pawn at +452)."""
    c = self.player()
    if not (c and self.m.u32(c + 452)):
        return False
    movie = self.m.u32(MOVIE_PLAYER)  # a playing full-screen movie = loading screen / cutscene
    return not (movie and self.m.u32(movie + 88))


def option(self, name):
    """Selected index of the UIOptionButton `name` of the top scene (None if absent)."""
    self.scenes()
    top = _top(self)
    for b in (_buttons(self, top) if top else []):
        if self.nm(b) == name:
            return self.m.u32(b + OPTION_INDEX)
    return None


def cinematic(self):
    """The player controller is in cinematic mode (a Matinee cutscene is playing): [controller + 808] & 0x20000."""
    c = self.player()
    return bool(c and self.m.u32(c + 808) & 0x20000)


def signals(self):
    """Debug: raw cutscene signals (movie playing, cinematic bit, playing SeqAct_Interp count)."""
    movie = self.m.u32(MOVIE_PLAYER)
    c = self.player()
    interps = self.of_class('SeqAct_Interp')
    return {'movie': bool(movie and self.m.u32(movie + 88)),
            'cine': bool(c and self.m.u32(c + 808) & 0x20000),
            'interp': sum(1 for o in interps if self.m.u32(o + 264) & 0x80000000),
            'interp_flags': sorted({hex(self.m.u32(o + 264)) for o in interps})[:4]}


State.signals = signals
State.cutscene = cutscene
State.option = option
State.cinematic = cinematic
State.focused, State.items, State.player, State.in_game = focused, items, player, in_game


def main():
    s = State(int(sys.argv[1]))
    if len(sys.argv) > 3 and sys.argv[2] == '--wait':
        end = time.time() + (float(sys.argv[4]) if len(sys.argv) > 4 else 60)
        while time.time() < end:
            sc = s.scenes()
            if sc and sc[-1] == sys.argv[3]:
                print(' > '.join(sc))
                return
            time.sleep(0.2)
        print('timeout: ' + ' > '.join(s.scenes()))
        raise SystemExit(1)
    print(' > '.join(s.scenes()) + ' [' + str(s.focused()) + ']')


if __name__ == '__main__':
    main()
