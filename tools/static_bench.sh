#!/bin/bash
# static_bench.sh <exe> <label> [extra args...]
# Iraq 05_00, standing at the start: 20 s of fps, engine tick, GPU ms and CPU ms per frame of the busiest threads.
exe="$1"; label="$2"; shift 2
R=$(cd "$(dirname "$0")/.." && pwd)
S=$R/tools
dir=$R/aot/out/build/win-amd64-release
cd "$dir" || exit 1
rm -f native_gpu.log decouple.log autopilot.log input.script autopilot.path
args=(--log_to_files=true --fullscreen=true --ao2_start_checkpoint=05_00 --ao2_god_mode=true --ao2_fps_unlock
      --ao2_fps_target=240 --ao2_decouple=true --ao2_native_gpu_resolution_scale=2 --ao2_fps_overlay=0
      --ao2_native_gpu_vsync=false --ao2_native_gpu_stats_every=240 --ao2_decouple_debug=true
      --ao2_skip_cutscenes=true --ao2_skip_unskippable=true --ao2_anti_aliasing=off --ao2_ambient_occlusion=ssao
      --ao2_motion_blur=0 --ao2_depth_of_field=0 --ao2_shadow_resolution=0 "$@")
wexe=$(cygpath -w "$exe")
powershell -NoProfile -Command "\$p = Start-Process -FilePath '$wexe' -ArgumentList '${args[*]}' -WorkingDirectory '$(cygpath -w $dir)' -PassThru; \$p.Id" > /tmp/bench_pid.txt
pid=$(tr -d '\r' < /tmp/bench_pid.txt)
until grep -E "^  [0-9.]+ fps" native_gpu.log 2>/dev/null | tail -2 | awk '{s+=$1} END{exit !(NR==2 && s/2>45)}'; do
  tasklist | grep -q " $pid " || { echo "game exited"; exit 1; }
  sleep 3
done
sleep 20
start_line=$(wc -l < decouple.log); gstart=$(wc -l < native_gpu.log)
python "$S/threadload.py" $pid 20 > /tmp/bench_threads.txt
tail -n +$((start_line+1)) decouple.log | sed -n 's/.*fps=\([0-9.]*\).*engine tick avg \([0-9.]*\).*draw-only [0-9]*, avg \([0-9.]*\).*/\1 \2 \3/p' > /tmp/bench_dec.txt
fps=$(awk '{f+=$1; n++} END{printf "%.1f", f/n}' /tmp/bench_dec.txt)
awk -v l="$label" '{f+=$1; t+=$2; d+=$3; n++} END{printf "%s: fps %.1f, engine tick %.2f ms, draw-only %.2f ms\n", l, f/n, t/n, d/n}' /tmp/bench_dec.txt
tail -n +$((gstart+1)) native_gpu.log | sed -n 's/.*gpu ms\/frame:.*total \([0-9.]*\)/\1/p' | awk '{g+=$1; n++} END{printf "  gpu ms/frame %.2f\n", g/n}'
head -4 /tmp/bench_threads.txt | awk -v fps=$fps '{pct=$1; sub("%","",pct); $1=""; printf "  %5.2f ms/frame %s\n", pct*10/fps, $0}'
powershell -NoProfile -Command "Stop-Process -Id $pid -Force"
