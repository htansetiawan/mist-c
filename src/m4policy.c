#include "m4policy.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {double risk,value;int action;} Action;
static int order(const void *aa,const void *bb) {
    const Action *a=aa,*b=bb;
    if(a->risk!=b->risk)return (a->risk>b->risk)-(a->risk<b->risk);
    if(a->value!=b->value)return (b->value>a->value)-(b->value<a->value);
    return (a->action>b->action)-(a->action<b->action);
}
static int distribution(const double *p,int n) {
    if(!p||n<1||n>256)return 0;
    double sum=0;for(int a=0;a<n;++a){if(!isfinite(p[a])||p[a]<0)return 0;sum+=p[a];}
    return fabs(sum-1)<1e-9;
}
int m4_policy_redirect(const double *risk,const double *value,const double *play,
                       int n,double *out,int *destination) {
    if(!risk||!value||!out||!destination||!distribution(play,n))return -1;
    Action sorted[256],frontier[256];int used=0,map[256],changed=0;double policy[256]={0};
    for(int a=0;a<n;++a) {
        if(!isfinite(risk[a])||risk[a]<0||risk[a]>1||!isfinite(value[a]))return -1;
        sorted[a]=(Action){risk[a],value[a],a};
    }
    qsort(sorted,(size_t)n,sizeof(*sorted),order);
    for(int a=0;a<n;++a)if(!used||sorted[a].value>frontier[used-1].value)frontier[used++]=sorted[a];
    for(int a=0;a<n;++a) {
        int lo=0,hi=used;
        while(lo<hi){int mid=lo+(hi-lo)/2;if(frontier[mid].value<value[a])lo=mid+1;else hi=mid;}
        map[a]=lo<used&&frontier[lo].risk<risk[a]-1e-9?frontier[lo].action:a;
        policy[map[a]]+=play[a];changed+=play[a]>0&&map[a]!=a;
    }
    memcpy(out,policy,(size_t)n*sizeof(double));memcpy(destination,map,(size_t)n*sizeof(int));return changed;
}
int m4_policy_retarget(KlussBlueprint *b,int I,const double *play,double *v,double *cf,double *op) {
    if(!b||!b->valid||!v||!cf||!op||I<0||I>=b->tree.n_infosets||b->tree.is_player[I]!=0)return 0;
    int n=b->tree.is_actions[I],offset=b->tree.is_offset[I];if(!distribution(play,n))return 0;
    double old[256];memcpy(old,b->strategy+offset,(size_t)n*sizeof(double));
    memcpy(b->strategy+offset,play,(size_t)n*sizeof(double));
    int ok=kluss_blueprint_stats(b,v,cf,op);double mass=0,expected=0;
    if(ok)for(int r=0;r<b->boundary_count;++r) {
        const GadgetBoundary *root=&b->boundary[r];double w=root->cf_reach*root->opponent_reach;
        mass+=w;expected+=w*v[root->node];
    }
    if(!ok||!(mass>0)||!isfinite(expected)) {
        memcpy(b->strategy+offset,old,(size_t)n*sizeof(double));return 0;
    }
    b->expected_value=expected/mass;b->active_mass=mass;return 1;
}
