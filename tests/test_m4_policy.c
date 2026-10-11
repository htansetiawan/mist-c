#include "../src/m4policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void near(double a,double b){assert(isfinite(a)&&fabs(a-b)<1e-10);}
static void reference(const double *risk,const double *value,int n,int *map) {
    for(int a=0;a<n;++a) {
        int best=-1;
        for(int b=0;b<n;++b)if(risk[b]<risk[a]-1e-9&&value[b]>=value[a]&&
            (best<0||risk[b]<risk[best]||(risk[b]==risk[best]&&
                (value[b]>value[best]||(value[b]==value[best]&&b<best)))))best=b;
        map[a]=best<0?a:best;
    }
}
static void selector(void) {
    double risk[256]={.678431373,.301960784,.9},value[256]={-.971052423,-.970813019,-.9708};
    double p[256]={1,0,0},out[256];int map[256],ref[256];
    assert(m4_policy_redirect(risk,value,p,3,out,map)==1);near(out[1],1);assert(map[0]==1);
    /* Strict risk reduction, no value regression, deterministic ties. */
    for(int trial=1;trial<=200;++trial) {
        int n=1+trial%256;double total=0;
        for(int a=0;a<n;++a){risk[a]=(double)(rand()%20)/20;value[a]=(double)(rand()%20-10)/10;p[a]=rand()%7;total+=p[a];}
        if(!total){p[0]=total=1;}
        for(int a=0;a<n;++a)p[a]/=total;
        reference(risk,value,n,ref);int changes=m4_policy_redirect(risk,value,p,n,out,map);assert(changes>=0);
        double before_risk=0,after_risk=0,before_value=0,after_value=0,mass=0;
        for(int a=0;a<n;++a){assert(map[a]==ref[a]);mass+=out[a];before_risk+=p[a]*risk[a];after_risk+=out[a]*risk[a];before_value+=p[a]*value[a];after_value+=out[a]*value[a];}
        near(mass,1);assert(after_risk<=before_risk+1e-12);assert(after_value>=before_value-1e-12);
    }
    risk[0]=.5;risk[1]=.5-5e-10;value[0]=value[1]=0;p[0]=1;p[1]=0;
    assert(m4_policy_redirect(risk,value,p,2,out,map)==0);near(out[0],1);
    out[0]=123;map[0]=123;risk[1]=NAN;
    assert(m4_policy_redirect(risk,value,p,2,out,map)==-1);near(out[0],123);assert(map[0]==123);
    assert(m4_policy_redirect(risk,value,p,257,out,map)==-1);
}
static int leaf(Game *g,double value){return game_node(g,NODE_TERMINAL,0,-1,value);}
static void saved_zero_support(void) {
    Game g;game_init(&g);int I=game_infoset(&g,0,2),J=game_infoset(&g,1,2);
    int root=game_node(&g,NODE_CHANCE,0,-1,0),world[2],future[4];
    for(int w=0;w<2;++w) {
        world[w]=game_node(&g,NODE_PLAYER,0,I,0);
        int reply=game_node(&g,NODE_PLAYER,1,J,0);
        future[2*w]=leaf(&g,.4+w*.1);future[2*w+1]=leaf(&g,.8+w*.1);
        game_children(&g,reply,future+2*w,NULL,2);
        int actions[2]={leaf(&g,-1),reply};game_children(&g,world[w],actions,NULL,2);
    }
    double weights[2]={.7,.3};game_children(&g,root,world,weights,2);game_finalize(&g);
    double *x=calloc((size_t)g.n_slots,sizeof(double));int o=g.is_offset[I],j=g.is_offset[J];
    x[o]=1;x[j]=x[j+1]=.5;
    uint64_t *ours=calloc((size_t)g.n_nodes,sizeof(uint64_t)),*theirs=calloc((size_t)g.n_nodes,sizeof(uint64_t));
    KlussFrontier frontier[4];
    for(int k=0;k<4;++k){ours[future[k]]=99;theirs[future[k]]=100+k;frontier[k]=(KlussFrontier){future[k],99,100+k};}
    KlussBlueprint b;kluss_blueprint_init(&b);assert(kluss_blueprint_capture(&b,&g,x,ours,theirs));
    double *v=calloc((size_t)g.n_nodes,sizeof(double)),*cf=calloc((size_t)g.n_nodes,sizeof(double)),*op=calloc((size_t)g.n_nodes,sizeof(double));
    assert(kluss_blueprint_stats(&b,v,cf,op));near(cf[future[0]],0);near(b.expected_value,-1);
    double played[2]={0,1};assert(m4_policy_retarget(&b,I,played,v,cf,op));
    near(x[o],1);near(b.strategy[o],0);near(b.strategy[o+1],1);near(b.expected_value,.63);
    for(int k=0;k<4;++k){near(cf[future[k]],k<2?.7:.3);near(op[future[k]],.5);}
    KlussPrepared prepared;kluss_prepared_init(&prepared);assert(kluss_prepare(&b,frontier,4,99,&prepared));
    assert(prepared.current_count==4);
    played[0]=NAN;assert(!m4_policy_retarget(&b,I,played,v,cf,op));near(b.strategy[o+1],1);
    assert(!m4_policy_retarget(&b,J,(double[]){1,0},v,cf,op));
    free(x);free(ours);free(theirs);free(v);free(cf);free(op);kluss_prepared_free(&prepared);kluss_blueprint_free(&b);game_free(&g);
}
int main(void){selector();saved_zero_support();puts("test_m4_policy: sorted frontier matches exhaustive reference; raw value preserved; zero-support redirect and saved continuation reach agree");return 0;}
