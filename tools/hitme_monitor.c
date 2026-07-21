#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define NCHA 56
#define NSCK 2
#define CHA_BASE(c)  (0x2000UL + (c) * 0x10UL)
#define CTRL(c,i)    (CHA_BASE(c) + 0x2 + (i))
#define CTR(c,i)     (CHA_BASE(c) + 0x8 + (i))
#define UNIT_CTL(c)  (CHA_BASE(c) + 0x0)
#define GLOBAL_CTL   0x2FF0UL
#define RST_BOTH     0x300UL

static int sck_cpu[NSCK] = {0, 48};
static int fd[NSCK];

static inline uint64_t rdmsr(int s,uint64_t o){ uint64_t v=0; if(pread(fd[s],&v,8,o)!=8)v=0; return v; }
static inline void     wrmsr(int s,uint64_t o,uint64_t v){ if(pwrite(fd[s],&v,8,o)!=8){} }
static uint64_t ctl(uint32_t ev,uint64_t um){ return (uint64_t)ev|((um&0xFF)<<8)|((um>>8)<<32); }

static void prog_socket(int s){
    wrmsr(s,GLOBAL_CTL,1);
    for(int c=0;c<NCHA;c++){ wrmsr(s,UNIT_CTL(c),RST_BOTH);
        wrmsr(s,CTRL(c,0),ctl(0x5f,0x1d));
        wrmsr(s,CTRL(c,1),ctl(0x5e,0x03)); }
    wrmsr(s,GLOBAL_CTL,0);
}

static void snapshot(uint64_t *out){
    for(int s=0;s<NSCK;s++)
        for(int c=0;c<NCHA;c++){
            out[(s*NCHA+c)*2+0]=rdmsr(s,CTR(c,0));
            out[(s*NCHA+c)*2+1]=rdmsr(s,CTR(c,1));
        }
}

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: %s <out.csv> <label> <nfp> [nsamp=40] [interval_us=5000]\n",argv[0]); return 1; }
    const char *out=argv[1];
    int label=atoi(argv[2]);
    int nfp=atoi(argv[3]);
    int nsamp=argc>4?atoi(argv[4]):40;
    int interval_us=argc>5?atoi(argv[5]):5000;
    int NF = NSCK*NCHA*2;

    for(int s=0;s<NSCK;s++){ char p[64]; snprintf(p,sizeof p,"/dev/cpu/%d/msr",sck_cpu[s]);
        fd[s]=open(p,O_RDWR); if(fd[s]<0){ perror(p); fprintf(stderr,"run as root, server stopped\n"); return 2; } }
    for(int s=0;s<NSCK;s++) prog_socket(s);

    FILE *f=fopen(out,"a"); if(!f){ perror(out); return 3; }
    uint64_t *prev=malloc(sizeof(uint64_t)*NF), *cur=malloc(sizeof(uint64_t)*NF);
    long *samp=malloc(sizeof(long)*nsamp*NF);
    struct timespec iv={ interval_us/1000000, (long)(interval_us%1000000)*1000 };

    for(int fpi=0; fpi<nfp; fpi++){
        snapshot(prev);
        for(int t=0;t<nsamp;t++){
            nanosleep(&iv,NULL);
            snapshot(cur);
            for(int k=0;k<NF;k++){ long d=(long)(cur[k]-prev[k]); samp[t*NF+k]= d<0?0:d; }
            uint64_t *tmp=prev; prev=cur; cur=tmp;
        }
        fprintf(f,"%d,%d",label,fpi);
        for(int i=0;i<nsamp*NF;i++) fprintf(f,",%ld",samp[i]);
        fprintf(f,"\n");
        fflush(f);
    }
    fclose(f);
    fprintf(stderr,"[monitor] wrote %d fingerprints (label=%d, %d samples x %d counters) -> %s\n",
            nfp,label,nsamp,NF,out);
    return 0;
}
