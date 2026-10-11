#include "kluss.h"
#include "gadget.h"
#include <math.h>
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

void kluss_prepared_init(KlussPrepared *p){memset(p,0,sizeof(*p));game_init(&p->gadget.game);}
void kluss_prepared_free(KlussPrepared *p) {
    gadget_free(&p->gadget);free(p->roots);free(p->groups);free(p->opponent_keys);
    free(p->frontier_index);free(p->distance);kluss_prepared_init(p);
}
typedef struct {uint64_t key;int frontier;} FrontierKey;
static int compare_frontier(const void *aa,const void *bb) {
    const FrontierKey *a=aa,*b=bb;
    if(a->key!=b->key)return (a->key>b->key)-(a->key<b->key);
    return (a->frontier>b->frontier)-(a->frontier<b->frontier);
}

/* The gift at an opponent history is a sum over its own preceding actions.
 * First aggregate CF action values at INFORMATION SETS (not individual
 * boards), then accumulate the positive P0-value increments down each path.
 * blueprint_stats has already validated the reachable tree. */
static int blueprint_gifts(const Game *g,const double *x,const double *value,
                           const double *cf,double *gift) {
    double *iv=calloc((size_t)(g->n_infosets+1),sizeof(double));
    double *av=calloc((size_t)(g->n_slots+1),sizeof(double));
    int *queue=malloc((size_t)g->n_nodes*sizeof(int));
    if(!iv||!av||!queue){free(iv);free(av);free(queue);return 0;}
    for(int h=0;h<g->n_nodes;++h) {
        const GameNode *n=&g->nodes[h];
        if(cf[h]==0||n->type!=NODE_PLAYER||n->player!=1)continue;
        int I=n->infoset,o=g->is_offset[I];
        for(int a=0;a<n->count;++a)av[o+a]+=cf[h]*value[g->child[n->first+a]];
    }
    for(int I=0;I<g->n_infosets;++I)if(g->is_player[I]==1) {
        int o=g->is_offset[I];
        for(int a=0;a<g->is_actions[I];++a)iv[I]+=x[o+a]*av[o+a];
    }
    memset(gift,0,(size_t)g->n_nodes*sizeof(double));
    queue[0]=g->root;int count=1,ok=1;
    for(int k=0;k<count&&ok;++k) {
        int h=queue[k];const GameNode *n=&g->nodes[h];
        for(int a=0;a<n->count;++a) {
            int child=g->child[n->first+a];double extra=0;
            if(n->type==NODE_PLAYER&&n->player==1)extra=fmax(0,av[g->is_offset[n->infoset]+a]-iv[n->infoset]);
            gift[child]=gift[h]+extra;
            if(!isfinite(gift[child])){ok=0;break;}
            queue[count++]=child;
        }
    }
    free(iv);free(av);free(queue);return ok;
}

int kluss_prepare(const KlussBlueprint *b,const KlussFrontier *f,int n,
                  uint64_t current,KlussPrepared *out) {
    if(!b||!b->valid||!f||n<=0||!out)return 0;
    const Game *g=&b->tree;int ok=0,kept=0;
    KlussPrepared fresh;kluss_prepared_init(&fresh);
    unsigned char *keep=malloc((size_t)n),*seen=NULL;
    int *dist=malloc((size_t)n*sizeof(int)),*group=malloc((size_t)n*sizeof(int));
    FrontierKey *keys=malloc((size_t)n*sizeof(FrontierKey));
    double *value=malloc((size_t)g->n_nodes*sizeof(double)),*cf=malloc((size_t)g->n_nodes*sizeof(double));
    double *opp=malloc((size_t)g->n_nodes*sizeof(double)),*gift=malloc((size_t)g->n_nodes*sizeof(double)),*mass=NULL;
    if(!keep||!dist||!group||!keys||!value||!cf||!opp||!gift)goto done;
    for(int i=0;i<n;++i) {
        int h=f[i].node;
        if(h<0||h>=g->n_nodes||b->ours[h]!=f[i].ours||b->theirs[h]!=f[i].theirs)goto done;
    }
    kept=kluss_select(f,n,current,keep,dist);if(kept<=0)goto done;
    if(!gadget_blueprint_stats(g,b->strategy,value,cf,opp)||!blueprint_gifts(g,b->strategy,value,cf,gift))goto done;
    int k=0;
    for(int i=0;i<n;++i)if(keep[i])keys[k++]=(FrontierKey){f[i].theirs,i};
    qsort(keys,(size_t)kept,sizeof(FrontierKey),compare_frontier);
    int ng=0;
    for(int i=0;i<kept;++i) {
        if(i==0||keys[i].key!=keys[i-1].key)++ng;
        group[keys[i].frontier]=ng-1;
    }
    fresh.roots=malloc((size_t)kept*sizeof(GadgetRoot));fresh.groups=calloc((size_t)ng,sizeof(GadgetGroup));
    fresh.opponent_keys=malloc((size_t)ng*sizeof(uint64_t));fresh.frontier_index=malloc((size_t)kept*sizeof(int));
    fresh.distance=malloc((size_t)kept*sizeof(int));mass=calloc((size_t)ng,sizeof(double));seen=calloc((size_t)ng,1);
    if(!fresh.roots||!fresh.groups||!fresh.opponent_keys||!fresh.frontier_index||!fresh.distance||!mass||!seen)goto done;
    for(int i=0;i<n;++i)if(keep[i]) {
        int h=f[i].node,j=group[i],r=fresh.root_count++;
        fresh.roots[r]=(GadgetRoot){h,j,cf[h]};fresh.frontier_index[r]=i;fresh.distance[r]=dist[i];
        fresh.current_count+=dist[i]==0;
        GadgetGroup *gj=&fresh.groups[j];
        if(!seen[j]) {seen[j]=1;gj->gift=gift[h];gj->opponent_reach=opp[h];fresh.opponent_keys[j]=f[i].theirs;}
        else if(fabs(gj->gift-gift[h])>1e-10*fmax(1,gj->gift)
             ||fabs(gj->opponent_reach-opp[h])>1e-10*fmax(1,gj->opponent_reach))goto done;
        mass[j]+=cf[h];gj->alternate+=cf[h]*value[h];
    }
    fresh.group_count=ng;
    for(int j=0;j<ng;++j) {
        if(!(mass[j]>0)||!isfinite(mass[j])||!isfinite(fresh.groups[j].alternate))goto done;
        fresh.groups[j].alternate/=mass[j];
    }
    if(!gadget_build(g,fresh.roots,kept,fresh.groups,ng,&fresh.gadget))goto done;
    if(!gadget_seed_blueprint(&fresh.gadget,g,b->strategy))goto done;
    kluss_prepared_free(out);*out=fresh;kluss_prepared_init(&fresh);ok=1;
done:
    kluss_prepared_free(&fresh);free(keep);free(dist);free(group);free(keys);
    free(value);free(cf);free(opp);free(gift);free(mass);free(seen);return ok;
}
