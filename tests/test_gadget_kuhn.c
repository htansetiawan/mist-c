/* Complete-frontier resolve on Kuhn: measured full-game opponent BR cannot
 * deteriorate relative to the converged blueprint. This is NOT a blanket
 * safety assertion for truncated 2-KLUSS, which lacks that guarantee. */
#include "../src/gadget.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int term(Game *g,double v){return game_node(g,NODE_TERMINAL,0,-1,v);}
int main(void) {
    Game s;game_init(&s);int first[3],cb[3],after_c[3],after_b[3];
    for(int c=0;c<3;++c){first[c]=game_infoset(&s,0,2);cb[c]=game_infoset(&s,0,2);after_c[c]=game_infoset(&s,1,2);after_b[c]=game_infoset(&s,1,2);}
    int root=game_node(&s,NODE_CHANCE,0,-1,0),deals[6],group[6],d=0;double p[6];
    for(int a=0;a<3;++a)for(int b=0;b<3;++b)if(a!=b) {
        double win=a>b?1:-1;
        int n0=game_node(&s,NODE_PLAYER,0,first[a],0),n1c=game_node(&s,NODE_PLAYER,1,after_c[b],0);
        int n0cb=game_node(&s,NODE_PLAYER,0,cb[a],0),kids_cb[2]={term(&s,-1),term(&s,2*win)};
        game_children(&s,n0cb,kids_cb,NULL,2);int kids_c[2]={term(&s,win),n0cb};game_children(&s,n1c,kids_c,NULL,2);
        int n1b=game_node(&s,NODE_PLAYER,1,after_b[b],0),kids_b[2]={term(&s,1),term(&s,2*win)};
        game_children(&s,n1b,kids_b,NULL,2);int kids0[2]={n1c,n1b};game_children(&s,n0,kids0,NULL,2);
        deals[d]=n0;group[d]=b;p[d++]=1.0/6;
    }
    game_children(&s,root,deals,p,6);game_finalize(&s);
    for(int t=1;t<=4000;++t)pcfr_iteration(&s,t);
    double *x=malloc((size_t)s.n_slots*sizeof(double));pcfr_current(&s,x);
    double before=best_response_value(&s,x,1);assert(fabs(game_value(&s,x)+1.0/18)<.002);
    double *v=malloc((size_t)s.n_nodes*sizeof(double)),*cf=malloc((size_t)s.n_nodes*sizeof(double)),*opp=malloc((size_t)s.n_nodes*sizeof(double));
    assert(gadget_blueprint_stats(&s,x,v,cf,opp));GadgetRoot r[6];GadgetGroup groups[3];
    for(int i=0;i<6;++i)r[i]=(GadgetRoot){deals[i],group[i],cf[deals[i]]};
    for(int j=0;j<3;++j){groups[j].gift=0;assert(gadget_group_blueprint(r,6,j,v,opp,&groups[j].alternate,&groups[j].opponent_reach));}
    /* Contradictory root labels cannot give the opponent perfect recall. */
    Gadget bad;int saved_group=r[0].group;r[0].group=(saved_group+1)%3;
    assert(!gadget_build(&s,r,6,groups,3,&bad));gadget_free(&bad);r[0].group=saved_group;
    for(int mode=0;mode<2;++mode) {
        Gadget g;assert(gadget_build(&s,r,6,groups,3,&g));assert(gadget_seed_blueprint(&g,&s,x));gadget_mode(&g,mode);
        for(int t=1;t<=4000;++t)pcfr_iteration(&g.game,t);
        double *gx=malloc((size_t)g.game.n_slots*sizeof(double)),*combined=malloc((size_t)s.n_slots*sizeof(double));
        pcfr_current(&g.game,gx);memcpy(combined,x,(size_t)s.n_slots*sizeof(double));
        for(int i=0;i<s.n_infosets;++i)if(s.is_player[i]==0&&g.infoset_map[i]>=0)
            memcpy(combined+s.is_offset[i],gx+g.game.is_offset[g.infoset_map[i]],(size_t)s.is_actions[i]*sizeof(double));
        double after=best_response_value(&s,combined,1),gap=exploitability(&g.game,gx);
        printf("Kuhn mode=%d: opponent BR %.8f -> %.8f, gadget gap %.8g\n",mode,before,after,gap);
        assert(after<=before+.005);assert(gap<.01);
        free(gx);free(combined);gadget_free(&g);
    }
    /* A zero-probability opponent action leaves its counterfactual reach
     * intact, even though its opponent-own reach is zero. */
    int chosen=after_c[1],offset=s.is_offset[chosen];x[offset]=1;x[offset+1]=0;
    assert(gadget_blueprint_stats(&s,x,v,cf,opp));int found=0;
    for(int n=0;n<s.n_nodes;++n)if(s.nodes[n].type==NODE_PLAYER&&s.nodes[n].infoset==chosen) {
        int child=s.child[s.nodes[n].first+1];assert(opp[child]==0);assert(fabs(cf[child]-cf[n])<1e-12);++found;
    }
    assert(found==2);
    free(x);free(v);free(cf);free(opp);game_free(&s);puts("test_gadget_kuhn: passed");return 0;
}
