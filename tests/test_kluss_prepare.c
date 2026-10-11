#include "../src/kluss.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void near(double a,double b){assert(isfinite(a)&&fabs(a-b)<1e-10);}
static int term(Game *g,double v){return game_node(g,NODE_TERMINAL,0,-1,v);}
static int choice(Game *g,int I,double a,double b) {
    int h=game_node(g,NODE_PLAYER,0,I,0),kids[2]={term(g,a),term(g,b)};
    game_children(g,h,kids,NULL,2);return h;
}
static void capture(KlussBlueprint *b,Game *s,const double *x,const KlussFrontier *f,int n) {
    uint64_t *ours=calloc((size_t)s->n_nodes,sizeof(uint64_t)),*theirs=calloc((size_t)s->n_nodes,sizeof(uint64_t));
    assert(ours&&theirs);
    for(int i=0;i<n;++i){ours[f[i].node]=f[i].ours;theirs[f[i].node]=f[i].theirs;}
    assert(kluss_blueprint_capture(b,s,x,ours,theirs));free(ours);free(theirs);
}
static void closure_and_gift(void) {
    Game s;game_init(&s);int J=game_infoset(&s,1,2),I0=game_infoset(&s,0,2),I1=game_infoset(&s,0,2);
    int root=game_node(&s,NODE_PLAYER,1,J,0),c=game_node(&s,NODE_CHANCE,0,-1,0);
    int a=choice(&s,I0,0,.4),b=choice(&s,I1,.4,.8),other=choice(&s,I1,-.4,-.4);
    int worlds[2]={a,b},actions[2]={c,other};double p[2]={.6,.4};
    game_children(&s,c,worlds,p,2);game_children(&s,root,actions,NULL,2);game_finalize(&s);
    double *x=malloc((size_t)s.n_slots*sizeof(double));assert(x);pcfr_current(&s,x);
    /* a~opponent b~ours other; order-two closure includes b, but not other. */
    KlussFrontier f[3]={{a,11,100},{b,12,100},{other,12,200}};
    KlussBlueprint blueprint;kluss_blueprint_init(&blueprint);capture(&blueprint,&s,x,f,3);
    KlussPrepared prepared;kluss_prepared_init(&prepared);
    assert(kluss_prepare(&blueprint,f,3,11,&prepared));
    assert(prepared.root_count==2&&prepared.group_count==1&&prepared.current_count==1);
    assert(prepared.distance[0]==0&&prepared.distance[1]==1);
    assert(prepared.frontier_index[0]==0&&prepared.frontier_index[1]==1);
    assert(prepared.opponent_keys[0]==100);
    near(prepared.roots[0].cf_reach,.6);near(prepared.roots[1].cf_reach,.4);
    near(prepared.groups[0].alternate,.36);near(prepared.groups[0].gift,.38);
    near(prepared.groups[0].opponent_reach,.5);near(prepared.gadget.offset[0],-.02);
    assert(prepared.gadget.node_map[other]==-1);
    int retained=prepared.gadget.node_map[b];assert(retained>=0&&prepared.gadget.game.nodes[retained].type==NODE_PLAYER);
    double *gx=malloc((size_t)prepared.gadget.game.n_slots*sizeof(double));assert(gx);pcfr_current(&prepared.gadget.game,gx);
    int I=prepared.gadget.infoset_map[I1],o=prepared.gadget.game.is_offset[I];near(gx[o],.5);
    for(int i=0;i<prepared.gadget.game.n_slots;++i){assert(prepared.gadget.game.regret[i]==0);assert(prepared.gadget.game.strategy_sum[i]==0);}
    near(prepared.gadget.game.last[o],.5);
    /* Invalid metadata and missing I preserve the previously prepared game. */
    GameNode *saved=prepared.gadget.game.nodes;KlussFrontier bad[3]={f[0],f[1],f[2]};bad[1].theirs=999;
    assert(!kluss_prepare(&blueprint,bad,3,11,&prepared));assert(prepared.gadget.game.nodes==saved);
    assert(!kluss_prepare(&blueprint,f,3,777,&prepared));assert(prepared.gadget.game.nodes==saved);
    bad[0]=f[0];bad[1]=f[0];bad[2]=f[2];assert(!kluss_prepare(&blueprint,bad,3,11,&prepared));assert(prepared.gadget.game.nodes==saved);
    /* Even an opponent action with zero blueprint probability retains its
     * CF support; a newly observed opponent deviation is not impossible. */
    x[s.is_offset[J]]=0;x[s.is_offset[J]+1]=1;capture(&blueprint,&s,x,f,3);
    assert(kluss_prepare(&blueprint,f,3,11,&prepared));near(prepared.groups[0].opponent_reach,0);
    near(prepared.groups[0].gift,.76);near(prepared.gadget.offset[0],-.4);near(prepared.gadget.alpha[0],1);
    free(gx);free(x);kluss_prepared_free(&prepared);kluss_blueprint_free(&blueprint);game_free(&s);
    puts("blueprint preparation: KLUSS selection, grouped alternates, gifts, fresh seeds and atomic failure passed");
}
static void shared_infoset_gift(void) {
    /* Two hidden worlds share a P1 decision. A gift must be computed after
     * CF aggregation at J; per-board conditional gifts (.75,1.5) are wrong. */
    Game s;game_init(&s);int J=game_infoset(&s,1,2),I0=game_infoset(&s,0,2),I1=game_infoset(&s,0,2);
    int root=game_node(&s,NODE_CHANCE,0,-1,0),parents[2],nodes[2];double p[2]={.4,.6};
    for(int k=0;k<2;++k) {
        double v=k?1:.5;
        parents[k]=game_node(&s,NODE_PLAYER,1,J,0);nodes[k]=choice(&s,k?I1:I0,v,v);
        int actions[2]={nodes[k],term(&s,-v)};game_children(&s,parents[k],actions,NULL,2);
    }
    game_children(&s,root,parents,p,2);game_finalize(&s);
    double *x=malloc((size_t)s.n_slots*sizeof(double));assert(x);pcfr_current(&s,x);x[s.is_offset[J]]=.25;x[s.is_offset[J]+1]=.75;
    KlussFrontier f[2]={{nodes[0],11,100},{nodes[1],12,100}};
    KlussBlueprint blueprint;kluss_blueprint_init(&blueprint);capture(&blueprint,&s,x,f,2);
    KlussPrepared prepared;kluss_prepared_init(&prepared);assert(kluss_prepare(&blueprint,f,2,11,&prepared));
    near(prepared.groups[0].alternate,.8);near(prepared.groups[0].gift,1.2);near(prepared.gadget.offset[0],-.4);
    near(prepared.groups[0].opponent_reach,.25);near(prepared.roots[0].cf_reach,.4);near(prepared.roots[1].cf_reach,.6);
    kluss_prepared_free(&prepared);kluss_blueprint_free(&blueprint);free(x);game_free(&s);
    puts("blueprint gifts: shared opponent infoset aggregates CF values before path accumulation");
}
int main(void){closure_and_gift();shared_infoset_gift();puts("test_kluss_prepare: passed");return 0;}
