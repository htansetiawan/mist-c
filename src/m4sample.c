#define _POSIX_C_SOURCE 199309L
#include "m4sample.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct {int index, count;double weight;} Witness;
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
static int by_index(const void *a,const void *b){const Witness *x=a,*y=b;return (x->index>y->index)-(x->index<y->index);}
static int by_coverage(const void *a,const void *b){
    const Witness *x=a,*y=b;
    if(x->count!=y->count)return y->count-x->count;
    if(x->weight!=y->weight)return x->weight>y->weight?-1:1;
    return by_index(a,b);
}
int m4_witness_sample(const Belief *b,const MoveList *moves,const unsigned char *known,
                      int limit,double deadline,int *indices,M4WitnessStats *stats) {
    if(!stats)return 0;
    memset(stats,0,sizeof(*stats));
    if(!b||!moves||!indices||limit<=0||b->count>INT_MAX||moves->count<1||moves->count>256)return 0;
    int best[256],uncovered=0;double weight[256];
    for(int a=0;a<moves->count;++a){best[a]=-1;weight[a]=-INFINITY;if(!known||!known[a])++uncovered;}
    if(!uncovered){stats->complete=1;return 0;}
    for(size_t i=0;i<b->count;++i) {
        if((i&63)==0&&deadline>0&&seconds()>=deadline)break;
        stats->scanned=i+1;double lw=b->logw?b->logw[i]:0;
        if(!isfinite(lw))continue;
        for(int a=0;a<moves->count;++a) {
            if((known&&known[a])||lw<=weight[a])continue;
            Position p=b->items[i];Undo u;make_move(&p,moves->moves[a],&u);
            if(king_square(&p,b->us^1)>=0&&mover_left_king_capturable(&p)){best[a]=(int)i;weight[a]=lw;}
        }
    }
    stats->complete=stats->scanned==b->count;
    Witness candidates[256];int n=0;
    for(int a=0;a<moves->count;++a)if(best[a]>=0)candidates[n++]=(Witness){best[a],1,weight[a]};
    qsort(candidates,(size_t)n,sizeof(*candidates),by_index);
    int unique=0;
    for(int i=0;i<n;++i) {
        if(unique&&candidates[i].index==candidates[unique-1].index)++candidates[unique-1].count;
        else candidates[unique++]=candidates[i];
    }
    qsort(candidates,(size_t)unique,sizeof(*candidates),by_coverage);
    if(unique>limit)unique=limit;
    for(int i=0;i<unique;++i){indices[i]=candidates[i].index;stats->represented_moves+=candidates[i].count;}
    stats->witnesses=unique;return unique;
}
