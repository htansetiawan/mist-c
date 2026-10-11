/* Figure 10: analytic checks for the shared Resolve/Maxmargin controller.
 * The fixed-gadget Nash-gap tests live in test_gadget.c. This adaptive
 * controller is not itself a single stationary game. */
#include "../src/gadget.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void close_to(double actual,double expected,double tol) {
    if(fabs(actual-expected)>tol)fprintf(stderr,"actual %.12g expected %.12g tolerance %.3g\n",actual,expected,tol);
    assert(isfinite(actual)&&fabs(actual-expected)<=tol);
}
static int terminal(Game *s,double v){return game_node(s,NODE_TERMINAL,0,-1,v);}
static void matrix(Game *s,const double pay[2][2],Gadget *g) {
    game_init(s);int I=game_infoset(s,0,2),r=game_node(s,NODE_CHANCE,0,-1,0),nodes[2];
    GadgetRoot roots[2];GadgetGroup groups[2]={{0,0,.99},{0,0,.01}};
    for(int j=0;j<2;++j) {
        nodes[j]=game_node(s,NODE_PLAYER,0,I,0);
        int kids[2]={terminal(s,pay[j][0]),terminal(s,pay[j][1])};
        game_children(s,nodes[j],kids,NULL,2);roots[j]=(GadgetRoot){nodes[j],j,j==0?.99:.01};
    }
    double p[2]={.99,.01};game_children(s,r,nodes,p,2);game_finalize(s);
    assert(gadget_build(s,roots,2,groups,2,g));
}
static void policy(Game *g,int I,double p) {
    assert(g->is_actions[I]==2);int o=g->is_offset[I];
    g->regret[o]=p;g->regret[o+1]=1-p;g->last[o]=g->last[o+1]=0;
}
static double probability(const Gadget *g,int I) {
    double *x=malloc((size_t)g->game.n_slots*sizeof(double));assert(x);pcfr_current(&g->game,x);
    double p=x[g->game.is_offset[I]];free(x);return p;
}
static void arithmetic(void) {
    const double pay[2][2]={{.2,-.6},{-.8,.1}};Game s;Gadget g;matrix(&s,pay,&g);
    Game *tree=&g.game;int own=g.infoset_map[0];
    policy(tree,own,.5);policy(tree,g.selector,.7);
    policy(tree,g.entry_infoset[0],.9);policy(tree,g.entry_infoset[1],.2);
    double *x=malloc((size_t)tree->n_slots*sizeof(double)),w[2];assert(x);pcfr_current(tree,x);
    close_to(gadget_root_weights(&g,x,w),.8,1e-12);
    close_to(w[0],.1996,1e-12);close_to(w[1],.2232,1e-12);
    close_to(w[0]+w[1],.4228,1e-12); /* MUST NOT be normalized to one. */
    GadgetSolver solver;gadget_solver_init(&solver);assert(gadget_iteration(&g,1,&solver));
    int o=tree->is_offset[own];
    close_to(tree->last[o],-.0206,1e-12);close_to(tree->last[o+1],.0206,1e-12);
    close_to(probability(&g,own),.4588,1e-12);
    /* Alternating update: P1 sees P0's new prediction, not its old .5. */
    close_to(solver.root_value[0],.23296,1e-12);close_to(solver.root_value[1],.31292,1e-12);
    int q=tree->is_offset[g.selector],e0=tree->is_offset[g.entry_infoset[0]],e1=tree->is_offset[g.entry_infoset[1]];
    close_to(tree->last[q],-.023988,1e-12);close_to(tree->last[q+1],.055972,1e-12);
    close_to(tree->last[e0],-.023296,1e-12);close_to(tree->last[e0+1],.209664,1e-12);
    close_to(tree->last[e1],-.250336,1e-12);close_to(tree->last[e1+1],.062584,1e-12);
    policy(tree,g.entry_infoset[0],1);policy(tree,g.entry_infoset[1],1);policy(tree,g.selector,.7);
    pcfr_current(tree,x);close_to(gadget_root_weights(&g,x,w),0,0);close_to(w[0],.7,1e-12);close_to(w[1],.3,1e-12);
    policy(tree,g.entry_infoset[0],0);policy(tree,g.entry_infoset[1],0);
    pcfr_current(tree,x);close_to(gadget_root_weights(&g,x,w),1,0);close_to(w[0],.745,1e-12);close_to(w[1],.255,1e-12);
    x[q]=NAN;assert(isnan(gadget_root_weights(&g,x,w)));assert(!gadget_iteration(&g,0,&solver));
    free(x);gadget_solver_free(&solver);gadget_free(&g);game_free(&s);
    puts("controller one-iteration oracle: exact nonunit root mass and alternating updates passed");
}
static void equilibria(void) {
    const double cases[3][2][2]={{{.2,-.6},{-.8,.1}},{{1.2,.4},{.2,1.1}},{{.8,0},{-1.2,0}}};
    const double expected[3]={.75,7.0/17,0};
    for(int k=0;k<3;++k) {
        Game s;Gadget g;matrix(&s,cases[k],&g);GadgetSolver solver;gadget_solver_init(&solver);
        for(int t=1;t<=16000;++t)assert(gadget_iteration(&g,t,&solver));
        double p=probability(&g,g.infoset_map[0]);
        printf("adaptive controller case=%d: action0 %.9f target %.9f, max_enter %.6f, active_mass %.6f\n",k,p,expected[k],solver.max_enter,solver.active_mass);
        close_to(p,expected[k],.003);
        if(k==0){close_to(solver.max_enter,1,.001);close_to(solver.active_mass,.541875,.003);}
        if(k==1){close_to(solver.max_enter,0,.001);close_to(solver.active_mass,1,.001);}
        gadget_solver_free(&solver);gadget_free(&g);game_free(&s);
    }
}
static void pruning(void) {
    Game s;game_init(&s);int I0=game_infoset(&s,0,2),I1=game_infoset(&s,0,2),J1=game_infoset(&s,1,2);
    int nodes[2]={game_node(&s,NODE_PLAYER,0,I0,0),game_node(&s,NODE_PLAYER,0,I1,0)};
    int opponent=game_node(&s,NODE_PLAYER,1,J1,0),reply[2]={terminal(&s,-1),terminal(&s,1)};
    game_children(&s,opponent,reply,NULL,2);
    int a[2]={terminal(&s,.1),terminal(&s,.2)},b[2]={opponent,terminal(&s,.4)};
    game_children(&s,nodes[0],a,NULL,2);game_children(&s,nodes[1],b,NULL,2);game_finalize(&s);
    GadgetRoot roots[2]={{nodes[0],0,1},{nodes[1],1,1}};GadgetGroup groups[2]={{0,0,1},{0,0,1}};Gadget g;
    assert(gadget_build(&s,roots,2,groups,2,&g));
    policy(&g.game,g.selector,1);policy(&g.game,g.entry_infoset[0],1);policy(&g.game,g.entry_infoset[1],1);
    int i=g.infoset_map[I1],o=g.game.is_offset[i];
    g.game.regret[o]=3;g.game.regret[o+1]=1;g.game.last[o]=2;g.game.last[o+1]=-1;
    GadgetSolver solver;gadget_solver_init(&solver);assert(gadget_iteration(&g,1,&solver));
    close_to(g.game.regret[o],3,0);close_to(g.game.regret[o+1],1,0);
    close_to(g.game.last[o],2,0);close_to(g.game.last[o+1],-1,0);
    /* J1 has zero current opponent-root reach; it still learns CF replies. */
    int j=g.game.is_offset[g.infoset_map[J1]];
    close_to(g.game.last[j],1,1e-12);close_to(g.game.last[j+1],-1,1e-12);
    close_to(probability(&g,g.infoset_map[J1]),1,1e-12);
    gadget_solver_free(&solver);gadget_free(&g);game_free(&s);
    puts("controller pruning: unvisited P0 predictions preserved; zero-root-mass P1 reply updated");
}
static void objective_changes(void) {
    /* Expansion may revise leaf evaluations enough to change which gadget
     * should dominate. Keep the same minimizers across those revisions. */
    const double unsafe[2][2]={{.2,-.6},{-.8,.1}};Game s;Gadget g;matrix(&s,unsafe,&g);
    GadgetSolver solver;gadget_solver_init(&solver);int t=0;
    for(int phase=0;phase<3;++phase) {
        for(int j=0;j<2;++j) {
            int h=g.game.child[g.game.nodes[g.group_chance[j]].first];
            for(int a=0;a<2;++a) {
                int child=g.game.child[g.game.nodes[h].first+a];
                g.game.nodes[child].value=unsafe[j][a]+(phase==1?1:0);
            }
        }
        for(int k=0;k<12000;++k)assert(gadget_iteration(&g,++t,&solver));
        double p=probability(&g,g.infoset_map[0]);
        printf("adaptive revision phase=%d: action0 %.9f max_enter %.6f\n",phase,p,solver.max_enter);
        close_to(p,phase==1?7.0/17:.75,.004);close_to(solver.max_enter,phase==1?0:1,.002);
    }
    gadget_solver_free(&solver);gadget_free(&g);game_free(&s);
}
static void growth(void) {
    const double pay[2][2]={{1,.2},{-1,.2}};Game s;Gadget g;matrix(&s,pay,&g);
    GadgetSolver solver;gadget_solver_init(&solver);assert(gadget_iteration(&g,1,&solver));
    /* Append an 81-way P1 decision AFTER the auxiliary root infoset IDs.
     * This covers workspace growth and the traversal's heap action buffer. */
    int root=g.game.child[g.game.nodes[g.group_chance[0]].first];
    int leaf=g.game.child[g.game.nodes[root].first+1];
    int I=game_infoset(&g.game,1,81),kids[81];
    assert(I>g.entry_infoset[1]);
    for(int a=0;a<81;++a)kids[a]=terminal(&g.game,(a-40)/40.0);
    game_expand_leaf(&g.game,leaf,1,I,kids,81);
    assert(!gadget_iteration(&g,2,&solver)); /* Reject unfinalized new slots. */
    game_grow(&g.game);policy(&g.game,g.infoset_map[0],0);
    assert(gadget_iteration(&g,2,&solver));assert(solver.slots>=g.game.n_slots);
    int o=g.game.is_offset[I];close_to(g.game.last[o],1,1e-12);close_to(g.game.last[o+80],-1,1e-12);
    gadget_solver_free(&solver);gadget_free(&g);game_free(&s);
    puts("controller growth: appended infoset, 81 actions and workspace resizing passed");
}
int main(void){arithmetic();equilibria();objective_changes();pruning();growth();puts("test_gadget_controller: passed");return 0;}
