#include "../src/gadget.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void matrix(Game *g,const double pay[2][2],int roots[2]) {
    game_init(g);int I=game_infoset(g,0,2),root=game_node(g,NODE_CHANCE,0,-1,0);
    for(int j=0;j<2;++j) {
        roots[j]=game_node(g,NODE_PLAYER,0,I,0);int kids[2];
        for(int a=0;a<2;++a)kids[a]=game_node(g,NODE_TERMINAL,0,-1,pay[j][a]);
        game_children(g,roots[j],kids,NULL,2);
    }
    double p[2]={.99,.01};game_children(g,root,roots,p,2);game_finalize(g);
}
static double solve(Gadget *g,int mode,int iters,double *gap) {
    gadget_mode(g,mode);for(int t=1;t<=iters;++t)pcfr_iteration(&g->game,t);
    double *x=malloc((size_t)g->game.n_slots*sizeof(double));pcfr_current(&g->game,x);
    double p=x[g->game.is_offset[g->infoset_map[0]]];
    *gap=exploitability(&g->game,x);free(x);return p;
}
int main(void) {
    Game s;int r[2];const double trap[2][2]={{1,.2},{-1,.2}};matrix(&s,trap,r);
    for(int t=1;t<=1000;++t)pcfr_iteration(&s,t);
    double x[2];pcfr_current(&s,x);assert(x[0]>.999);
    assert(fabs(game_value(&s,x)-.98)<1e-9);
    GadgetRoot roots[2]={{r[0],0,.99},{r[1],1,.01}};
    GadgetGroup groups[2]={{.2,0,.99},{.2,0,.01}};
    for(int mode=0;mode<2;++mode) {
        Gadget g;assert(gadget_build(&s,roots,2,groups,2,&g));
        assert(fabs(g.alpha[0]-.745)<1e-12);assert(fabs(g.alpha[1]-.255)<1e-12);
        double gap,p=solve(&g,mode,2000,&gap);
        printf("low-prior trap mode=%d: risky %.8f gap %.8g\n",mode,p,gap);
        assert(p<1e-4);assert(gap<1e-4);gadget_free(&g);
    }
    /* Nonzero alternate values catch an incorrect +alt terminal shift. */
    game_free(&s);const double pay[2][2]={{3,1},{-1,2}};matrix(&s,pay,r);
    roots[0].node=r[0];roots[1].node=r[1];groups[0]=(GadgetGroup){2,0,.9};groups[1]=(GadgetGroup){.5,0,.1};
    Gadget g;assert(gadget_build(&s,roots,2,groups,2,&g));double gap,p=solve(&g,GADGET_MAXMARGIN,12000,&gap);
    printf("analytic mixed game: action0 %.8f (target .5), gap %.8g\n",p,gap);
    assert(fabs(p-.5)<.003);assert(gap<.01);
    assert(fabs(gadget_leaf_value(&g,0,3)-1)<1e-12);
    assert(isnan(gadget_leaf_value(&g,-1,3)));
    /* New turn carries behavior, never the old regret and prediction sums. */
    x[0]=.8;x[1]=.2;assert(gadget_seed_blueprint(&g,&s,x));
    double *all=malloc((size_t)g.game.n_slots*sizeof(double));pcfr_current(&g.game,all);
    assert(fabs(all[g.game.is_offset[g.infoset_map[0]]]-.8)<1e-12);
    for(int i=0;i<g.game.n_slots;++i){assert(g.game.regret[i]==0);assert(g.game.strategy_sum[i]==0);}
    int seed=g.game.is_offset[g.infoset_map[0]];
    assert(fabs(g.game.last[seed]-.8)<1e-12);assert(fabs(g.game.last[seed+1]-.2)<1e-12);
    GadgetSolver workspace;gadget_solver_init(&workspace);assert(gadget_iteration(&g,1,&workspace));
    for(int a=0;a<2;++a)assert(fabs(g.game.regret[seed+a]-fmax(0,g.game.last[seed+a]))<1e-12);
    gadget_solver_free(&workspace);
    free(all);gadget_free(&g);
    /* Positive gift LOWERS the effective P0 alternate, per Figure 9 line 8. */
    groups[0].gift=.4;assert(gadget_build(&s,roots,2,groups,2,&g));assert(fabs(g.offset[0]-1.6)<1e-12);gadget_free(&g);
    assert(fabs(gadget_gift_step(.1,.2,.5)-.4)<1e-12);assert(gadget_gift_step(.1,.5,.2)==.1);
    assert(gadget_new_alternate(.7,.2)==.2);assert(gadget_new_alternate(-.7,.2)==-.7);
    /* Conditional weights within ONE J; y(J) is counted once, not per node. */
    double value[7],cf[7],opp[7];assert(s.n_nodes==7);
    x[0]=.25;x[1]=.75;assert(gadget_blueprint_stats(&s,x,value,cf,opp));
    roots[0]=(GadgetRoot){r[0],0,cf[r[0]]};roots[1]=(GadgetRoot){r[1],0,cf[r[1]]};
    double alt,y;assert(gadget_group_blueprint(roots,2,0,value,opp,&alt,&y));
    assert(fabs(alt-(.99*1.5+.01*1.25))<1e-12);assert(y==1);
    GadgetGroup one={alt,0,y};assert(gadget_build(&s,roots,2,&one,1,&g));
    GameNode node=g.game.nodes[g.group_chance[0]];assert(fabs(g.game.prob[node.first]-.99)<1e-12);gadget_free(&g);
    /* Malformed partitions/weights fail explicitly. */
    roots[1].node=roots[0].node;assert(!gadget_build(&s,roots,2,&one,1,&g));gadget_free(&g);
    roots[1].node=r[1];roots[0].cf_reach=roots[1].cf_reach=0;assert(!gadget_build(&s,roots,2,&one,1,&g));gadget_free(&g);
    roots[0].cf_reach=NAN;assert(!gadget_build(&s,roots,2,&one,1,&g));gadget_free(&g);
    roots[0].cf_reach=1;roots[0].node=s.root;roots[1].cf_reach=1;assert(!gadget_build(&s,roots,2,&one,1,&g));gadget_free(&g);
    roots[0].node=r[0];groups[0].opponent_reach=groups[1].opponent_reach=0;roots[1].group=1;
    assert(gadget_build(&s,roots,2,groups,2,&g));assert(g.alpha[0]==.5&&g.alpha[1]==.5);gadget_free(&g);
    game_free(&s);
    /* Both margins cannot be nonnegative. Resolve must optimize the weighted
     * clipped objective, rather than silently behaving like Maxmargin. */
    const double unsafe[2][2]={{.2,-.6},{-.8,.1}};matrix(&s,unsafe,r);
    roots[0]=(GadgetRoot){r[0],0,.99};roots[1]=(GadgetRoot){r[1],1,.01};
    groups[0]=(GadgetGroup){0,0,.99};groups[1]=(GadgetGroup){0,0,.01};
    for(int mode=0;mode<2;++mode) {
        assert(gadget_build(&s,roots,2,groups,2,&g));p=solve(&g,mode,16000,&gap);
        double expected=mode==GADGET_RESOLVE?.75:7.0/17;
        printf("infeasible margins mode=%d: action0 %.8f (target %.8f), gap %.8g\n",mode,p,expected,gap);
        assert(fabs(p-expected)<.003);assert(gap<.01);gadget_free(&g);
    }
    game_free(&s);puts("test_gadget: passed");return 0;
}
