#!/bin/sh
# Permanent edge set: expected result per file; Python and C must agree, and rewritten ones must match tables.
cd /home/claude/s5/rw; L=logs/edge.log; J=logs/edge_compare.jsonl; : > $L; : > $J
while read f exp; do b=$(basename $f)
  py=$(python3 -c "
import sys; sys.path.insert(0,'sim'); import rewrite
try: r=rewrite.rewrite_A('$f','edge/A_$b'); print('N=%d' % r['N'] if r['N'] else 'unchanged')
except Exception as e: print('reject')" 2>&1 | tail -1)
  j=$(./avcr64 $f edge/C_$b); rc=$(echo "$j" | python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print(d['rc'], 'N=%d' % d['N'])")
  ok=no
  case "$exp" in
    rw*) [ "$py" = "N=${exp#rw}" ] && [ "$(echo $rc | cut -d' ' -f1)" = 0 ] && ok=yes
         python3 sim/compare.py $f edge/A_$b edge/C_$b "$j" >> $J
         tail -1 $J | grep -q '"ok": true' || ok=no ;;
    nochange) [ "$py" = unchanged ] && [ "$(echo $rc | cut -d' ' -f1)" = 1 ] && cmp -s $f edge/C_$b && ok=yes ;;
    *) [ "$py" = reject ] && [ "$(echo $rc | cut -d' ' -f1)" = "$exp" ] && cmp -s $f edge/C_$b && ok=yes ;;
  esac
  echo "$b expected=$exp py=$py c_rc=$rc PASS=$ok" >> $L
done <<LIST
edge/e_co64.mp4 rw2
edge/e_co64_fs.mp4 rw2
edge/e_emptyedit.mp4 rw2
edge/e_noedts.mp4 rw2
edge/e_stscmulti.mp4 rw2
edge/e_lead_negcts.mp4 rw2
edge/e_lead_negstart.mp4 rw2
edge/e_trailempty.mp4 rw2
edge/e_2media.mp4 -14
edge/e_stz2.mp4 -6
edge/e_mvex.mp4 -5
edge/e_2desc.mp4 -7
edge/e_rdly.mp4 -13
edge/e_mdhd1.mp4 -11
edge/rev/c1_nostss.mp4 -15
edge/rev/c2_trim.mp4 rw2
edge/rev/c3_mp4vfirst.mp4 rw2
edge/rev/c4_tkhd1.mp4 -11
edge/rev/c5_stdp_sbgp.mp4 rw2
edge/rev/c6_bigN.mp4 -10
edge/rev/c7_avc_nob_first.mp4 rw2
edge/rev/c8_two_avc_b.mp4 rw2
LIST
echo "edge: $(grep -c PASS=yes $L) pass, $(grep -c PASS=no $L) fail"
