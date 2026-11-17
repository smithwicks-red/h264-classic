/* ppc-linux test of the AVCDecLib API: in.264 out.yuv altivec(0/1) mode(annexb|avcc) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avcdec.h"
static long inuse, peak;
static void *a(void *o, unsigned long n){ void *p; (void)o; if(posix_memalign(&p,16,n+16))return 0; *(long*)p=n; inuse+=n; if(inuse>peak)peak=inuse; return (char*)p+16; }
static void f(void *o, void *p){ (void)o; if(p){ inuse-=*(long*)((char*)p-16); free((char*)p-16);} }
static void lg(void *o,int l,const char*fmt,va_list v){ (void)o;(void)l; vfprintf(stderr,fmt,v); }
static FILE *out; static long nout;
static void put(AVCDecPicture *p){ long y; nout++;
  for(y=0;y<p->height;y++) fwrite(p->plane[0]+y*p->stride[0],1,p->width,out);
  for(y=0;y<p->height/2;y++) fwrite(p->plane[1]+y*p->stride[1],1,p->width/2,out);
  for(y=0;y<p->height/2;y++) fwrite(p->plane[2]+y*p->stride[2],1,p->width/2,out); }
static long nal_next(const unsigned char *b,long n,long i){ for(;i+3<=n;i++) if(b[i]==0&&b[i+1]==0&&b[i+2]==1) return i; return n; }
int main(int argc,char**argv){
  AVCDecCallbacks cb={a,f,lg,0}; unsigned long spm; AVCDecContext *c; AVCDecPicture pic;
  FILE *fi=fopen(argv[1],"rb"); long n; unsigned char *buf; int avcc=argc>4&&!strcmp(argv[4],"avcc");
  fseek(fi,0,SEEK_END); n=ftell(fi); fseek(fi,0,SEEK_SET); buf=malloc(n); fread(buf,1,n,fi); fclose(fi);
  out=fopen(argv[2],"wb");
  avcdec_init(&cb, atoi(argv[3]) ? AVCDEC_FLAG_ALTIVEC : 0, &spm);
  if(!avcc){
    const unsigned char *p=buf; long left=n, used;
    c=avcdec_open(0,0);
    while(left>0){ if(avcdec_feed_annexb(c,p,left,&used,&pic)>0) put(&pic); p+=used; left-=used; }
    if(avcdec_feed_annexb(c,0,0,&used,&pic)>0) put(&pic);
  } else {
    /* split NALs; SPS/PPS -> avcC; other NALs grouped up to each VCL NAL -> 4-byte length samples */
    static unsigned char avcC[4096], smp[1<<20]; long ac=0, sl=0, i=nal_next(buf,n,0); int nsps=0,npps=0;
    static const unsigned char *sps[32],*pps[32]; static long spsl[32],ppsl[32];
    struct { const unsigned char *d; long l; } nals[100000]; long nn=0,k;
    while(i<n){ long s=i+3,e=nal_next(buf,n,s),t=e; while(t>s&&buf[t-1]==0)t--; nals[nn].d=buf+s; nals[nn].l=t-s; nn++; i=e; }
    for(k=0;k<nn;k++){ int ty=nals[k].d[0]&31; if(ty==7&&nsps<32){sps[nsps]=nals[k].d;spsl[nsps++]=nals[k].l;} if(ty==8&&npps<32){pps[npps]=nals[k].d;ppsl[npps++]=nals[k].l;} }
    avcC[ac++]=1; avcC[ac++]=sps[0][1]; avcC[ac++]=sps[0][2]; avcC[ac++]=sps[0][3]; avcC[ac++]=0xFF; avcC[ac++]=0xE0|1;
    avcC[ac++]=spsl[0]>>8; avcC[ac++]=spsl[0]; memcpy(avcC+ac,sps[0],spsl[0]); ac+=spsl[0];
    avcC[ac++]=1; avcC[ac++]=ppsl[0]>>8; avcC[ac++]=ppsl[0]; memcpy(avcC+ac,pps[0],ppsl[0]); ac+=ppsl[0];
    c=avcdec_open(avcC,ac);
    for(k=0;k<nn;k++){ int ty=nals[k].d[0]&31; if(ty==7||ty==8||ty==9) continue;
      smp[sl]=nals[k].l>>24; smp[sl+1]=nals[k].l>>16; smp[sl+2]=nals[k].l>>8; smp[sl+3]=nals[k].l; memcpy(smp+sl+4,nals[k].d,nals[k].l); sl+=4+nals[k].l;
      if(ty==1||ty==5){ if(avcdec_decode_sample(c,smp,sl,&pic)>0) put(&pic); sl=0; } }
  }
  while(avcdec_flush(c,&pic)>0) put(&pic);
  avcdec_close(c); fclose(out);
  fprintf(stderr,"frames=%ld spmod16=%lu peak=%ld inuse_after=%ld\n",nout,spm,peak,inuse);
  return 0; }
