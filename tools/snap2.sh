#!/bin/sh
# Capture one frame of the running game (capture.trigger) and copy the presented image to tools/snaps/<name>.png
d=/c/aot2/aot/out/build/win-amd64-release
name=${1:-snap}
rm -rf $d/capture
touch $d/capture.trigger
i=0; while [ -f $d/capture.trigger ] || [ ! -f $d/capture/index.txt ]; do sleep 0.25; i=$((i+1)); [ $i -gt 80 ] && { echo timeout; exit 1; }; done
sleep 1
(cd $d && python /c/aot2/tools/capture_to_png.py capture >/dev/null)
mkdir -p /c/aot2/tools/snaps
p=$(ls $d/capture/*_presented.png | tail -1)
cp "$p" /c/aot2/tools/snaps/$name.png && echo "C:/aot2/tools/snaps/$name.png"
