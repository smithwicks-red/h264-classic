import re,sys
t=open(sys.argv[1]).read().split('Cross Reference Table')[1]
cur=None; refs={}
for line in t.splitlines():
    m=re.match(r'^(\S+)\s+(\S+)$',line)
    if m: cur=m.group(1); refs[cur]=[m.group(2)]; continue
    m=re.match(r'^\s+(\S+)$',line)
    if m and cur: refs[cur].append(m.group(1))
for s in sys.argv[2:]:
    if s in refs: print(s,':',' '.join(x.replace('util_','u:').replace('avc_','c:') for x in refs[s][1:]))
