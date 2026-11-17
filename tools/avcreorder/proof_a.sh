#!/bin/sh
# usage: proof_a.sh <tool-binary> <outdir> <log>
T=$1; O=$2; L=$3; : > $L
for f in /home/claude/s5/sim/files/*; do b=$(basename $f)
  j=$($T $f $O/C_$b)
  python3 /home/claude/s5/rw/sim/compare.py $f /home/claude/s5/rw/pyout/A_$b $O/C_$b "$j" >> $L
done
python3 -c "
import json; r=[json.loads(l) for l in open('$L')]
print('$L', 'files', len(r), 'ok', sum(x['ok'] for x in r), 'fail', [ (x['file'],x['diffs']) for x in r if not x['ok']], 'other-track samples checked', sum(x['other_samples'] for x in r))"
