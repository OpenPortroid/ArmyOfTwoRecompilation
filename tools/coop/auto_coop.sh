#!/bin/bash
# auto_coop.sh - one online co-op test on two PCs, no screenshots needed.
#
#   bash tools/coop/auto_coop.sh [map_rights] [minutes]
#
# map_rights: presses of "right" on the map selector (0 Somalia, 1 Afghanistan,
# 2 Iraq; default 2). minutes: how long to let the level run after the
# lockstep starts if nothing desyncs (default 6).
#
# Host = the partner PC over ssh (ao2_partner_ed25519 key), where the user has
# started "AO2 launcher.cmd" in the game folder (it starts the host when
# launch_host.request appears). Joiner = this PC ("Join co-op.bat" flags).
# Both get the menus through input.script (no window focus needed); the
# timings are the ones that worked on 2026-10-07 (menus drop presses that come
# too early). Results: tools/coop/results/<time>/ - both sync_trace.log and
# coop_debug.log, and the sync_diff.py report.
set -u
RIGHTS=${1:-2}
MINUTES=${2:-6}
HOST=admin@192.168.31.88
KEY="$HOME/.ssh/ao2_partner_ed25519"
HOST_DIR='C:\aot2\aot\out\build\mn'
HOST_DIR_SCP='C:/aot2/aot/out/build/mn'
REPO=$(cd "$(dirname "$0")/../.." && pwd)
GAME="$REPO/aot/out/build/win-amd64-release"
HOST_IP=$(head -1 "$GAME/join_host.txt" 2>/dev/null | tr -d '\r ')
HOST_IP=${HOST_IP:-192.168.31.88}
OUT="$REPO/tools/coop/results/$(date +%Y%m%d_%H%M%S)"
R() { ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes "$HOST" "$@"; }
log() { echo "[$(date +%T)] $*"; }

rights=""
for ((i = 0; i < RIGHTS; i++)); do rights="$rights right wait1200"; done
# From the process start: startup movies, Press Start, main menu ->
# Campaign -> Public Co-op Game -> map -> confirm (host: opens the lobby,
# joiner: joins it).
MENU="wait30000 skip wait2000 skip wait2000 skip wait2000 start wait5000 a wait5000 down wait700 down wait700 down wait1000 a wait6000$rights a"

mkdir -p "$OUT"
log "stopping old games"
R "taskkill /im armyoftworecomp.exe /f >nul 2>&1" || true
taskkill //im armyoftworecomp.exe //f >/dev/null 2>&1 || true
sleep 3
R "del /q $HOST_DIR\\sync_trace.log $HOST_DIR\\coop_debug.log $HOST_DIR\\input.script 2>nul"
rm -f "$GAME/sync_trace.log" "$GAME/coop_debug.log" "$GAME/input.script"

log "starting the host (launcher on the partner PC)"
R "type nul > $HOST_DIR\\launch_host.request"
for i in $(seq 1 20); do
  sleep 2
  R "tasklist | findstr /i armyoftworecomp >nul" && break
done
R "tasklist | findstr /i armyoftworecomp >nul" || { log "host did not start: is AO2 launcher.cmd running?"; exit 1; }
R "echo $MENU> $HOST_DIR\\input.script"
log "host menus sent"

# The joiner's last press must come after the host's lobby is up: start it
# 15 s later.
sleep 15
log "starting the joiner ($HOST_IP)"
(cd "$GAME" && echo "$HOST_IP" > join_host.txt && cmd //c start "" armyoftworecomp.exe --ao2_coop_host="$HOST_IP" --log_to_files=true --ao2_coop_debug=true)
sleep 3
printf '%s\n' "$MENU" > "$GAME/input.script"
log "joiner menus sent"

# Menus take ~75 s from the start; then the host starts the game.
sleep 85
log "host: Start Game"
R "echo a> $HOST_DIR\\input.script"

# The mission briefing movie plays 40-65 s after Start Game; A (Enter) skips
# it on both.
sleep 45
log "skipping the briefing (A on both)"
R "echo a> $HOST_DIR\\input.script"
echo a > "$GAME/input.script"

log "waiting for the lockstep"
for i in $(seq 1 120); do
  sleep 3
  [ -f "$GAME/sync_trace.log" ] && [ "$(grep -c '^F ' "$GAME/sync_trace.log")" -gt 60 ] && break
done
if ! [ -f "$GAME/sync_trace.log" ]; then
  log "no lockstep on this PC - check the lobby"; exit 1
fi
# The lockstep runs from the lobby on; the level has started when the trace
# lists pawns. "Mid-mission Shopping?" comes up on both then: B = Cancel.
for i in $(seq 1 120); do
  tail -c 200000 "$GAME/sync_trace.log" | grep -q "^P [0-9]* AO2Char" && break
  sleep 2
done
sleep 4
log "declining the shop (B on both)"
R "echo b> $HOST_DIR\\input.script"
echo b > "$GAME/input.script"
log "lockstep running; up to $MINUTES min, stops at the first checksum mismatch + 30 s"
# Every step starts by letting go of the sticks and the trigger: a new
# input.script replaces the running one, whose own "ly0" may not have come yet.
# Fight: both players run to the nearest live enemy and shoot when close
# (positions from this PC's trace - the same state on both while in sync;
# host = player 0, joiner = player 1). Set FIGHT=0 to only wait.
NAV="$(cygpath -w "$REPO/tools/coop/nav.py")"
TRACE="$(cygpath -w "$GAME/sync_trace.log")"
ROUTE="$(cygpath -w "$REPO/tools/coop/iraq_route.txt")" # the route to the first fight (Iraq start)
steer() { # <player id> -> input.script steps
  local out mx dist pos last
  out=$(python "$NAV" "$TRACE" "$1" "$ROUTE" "$(cygpath -w "$OUT/route$1")" 2>/dev/null)
  mx=$(echo "$out" | grep -o "mx -\?[0-9]*" | cut -d' ' -f2)
  dist=$(echo "$out" | grep -o "dist [0-9]*" | cut -d' ' -f2)
  pos=$(echo "$out" | grep -o "pos [-0-9,]*" | cut -d' ' -f2)
  [ -z "$dist" ] && { echo "lx0 ly0 rt-"; return; }
  last=$(cat "$OUT/pos$1" 2>/dev/null); echo "$pos" > "$OUT/pos$1"
  if echo "$out" | grep -q " enemy " && [ "$dist" -lt 700 ]; then
    echo "lx0 ly0 mx$mx wait300 rt+ wait1500 rt-"
  elif [ -n "$last" ] && python -c "import sys,math; a=[float(v) for v in '$last'.split(',')]; b=[float(v) for v in '$pos'.split(',')]; sys.exit(0 if math.dist(a,b) < 40 else 1)"; then
    # Stuck (rails, walls, cover): back off and step aside (no Y: that takes cover).
    echo "lx0 rt- ly-20000 wait700 ly0 lx$(( (RANDOM % 2) * 40000 - 20000 )) wait900 lx0 mx$mx wait200 ly20000 wait1000 ly0"
  else
    # Running speed ~450 units/s: stop about at the point (they are ~200 apart).
    local ms=$(( dist * 1000 / 450 )); [ $ms -gt 1500 ] && ms=1500; [ $ms -lt 250 ] && ms=250
    echo "lx0 rt- mx$mx wait300 ly20000 wait$ms ly0"
  fi
}
end=$((SECONDS + MINUTES * 60))
desync=0
while [ $SECONDS -lt $end ]; do
  if [ "${FIGHT:-1}" = 1 ]; then
    R "echo $(steer 0)> $HOST_DIR\input.script"
    steer 1 > "$GAME/input.script"
  fi
  sleep 2.5
  if grep -q "flag52=1" "$GAME/coop_debug.log" 2>/dev/null; then
    desync=1
    log "checksum mismatch: $(grep -m1 'flag52=1' "$GAME/coop_debug.log" | grep -o 'frame=[0-9]*' | head -1)"
    sleep 30
    break
  fi
done
[ $desync = 0 ] && log "no mismatch in $MINUTES min"

log "collecting logs -> $OUT"
cp "$GAME/sync_trace.log" "$OUT/A_sync_trace.log"
cp "$GAME/coop_debug.log" "$OUT/A_coop_debug.log"
scp -q -i "$KEY" -o IdentitiesOnly=yes "$HOST:$HOST_DIR_SCP/sync_trace.log" "$OUT/B_sync_trace.log"
scp -q -i "$KEY" -o IdentitiesOnly=yes "$HOST:$HOST_DIR_SCP/coop_debug.log" "$OUT/B_coop_debug.log"
python "$REPO/tools/sync_diff.py" "$OUT/A_sync_trace.log" "$OUT/B_sync_trace.log" --context 1 > "$OUT/diff.txt" 2>&1
head -40 "$OUT/diff.txt" | cut -c1-300
