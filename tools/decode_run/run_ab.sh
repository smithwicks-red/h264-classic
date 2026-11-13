#!/bin/sh
# run_ab.sh -- run the codec's REAL decode path on the host, old loop vs
# new loop, against ffmpeg. See the top of decode_run.c for what it does.
#
#   usage: run_ab.sh MOVIE.mp4 DECODER_SRC_DIR [WORKDIR]
#
# MOVIE.mp4        an H.264 Baseline MP4/MOV (the dv512 test clip)
# DECODER_SRC_DIR  the ported h264bsd sources (decoder-src/)
#
# Needs gcc, python3 with numpy, ffmpeg/ffprobe.
set -e
MOVIE=$1
D=$2
W=${3:-./decode_run_work}
HERE=$(cd "$(dirname "$0")" && pwd)
C=$HERE/../../codec-src
mkdir -p "$W"

python3 "$HERE/extract_samples.py" "$MOVIE" "$W/movie.samples"
ffmpeg -v error -y -i "$MOVIE" -f rawvideo -pix_fmt yuv420p "$W/ffmpeg.yuv"

python3 "$HERE/extract_run.py" "$C/H264Codec.c" "$W/frags_new"
python3 "$HERE/make_old_loop.py" "$C/H264Codec.c" "$W/old_H264Codec.c"
python3 "$HERE/extract_run.py" "$W/old_H264Codec.c" "$W/frags_old"

for v in new old; do
    gcc -std=gnu89 -O1 -w -I"$W/frags_$v" -I"$D" -I"$C" \
        -o "$W/decode_run_$v" "$HERE/decode_run.c" "$D"/h264bsd_*.c \
        "$C/avcC_parse.c" -lm
done

for v in old new; do
    for m in poster twice seq; do
        "$W/decode_run_$v" "$W/movie.samples" "$W/out_${v}_$m.yuv" $m \
            "$W/dst_${v}_$m.2vuy" 2> "$W/err_${v}_$m.log"
        echo "[$v/$m] $(grep 'mode' "$W/err_${v}_$m.log" | sed 's/\[harness\] //')"
        python3 "$HERE/compare_ff.py" "$W/out_${v}_$m.yuv" "$W/ffmpeg.yuv" | sed 's/^/    pictures: /'
        python3 "$HERE/check_dst.py" "$W/dst_${v}_$m.2vuy" "$W/ffmpeg.yuv" $m | sed 's/^/    dest:     /'
    done
done
