/* Kool et al., ICML 2019, Sec.4.2 equations 13/14; exponential-race form.
 * https://proceedings.mlr.press/v97/kool19a/kool19a.pdf
 * Retain k+1 smallest races, use the (k+1)th as the inclusion threshold.
 * Does not alter belief.c or its default sampling. */
#include "m4resample.h"
#include <math.h>
#include <stdlib.h>

typedef struct {double key;size_t index;} Ranked;
static int worse(Ranked a,Ranked b){return a.key>b.key||(a.key==b.key&&a.index>b.index);}
static int compare(const void *a,const void *b){Ranked x=*(const Ranked*)a,y=*(const Ranked*)b;return worse(x,y)-worse(y,x);}
static double unit(uint64_t *s) {
    uint64_t z=(*s+=0x9E3779B97F4A7C15ULL);
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;z=(z^(z>>27))*0x94D049BB133111EBULL;z^=z>>31;
    double u=((double)(z>>11)+.5)*(1.0/9007199254740992.0);
    return u<1?u:nextafter(1.,0.);
}
static void up(Ranked *h,size_t i) {
    while(i){size_t parent=(i-1)/2;if(!worse(h[i],h[parent]))break;
        Ranked swap=h[i];h[i]=h[parent];h[parent]=swap;i=parent;}
}
static void down(Ranked *h,size_t n) {
    size_t i=0;
    while(i<n/2){size_t child=2*i+1;if(child+1<n&&worse(h[child+1],h[child]))++child;
        if(!worse(h[child],h[i]))break;
        Ranked swap=h[i];h[i]=h[child];h[child]=swap;i=child;}
}
int m4_weighted_cap(const float *logw,size_t n,size_t keep,uint64_t *rng,size_t *indices,float *out) {
    if(!keep||!rng||(!indices&&n)||(!out&&n))return 0;
    if(logw)for(size_t i=0;i<n;++i)if(!isfinite(logw[i]))return 0;
    if(n<=keep){for(size_t i=0;i<n;++i){indices[i]=i;out[i]=logw?logw[i]:0;}return 1;}
    size_t capacity=keep+1;
    if(capacity>SIZE_MAX/sizeof(Ranked))return 0;
    Ranked *heap=malloc(capacity*sizeof(*heap));if(!heap)return 0;
    size_t count=0;
    for(size_t i=0;i<n;++i) {
        Ranked r={log(-log(unit(rng)))-(logw?logw[i]:0),i};
        if(count<capacity){heap[count]=r;up(heap,count++);}
        else if(worse(heap[0],r)){heap[0]=r;down(heap,count);}
    }
    qsort(heap,capacity,sizeof(*heap),compare);double threshold=heap[keep].key;
    for(size_t i=0;i<keep;++i) {
        size_t index=heap[i].index;double lw=logw?logw[index]:0,x=lw+threshold;
        /* log(1-exp(-exp(x))) is x for tiny exp(x), and 0 at the other tail. */
        double corrected=x<-18?-threshold:lw-(x>6?0:log(-expm1(-exp(x))));
        indices[i]=index;out[i]=(float)corrected;
    }
    free(heap);return 1;
}
