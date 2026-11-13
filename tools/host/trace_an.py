import sys, collections
rows=[list(map(int,l.split()[1:])) for l in open(sys.argv[1]) if l.startswith('T ')]
# call frameNumber due now rate cond caller late action present scale dur wall
print('calls', len(rows))
fn=[r[1] for r in rows]; wall=[r[12] & 0xFFFFFFFF for r in rows]
gaps=[(fn[i]-fn[i-1]) for i in range(1,len(fn))]
print('frameNumber first/last', fn[0], fn[-1], 'step histogram', collections.Counter(gaps).most_common(6))
iv=[(wall[i]-wall[i-1]) & 0xFFFFFFFF for i in range(1,len(wall))]
import statistics
print('arrival interval ms: median %.1f, min %.1f, max %.1f' % (statistics.median(iv)/1000, min(iv)/1000, max(iv)/1000))
print('intervals over 50 ms at calls:', [(i+1, round(v/1000)) for i,v in enumerate(iv) if v>50000][:20])
print('present bits', collections.Counter(r[9] for r in rows))
cf=collections.Counter(r[5] for r in rows); kf=collections.Counter(r[6] for r in rows)
print('conditionFlags', {hex(k):v for k,v in cf.items()})
print('callerFlags', {hex(k):v for k,v in kf.items()})
cu=[r[0] for r in rows if r[5] & (1<<14)]
print('catch-up (condition bit 14) at calls', cu[:40])
cu2=[r[0] for r in rows if r[6] & (1<<12)]
print('catch-up (caller bit 12) at calls', cu2[:40])
# wall offset vs frameNumber timeline (assume 1001/30000)
base=wall[0]
off=[((wall[i]-base) & 0xFFFFFFFF)/1000 - (fn[i]-fn[0])*1001/30 for i in range(len(rows))]
print('wall minus media time ms: first 5', [round(x) for x in off[:5]], 'min', round(min(off)), 'max', round(max(off)), 'last', round(off[-1]))
for i in range(0,len(rows),100): print('  call', i, 'frame', fn[i], 'offset ms', round(off[i]))
