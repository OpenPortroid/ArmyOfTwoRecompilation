#!/bin/bash
# local2.sh - two co-op instances on this PC (A hosts on 127.0.0.1, B joins from 127.0.0.2).
#
#   bash tools/coop/local2.sh            start both (windowed)
#   bash tools/coop/local2.sh menus      walk both to the Iraq lobby (input.script), host starts the game
#
# Each instance gets its own working dir (logs, input.script, capture.trigger),
# user data / cache roots (a copy of this PC's profile, so the maps unlocked
# there are there) and shader cache copy: two instances on the same roots
# crash (c0000374). Dirs: tools/coop/local/A, tools/coop/local/B.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
GAME="$REPO/aot/out/build/win-amd64-release"
ROOT="$REPO/tools/coop/local_tu2"
PROFILE="$HOME/Documents/armyoftworecomp_tu2" # TU2 build: its own user data (with the DLC)

if [ "${1:-}" = menus ]; then
  # Timings as auto_coop.sh (menus drop presses that come too early).
  MENU="a wait5000 down wait700 down wait700 down wait1000 a wait6000 right wait1200 right wait1200 a"
  echo "$MENU" > "$ROOT/A/input.script"
  sleep 15
  echo "$MENU" > "$ROOT/B/input.script"
  sleep 30
  echo a > "$ROOT/A/input.script"
  echo "Start Game sent"
  exit 0
fi

for name in A B; do
  dir="$ROOT/$name"
  mkdir -p "$dir"
  [ -d "$dir/shader_cache" ] || cp -r "$GAME/shader_cache" "$dir/shader_cache"
  [ -d "$dir/user" ] || cp -r "$PROFILE" "$dir/user"
  rm -f "$dir/sync_trace.log" "$dir/sync_dump.bin" "$dir/coop_debug.log" "$dir/input.script"
done
common="--fullscreen=false --window_width=1280 --window_height=720 --ao2_native_gpu_resolution_scale=1 --log_to_files=true --ao2_coop_debug=true --ao2_coop_sync_dump=${SYNC_DUMP:-5} ${EXTRA:-}"
start_one() { # <name> <ip> <extra args>
  local dir="$ROOT/$1" w
  w=$(cygpath -w "$dir")
  (cd "$dir" && cmd //c start "" "$(cygpath -w "$GAME/armyoftworecomp.exe")" $common --net_bind_ip=$2 \
    --net_gamertag=Player$1 "--user_data_root=$w\\user" "--cache_root=$w\\cache" $3)
}
start_one A 127.0.0.1 "--ao2_coop_server=true"
# B connects to the EA server A runs at start: give it time to come up (B hung
# for 30 s with a black screen when it started 2 s after A).
sleep 20
start_one B 127.0.0.2 "--ao2_coop_server=false --ao2_coop_host=127.0.0.1"
# Startup movies and Press Start: both to the main menu.
for name in A B; do
  echo "wait30000 skip wait2000 skip wait2000 skip wait2000 start" > "$ROOT/$name/input.script"
done
echo "started; main menu in ~45 s, then: bash tools/coop/local2.sh menus"
