#!/usr/bin/env python3
# Independent PEF validator (from PEFBinaryFormat.h / Mac OS Runtime Architectures ch.8)
import struct, sys
def H(w):  # Apple PEFComputeHashWord: SInt32 hash, UInt8 chars, PseudoRotate
    h=0
    for c in w:  # bytes -> unsigned
        h=((h<<1)-(h>>16))^c
        h=((h+2**31)%2**32)-2**31  # keep SInt32 wrap
    return ((len(w)<<16)|((h^(h>>16))&0xFFFF))&0xFFFFFFFF
def IDX(w,p): return (w^(w>>p))&((1<<p)-1)
def check(fn):
    d=open(fn,'rb').read(); err=[]; info={}
    t1,t2,arch,fv,_,_,_,_,nsec,ninst,_=struct.unpack('>4s4s4sIIIIIHHI',d[:40])
    if (t1,t2,arch)!=(b'Joy!',b'peff',b'pwpc') or fv!=1: err.append('bad container header')
    secs=[]
    for i in range(nsec):
        s=struct.unpack('>iIIIIIBBBB',d[40+28*i:68+28*i]); secs.append(s)
    spans=[]
    for i,(no,da,tl,ul,cl,co,kind,share,al,_) in enumerate(secs):
        if co+cl>len(d): err.append(f's{i} outside file')
        if kind in (0,1) and co%16: err.append(f's{i} off {co:#x} not 16-aligned')
        if kind in (0,1) and al<4: err.append(f's{i} align 2^{al}<16')
        if kind in (0,1,2) and not (cl<=ul<=tl) and kind!=2: err.append(f's{i} lengths inconsistent')
        if kind==1 and cl!=ul: err.append('unpacked data cl!=ul')
        if cl: spans.append((co,co+cl,i))
        info[f's{i}']=f'k{kind} off={co:#x} len={cl:#x} tot={tl:#x} al={al}'
    spans.sort()
    if spans and spans[0][0]<40+28*nsec: err.append('section overlaps headers')
    for a,b in zip(spans,spans[1:]):
        if a[1]>b[0]: err.append(f'overlap s{a[2]}/s{b[2]}')
    L=[s for s in secs if s[6]==4][0]; lo=L[5]; ld=d[lo:lo+L[4]]
    (ms,mo,isec,io,ts,to,nlib,nimp,nrs,rio,lso,eho,pw,nex)=struct.unpack('>iIiIiIIIIIIIII',ld[:56])
    info['main/init/term']=(ms,isec,ts)
    for nm,v in (('main',ms),('init',isec),('term',ts)):
        if v!=-1 and not 0<=v<nsec: err.append(f'{nm} section {v} invalid')
    # imports
    p=56; for_lib=[]
    for i in range(nlib):
        no,ov,cv,cnt,first,opt,_,_=struct.unpack('>IIIIIBBH',ld[p:p+24]); p+=24
        if first+cnt>nimp: err.append(f'lib{i} import range')
        if no>=eho-lso: err.append(f'lib{i} name off')
    p+=4*nimp
    # relocs
    hdrs=[]
    for i in range(nrs):
        si,_,rc,fr=struct.unpack('>HHII',ld[p:p+12]); p+=12; hdrs.append((si,rc,fr))
    if p!=lso and p!=rio: err.append(f'reloc hdr end {p:#x} vs instr {rio:#x}')
    ninstr=0; maxpos=0
    for si,rc,fr in hdrs:
        ins=[struct.unpack('>H',ld[rio+fr+2*k:rio+fr+2*k+2])[0] for k in range(rc)]
        ninstr+=rc; tgt=secs[si]; pos=0; k=0; hist=[]
        def touch(n):
            nonlocal pos,maxpos
            pos+=n; maxpos=max(maxpos,pos)
        while k<len(ins):
            w=ins[k]; start=k; k+=1
            if w>>14==0: pos+=4*((w>>6)&0xFF); touch(4*(w&0x3F))
            elif w>>13==2:
                op=(w>>9)&15; n=(w&0x1FF)+1
                touch(n*{2:12,3:8,4:8}.get(op,4))
                if op==5 and (w&0)>=0: pass
            elif w>>13==3:
                op=(w>>9)&15
                if op in (0,3): touch(4)
                if op==0 and (w&0x1FF)>=nimp: err.append('SmByImport idx')
            elif w>>12==8: pos+=(w&0xFFF)+1
            elif w>>12==9:
                bc=((w>>8)&15)+1; rc2=(w&0xFF)+1
                blk=hist[-bc:]
                for _ in range(rc2):
                    for (a,b) in blk:  # re-execute (simple ops only)
                        sub=ins[a:b]; # approximate: count bytes
                        for x in sub:
                            if x>>14==0: pos+=4*((x>>6)&0xFF); touch(4*(x&0x3F))
                            elif x>>13==2: touch(((x&0x1FF)+1)*{2:12,3:8,4:8}.get((x>>9)&15,4))
                            elif x>>12==8: pos+=(x&0xFFF)+1
                            else: touch(4)
            elif w>>10==0x28: pos=((w&0x3FF)<<16)|ins[k]; k+=1
            elif w>>10==0x29: touch(4); k+=1
            elif w>>10==0x2C: err.append('LgRepeat unhandled'); k+=1
            elif w>>10==0x2D: k+=1; touch(4) if ((w>>6)&15)==0 else None
            else: err.append(f'bad reloc opcode {w:#06x}'); break
            hist.append((start,k))
        if maxpos>tgt[3]: err.append(f'relocs run past section {si} ({maxpos:#x}>{tgt[3]:#x})')
    info['relocInstr']=ninstr; info['relocMaxPos']=hex(maxpos)
    if lso!=rio+2*ninstr: err.append('strings offset != end of reloc instrs')
    # exports
    S=1<<pw; slots=struct.unpack(f'>{S}I',ld[eho:eho+4*S])
    kb=eho+4*S; keys=struct.unpack(f'>{nex}I',ld[kb:kb+4*nex]); sb=kb+4*nex
    if sb+10*nex!=len(ld): err.append(f'loader length mismatch {sb+10*nex:#x} vs {len(ld):#x}')
    exp=[struct.unpack('>IIh',ld[sb+10*i:sb+10*i+10]) for i in range(nex)]
    # Apple table: smallest power with nex>>power < 10 (kAverageChainLimit), cap 16
    rp=0
    while rp<16 and (nex>>rp)>=10: rp+=1
    if rp!=pw: err.append(f'power {pw} != recommended {rp}')
    nxt=0
    for s,v in enumerate(slots):
        c=v>>18; f=v&0x3FFFF
        if c and f!=nxt: err.append(f'slot {s} first {f} != {nxt}')
        nxt+= c if c else 0
        if not c and f not in (0,nxt) : pass
    if nxt!=nex: err.append(f'chain counts sum {nxt} != {nex}')
    found=0
    for i,(cn,val,sec) in enumerate(exp):
        no=cn&0xFFFFFF; ln=keys[i]>>16
        name=ld[lso+no:lso+no+ln]
        if H(name)!=keys[i]: err.append(f'key mismatch {name!r}'); continue
        # CFM lookup
        sl=slots[IDX(H(name),pw)]; c=sl>>18; f=sl&0x3FFFF; ok=False
        for j in range(f,f+c):
            if keys[j]==H(name) and ld[lso+(exp[j][0]&0xFFFFFF):][:ln]==name: ok= (j==i); break
        if not ok: err.append(f'unreachable {name.decode(errors="replace")}')
        else: found+=1
        if not (sec in (-2,-3) or (0<=sec<nsec and val<=secs[sec][2])): err.append(f'export {name!r} value out of section')
    info['exports']=f'{found}/{nex} reachable, power {pw}'
    return err,info
if __name__=='__main__':
    bad=0
    for fn in sys.argv[1:]:
        e,i=check(fn); bad|=bool(e)
        print(fn, 'OK' if not e else 'FAIL', i if len(sys.argv)<4 else i['exports'])
        for x in e[:8]: print('   ',x)
    sys.exit(bad)
