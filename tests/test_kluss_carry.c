/* Exercise two consecutive adaptive solves. An ordinary Game snapshot from
 * resolve_root or maxmargin_root gives the wrong opponent reaches, synthetic
 * gifts, and centered values; this fixture distinguishes all three errors. */
#include "../src/kluss.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void near(double a,double b){assert(isfinite(a)&&fabs(a-b)<1e-10);}
static int terminal(Game *g,double v){return game_node(g,NODE_TERMINAL,0,-1,v);}
static int ours(Game *g,int I,double a,double b) {
    int h=game_node(g,NODE_PLAYER,0,I,0),kids[2]={terminal(g,a),terminal(g,b)};
    game_children(g,h,kids,NULL,2);return h;
}
int main(void) {
    Game source;game_init(&source);
    int I=game_infoset(&source,0,2),J[2]={game_infoset(&source,1,2),game_infoset(&source,1,2)};
    int initial=game_node(&source,NODE_CHANCE,0,-1,0),world[2],cut[2];
    for(int j=0;j<2;++j) {
        world[j]=game_node(&source,NODE_PLAYER,1,J[j],0);
        cut[j]=ours(&source,I,j?5:2,j?7:4);
        int kids[2]={cut[j],ours(&source,I,j?0:-2,j?2:0)};
        game_children(&source,world[j],kids,NULL,2);
    }
    double prior[2]={.99,.01};game_children(&source,initial,world,prior,2);game_finalize(&source);
    GadgetRoot roots[2]={{world[0],0,.99},{world[1],1,.01}};
    GadgetGroup groups[2]={{1.5,.2,.99},{-.7,0,.01}};
    Gadget g;assert(gadget_build(&source,roots,2,groups,2,&g));
    /* Expand a copied leaf AFTER gadget construction. Raw-score restoration
     * must include descendants absent from g.node_map. */
    int retained_cut=g.node_map[cut[1]],old_leaf=g.game.child[g.game.nodes[retained_cut].first+1];
    int added_I=game_infoset(&g.game,1,2);
    int added[2]={terminal(&g.game,gadget_leaf_value(&g,1,8)),terminal(&g.game,gadget_leaf_value(&g,1,6))};
    game_expand_leaf(&g.game,old_leaf,1,added_I,added,2);game_grow(&g.game);
    double *x=malloc((size_t)g.game.n_slots*sizeof(double));assert(x);pcfr_current(&g.game,x);
    int q=g.game.is_offset[g.selector];x[q]=.7;x[q+1]=.3;
    for(int j=0;j<2;++j) {
        int e=g.game.is_offset[g.entry_infoset[j]],o=g.game.is_offset[g.infoset_map[J[j]]];
        x[e+1]=j?.8:.1;x[e]=1-x[e+1];x[o]=j?.6:.25;x[o+1]=1-x[o];
    }
    uint64_t *oh=calloc((size_t)g.game.n_nodes,sizeof(uint64_t)),*th=calloc((size_t)g.game.n_nodes,sizeof(uint64_t));
    assert(oh&&th);
    KlussFrontier f[2];
    for(int j=0;j<2;++j){int h=g.node_map[cut[j]];oh[h]=11;th[h]=100+j;f[j]=(KlussFrontier){h,11,100+j};}
    KlussBlueprint b;kluss_blueprint_init(&b);assert(kluss_blueprint_capture_gadget(&b,&g,x,oh,th));
    near(b.active_mass,.4228);near(b.expected_value,.2232*4/.4228);
    assert(b.boundary_count==2);
    near(b.boundary[0].cf_reach,1);near(b.boundary[0].opponent_reach,.1996);
    near(b.boundary[1].opponent_reach,.2232);
    near(b.tree.nodes[added[0]].value,8);near(b.tree.nodes[added[1]].value,6);
    near(g.game.nodes[added[0]].value,8.7); /* source unchanged */
    double *v=malloc((size_t)b.tree.n_nodes*sizeof(double)),*cf=malloc((size_t)b.tree.n_nodes*sizeof(double)),*opp=malloc((size_t)b.tree.n_nodes*sizeof(double));
    assert(v&&cf&&opp&&kluss_blueprint_stats(&b,v,cf,opp));
    near(v[f[0].node],3);near(v[f[1].node],6);near(cf[f[0].node],1);
    near(opp[f[0].node],.1996*.25);near(opp[f[1].node],.2232*.6);
    near(cf[g.maxmargin_root],0);near(cf[g.resolve_root],0);
    KlussPrepared prepared;kluss_prepared_init(&prepared);assert(kluss_prepare(&b,f,2,11,&prepared));
    near(prepared.groups[0].alternate,3);near(prepared.groups[1].alternate,6);
    near(prepared.groups[0].gift,3);near(prepared.groups[1].gift,2);
    near(prepared.gadget.offset[0],0);near(prepared.gadget.offset[1],4);
    double y0=.1996*.25,y1=.2232*.6;
    near(prepared.gadget.alpha[0],.5*(y0/(y0+y1)+.5));
    /* Invalid probabilities leave the already valid owned snapshot intact. */
    GameNode *saved=b.tree.nodes;x[q]=NAN;
    assert(!kluss_blueprint_capture_gadget(&b,&g,x,oh,th));assert(b.tree.nodes==saved);x[q]=.7;
    /* Capture the next gadget: restoring its NEW offset must not reapply the
     * old offset. Neither synthetic selector actions nor old gifts carry. */
    Gadget *next=&prepared.gadget;
    double *nx=malloc((size_t)next->game.n_slots*sizeof(double));assert(nx);pcfr_current(&next->game,nx);
    uint64_t *noh=calloc((size_t)next->game.n_nodes,sizeof(uint64_t)),*nth=calloc((size_t)next->game.n_nodes,sizeof(uint64_t));assert(noh&&nth);
    KlussFrontier nf[2];
    for(int j=0;j<2;++j){int h=next->node_map[f[j].node];noh[h]=11;nth[h]=100+j;nf[j]=(KlussFrontier){h,11,100+j};}
    KlussBlueprint nb;kluss_blueprint_init(&nb);assert(kluss_blueprint_capture_gadget(&nb,next,nx,noh,nth));
    near(nb.tree.nodes[next->node_map[added[0]]].value,8);
    KlussPrepared np;kluss_prepared_init(&np);assert(kluss_prepare(&nb,nf,2,11,&np));
    near(np.groups[0].alternate,3);near(np.groups[1].alternate,6);
    near(np.groups[0].gift,0);near(np.groups[1].gift,0);
    /* A zero-mass opponent group still carries its CF support into a later
     * solve. The root weighting must not delete such a subtree. */
    for(int j=0;j<2;++j){int e=g.game.is_offset[g.entry_infoset[j]];x[e]=1;x[e+1]=0;}
    x[q]=1;x[q+1]=0;assert(kluss_blueprint_capture_gadget(&b,&g,x,oh,th));
    assert(kluss_blueprint_stats(&b,v,cf,opp));near(opp[f[1].node],0);near(cf[f[1].node],1);
    assert(kluss_prepare(&b,f,2,11,&prepared));near(prepared.groups[1].opponent_reach,0);
    near(prepared.gadget.alpha[1],.25);
    GadgetBoundary overlap[2]={{g.group_chance[0],1,1},{f[0].node,1,1}};
    assert(!gadget_forest_stats(&g.game,x,overlap,2,v,cf,opp));
    free(v);free(cf);free(opp);free(x);free(oh);free(th);free(nx);free(noh);free(nth);
    kluss_prepared_free(&np);kluss_prepared_free(&prepared);kluss_blueprint_free(&nb);kluss_blueprint_free(&b);gadget_free(&g);game_free(&source);
    puts("adaptive carry-over: actual nonunit root reaches, raw expanded leaves, real-action gifts and two successive solves passed");
    return 0;
}
