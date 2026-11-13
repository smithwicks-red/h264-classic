#!/bin/sh
# make_test_clip.sh OUT.mov -- rebuild the kit's "H264 test movie.mov".
#
# A 30-second 360x240 test pattern (ffmpeg's testsrc2: moving colour bars,
# a counter and a clock) with a beeping 440 Hz tone. Nothing in it comes
# from anyone else's footage, so it is free of rights: CC0 / public
# domain. H.264 Constrained Baseline, level 1.3, 25 fps, one reference
# frame, a key frame every 2 s; AAC-LC 64 kbit/s stereo. qtify.py then
# makes the .mov that QuickTime 6 opens with sound.
# Needs ffmpeg built with libx264, and python3.
set -e
OUT=${1:?usage: make_test_clip.sh OUT.mov}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=${OUT%.mov}.tmp.mp4
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=360x240:rate=25:duration=30" \
  -f lavfi -i "sine=frequency=440:beep_factor=4:duration=30:sample_rate=44100" \
  -c:v libx264 -profile:v baseline -level 1.3 -pix_fmt yuv420p -g 50 \
  -b:v 450k -maxrate 512k -bufsize 1M -x264-params ref=1 \
  -c:a aac -b:a 64k -ac 2 -map_metadata -1 \
  -fflags +bitexact -flags:v +bitexact -flags:a +bitexact \
  -metadata title="H264 OS9 test pattern (CC0)" -movflags +faststart "$TMP"
python3 "$HERE/qtify.py" "$TMP" "$OUT"
rm -f "$TMP"
