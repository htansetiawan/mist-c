#include "../src/m4resample.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {double key;size_t i;} Reference;
static int cmp(const void *a,const void *b){const Reference *x=a,*y=b;
    if(x->key!=y->key)return x->key>y->key?1:-1;
    return (x->i>y->i)-(x->i<y->i);}
static double random_open(uint64_t *s) {
    uint64_t z=(*s+=0x9E3779B97F4A7C15ULL);z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;
    z=(z^(z>>27))*0x94D049BB133111EBULL;z^=z>>31;
    double u=((double)(z>>11)+.5)/9007199254740992.;return u<1?u:nextafter(1.,0.);
}
int main(void) {
    float w[137],out[137];size_t indices[137];uint64_t a=981,b=a;
    for(int i=0;i<137;++i)w[i]=-(float)(i%17)/3;
    Reference r[137];
    for(int i=0;i<137;++i)r[i]=(Reference){log(-log(random_open(&a)))-w[i],(size_t)i};
    qsort(r,137,sizeof(*r),cmp);assert(m4_weighted_cap(w,137,7,&b,indices,out));assert(a==b);
    for(int i=0;i<7;++i){assert(indices[i]==r[i].i);
        double expected=w[indices[i]]-log(-expm1(-exp(w[indices[i]]+r[7].key)));
        assert(fabs(out[i]-expected)<1e-6);}
    a=b;assert(m4_weighted_cap(w,137,137,&b,indices,out));assert(a==b);
    for(int i=0;i<137;++i)assert(indices[i]==(size_t)i&&out[i]==w[i]);
    assert(m4_weighted_cap(NULL,137,7,&b,indices,out));
    for(int i=1;i<7;++i)assert(out[i]==out[0]);
    w[5]=NAN;assert(!m4_weighted_cap(w,137,7,&b,indices,out));
    assert(!m4_weighted_cap(NULL,137,0,&b,indices,out));
    /* Exact expectation of old self-normalized weights: 352/405. */
    float small[3]={logf(.8f),logf(.1f),logf(.1f)};double old=0,corrected=0,ratio=0;
    b=192;
    for(int t=0;t<200000;++t) {
        assert(m4_weighted_cap(small,3,2,&b,indices,out));
        double total=0,selected=0,old_total=0,old_selected=0;
        for(int j=0;j<2;++j){double mass=exp(out[j]);total+=mass;old_total+=exp(small[indices[j]]);
            if(indices[j]==0){selected+=mass;old_selected+=exp(small[indices[j]]);}}
        old+=old_selected/old_total;corrected+=selected;ratio+=selected/total;
    }
    old/=200000;corrected/=200000;ratio/=200000;
    assert(fabs(old-.869135802469)<.002);assert(fabs(corrected-.8)<.003);
    printf("test_m4_resample: heap matches full sort, threshold weights, exact/no-cap and invalid inputs passed; old=%.6f corrected=%.6f normalized=%.6f\n",old,corrected,ratio);
}
