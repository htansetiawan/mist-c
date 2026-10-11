#include "kluss.h"
#include "gadget.h"
#include <stdlib.h>
#include <string.h>

static int compare_key(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
int kluss_select(const KlussFrontier *f,int n,uint64_t current,unsigned char *keep,int *dist) {
    if(!f || n<=0 || !keep || !dist)return -1;
    uint64_t *keys=malloc((size_t)n*sizeof(uint64_t));if(!keys)return -1;
    int used=0;
    for(int i=0;i<n;++i) {
        keep[i]=0;dist[i]=-1;
        if(f[i].node<0){free(keys);return -1;}
        if(f[i].ours==current)keys[used++]=f[i].theirs;
    }
    if(!used){free(keys);return -1;}
    qsort(keys,(size_t)used,sizeof(uint64_t),compare_key);
    int count=0;
    for(int i=0;i<n;++i) {
        if(f[i].ours==current)dist[i]=0;
        else if(bsearch(&f[i].theirs,keys,(size_t)used,sizeof(uint64_t),compare_key))dist[i]=1;
        if(dist[i]>=0){keep[i]=1;++count;}
    }
    free(keys);return count;
}
void kluss_blueprint_init(KlussBlueprint *b){memset(b,0,sizeof(*b));game_init(&b->tree);}
void kluss_blueprint_free(KlussBlueprint *b){game_free(&b->tree);free(b->strategy);free(b->ours);free(b->theirs);kluss_blueprint_init(b);}

int kluss_blueprint_capture(KlussBlueprint *b,const Game *g,const double *x,
                            const uint64_t *ours,const uint64_t *theirs) {
    if(!b || !g || !x || !ours || !theirs || g->n_nodes<=0 || g->n_infosets<0)return 0;
    double *value=malloc((size_t)g->n_nodes*sizeof(double));
    double *cf=malloc((size_t)g->n_nodes*sizeof(double)),*opp=malloc((size_t)g->n_nodes*sizeof(double));
    if(!value||!cf||!opp){free(value);free(cf);free(opp);return 0;}
    int valid=gadget_blueprint_stats(g,x,value,cf,opp);
    double expected=valid?value[g->root]:0;free(value);free(cf);free(opp);
    if(!valid)return 0;
    KlussBlueprint fresh;kluss_blueprint_init(&fresh);
    fresh.strategy=malloc((size_t)(g->n_slots+1)*sizeof(double));
    fresh.ours=malloc((size_t)g->n_nodes*sizeof(uint64_t));fresh.theirs=malloc((size_t)g->n_nodes*sizeof(uint64_t));
    if(!fresh.strategy||!fresh.ours||!fresh.theirs){kluss_blueprint_free(&fresh);return 0;}
    for(int i=0;i<g->n_infosets;++i)game_infoset(&fresh.tree,g->is_player[i],g->is_actions[i]);
    for(int i=0;i<g->n_nodes;++i) {
        const GameNode *n=&g->nodes[i];game_node(&fresh.tree,n->type,n->player,n->infoset,n->value);
    }
    for(int i=0;i<g->n_nodes;++i) {
        const GameNode *n=&g->nodes[i];
        if(n->count)game_children(&fresh.tree,i,g->child+n->first,g->prob+n->first,n->count);
    }
    fresh.tree.root=g->root;game_finalize(&fresh.tree);
    memcpy(fresh.strategy,x,(size_t)g->n_slots*sizeof(double));
    memcpy(fresh.ours,ours,(size_t)g->n_nodes*sizeof(uint64_t));memcpy(fresh.theirs,theirs,(size_t)g->n_nodes*sizeof(uint64_t));
    fresh.expected_value=expected;fresh.valid=1;
    kluss_blueprint_free(b);*b=fresh;return 1;
}
