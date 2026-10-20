#!/bin/bash
# route_bench.sh <exe> <label> [extra args...]
# Iraq 05_00: autopilot up the hill into the first fight, 40 s of fighting at the end.
# Averages decouple.log fps / engine tick and native_gpu.log GPU ms over the run.
exe="$1"; label="$2"; shift 2
R=$(cd "$(dirname "$0")/.." && pwd)
S=$R/tools
# BENCH_DIR: the game folder (logs, cwd); BENCH_LEGACY=1 leaves out the quality flags
# that releases before 1.3 don't know
dir=${BENCH_DIR:-$R/aot/out/build/win-amd64-release}
cd "$dir" || exit 1
# a game still closing keeps the logs open
while tasklist | grep -qi "^$(basename "$exe") "; do sleep 1; done
rm -f native_gpu.log decouple.log autopilot.log input.script autopilot.path
args=(--log_to_files=true --fullscreen=true --ao2_start_checkpoint=05_00 --ao2_god_mode=true --ao2_fps_unlock
      --ao2_fps_target=240 --ao2_decouple=true --ao2_native_gpu_resolution_scale=2 --ao2_fps_overlay=0
      --ao2_native_gpu_vsync=false --ao2_native_gpu_stats_every=240 --ao2_decouple_debug=true
      --ao2_skip_cutscenes=true --ao2_skip_unskippable=true)
[ "$BENCH_LEGACY" = 1 ] || args+=(--ao2_anti_aliasing=off --ao2_ambient_occlusion=ssao
      --ao2_motion_blur=0 --ao2_depth_of_field=0 --ao2_shadow_resolution=0)
args+=("$@")
wexe=$(cygpath -w "$exe")
powershell -NoProfile -Command "\$p = Start-Process -FilePath '$wexe' -ArgumentList '${args[*]}' -WorkingDirectory '$(cygpath -w $dir)' -PassThru; \$p.Id" > /tmp/bench_pid.txt
pid=$(tr -d '\r' < /tmp/bench_pid.txt)
echo "$label pid $pid"
until grep -E "^  [0-9.]+ fps" native_gpu.log 2>/dev/null | tail -2 | awk '{s+=$1} END{exit !(NR==2 && s/2>45)}'; do
  tasklist | grep -q " $pid " || { echo "game exited"; exit 1; }
  sleep 3
done
sleep 5
route=0
for try in 1 2 3; do
  (cd "$S" && timeout 90 python -c "
import gps_path as g
r=g.read_route($pid)
g.write_path(r'$(cygpath -w "$dir")', g.chain(r, visible_only=True))
") && { route=1; break; }
  echo "no route (try $try)"; sleep 5
done
[ $route = 1 ] || { powershell -NoProfile -Command "Stop-Process -Id $pid -Force; Wait-Process -Id $pid -EA SilentlyContinue"; exit 1; }
start_line=$(wc -l < decouple.log)
gstart=$(wc -l < native_gpu.log)
last=""; same=0; endt=0
for i in $(seq 1 120); do
  sleep 5
  if [ $(( $(date +%s) - $(stat -c %Y decouple.log) )) -gt 20 ]; then echo "game hung"; break; fi
  cur=$(grep "^pos" autopilot.log | tail -1 | sed 's/.*to end //')
  if [ "$cur" = "$last" ]; then same=$((same+1)); else same=0; fi
  if [ $same -ge 2 ] && [ "${cur:-0}" -gt 8000 ] && [ ! -f input.script ]; then echo "b wait600 b" > input.script; same=0; fi
  last=$cur
  if [ "${cur:-9999}" -lt 400 ]; then endt=$((endt+1)); fi
  [ $endt -ge 8 ] && break
done
echo "stop" > autopilot.path
tail -n +$((start_line+1)) decouple.log | sed -n 's/.*fps=\([0-9.]*\).*engine tick avg \([0-9.]*\).*/\1 \2/p' > /tmp/bench_dec.txt
tail -n +$((gstart+1)) native_gpu.log | sed -n 's/.*gpu ms\/frame:.*total \([0-9.]*\)/\1/p' > /tmp/bench_gpu.txt
awk -v l="$label" '{f+=$1; t+=$2; n++; if($1<min||n==1)min=$1} END{printf "%s: %d s, fps avg %.1f min %.1f, engine tick avg %.2f ms\n", l, n, f/n, min, t/n}' /tmp/bench_dec.txt
awk '{g+=$1; n++} END{printf "  gpu ms/frame avg %.2f\n", g/n}' /tmp/bench_gpu.txt
# the slowest seconds after the first 5 (loading)
tail -n +6 /tmp/bench_dec.txt | sort -n | head -10 | awk '{printf "%s%.0f", (NR>1?" ":"  slowest seconds: "), $1} END{print ""}'
# the last 40 s (the fight)
tail -40 /tmp/bench_dec.txt | awk '{f+=$1; t+=$2; n++} END{printf "  fight (last %d s): fps avg %.1f, tick %.2f ms\n", n, f/n, t/n}'
powershell -NoProfile -Command "Stop-Process -Id $pid -Force; Wait-Process -Id $pid -EA SilentlyContinue"
