#!/bin/bash
# qemu-ppc: 9 clips x {C on G3 (no AltiVec unit), AltiVec on 7450} x mode
cd /home/claude/stage3/s3; M=${1:-annexb}
for clip in ../clips/*.264; do b=$(basename $clip); ref=$(grep "^$b " ../clips/ref.md5 | cut -d= -f2); line="$b"
  for v in 0 1; do cpu=$([ $v = 1 ] && echo 7450_v2.1 || echo 750)
    info=$(qemu-ppc -cpu $cpu ./t_linux $clip /tmp/l_$v.yuv $v $M 2>&1 | tail -1); m=$(md5sum /tmp/l_$v.yuv | cut -c1-32)
    line="$line | $([ $v = 1 ] && echo AV || echo C) $([ "$m" = "$ref" ] && echo OK || echo FAIL) $info"; done
  echo "$line"; done
