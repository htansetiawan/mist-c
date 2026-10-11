#include "../src/kluss.h"
#include "../src/gadget.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    KlussFrontier f[5]={{0,10,20},{1,10,21},{2,11,21},{3,11,22},{4,12,22}};
    unsigned char keep[5];int d[5];
    assert(kluss_select(f,5,10,keep,d)==3);
    assert(d[0]==0&&d[1]==0&&d[2]==1&&d[3]==-1&&d[4]==-1);
    assert(kluss_select(f,5,99,keep,d)==-1);
    /* Exercise sorting rather than assuming monotone hashes or dense IDs. */
    f[0].theirs=UINT64_MAX;f[4].theirs=UINT64_MAX;
    assert(kluss_select(f,5,10,keep,d)==4&&d[4]==1);
    /* Source tree has an off-current-I node B that must stay a PLAYER node. */
    Game src;game_init(&src);int root=game_node(&src,NODE_CHANCE,0,-1,0),kids[3];
    for(int i=0;i<3;++i) {
        int I=game_infoset(&src,0,2);kids[i]=game_node(&src,NODE_PLAYER,0,I,0);
        int leaves[2]={game_node(&src,NODE_TERMINAL,0,-1,0),game_node(&src,NODE_TERMINAL,0,-1,1)};
        game_children(&src,kids[i],leaves,NULL,2);
    }
    double probs[3]={.4,.3,.3};game_children(&src,root,kids,probs,3);game_finalize(&src);
    uint64_t *us=calloc((size_t)src.n_nodes,sizeof(uint64_t)),*them=calloc((size_t)src.n_nodes,sizeof(uint64_t));
    us[kids[0]]=10;us[kids[1]]=11;us[kids[2]]=12;them[kids[0]]=them[kids[1]]=20;them[kids[2]]=22;
    KlussFrontier frontier[3];for(int i=0;i<3;++i)frontier[i]=(KlussFrontier){kids[i],us[kids[i]],them[kids[i]]};
    assert(kluss_select(frontier,3,10,keep,d)==2&&d[1]==1);
    double x[6]={.9,.1,.9,.1,.9,.1};
    for(int i=0;i<src.n_slots;++i){src.regret[i]=1000+i;src.last[i]=42;src.strategy_sum[i]=999;}
    KlussBlueprint bp;kluss_blueprint_init(&bp);assert(kluss_blueprint_capture(&bp,&src,x,us,them));
    assert(bp.valid&&fabs(bp.expected_value-.1)<1e-12);
    assert(bp.ours[kids[1]]==11&&bp.theirs[kids[1]]==20);
    for(int i=0;i<bp.tree.n_slots;++i){assert(bp.tree.regret[i]==0&&bp.tree.last[i]==0&&bp.tree.strategy_sum[i]==0);}
    x[0]=1;x[1]=0;assert(bp.strategy[0]==.9); /* deep copy */
    x[0]=-1;assert(!kluss_blueprint_capture(&bp,&src,x,us,them));assert(bp.valid&&bp.strategy[0]==.9);
    GadgetRoot r[2]={{kids[0],0,.4},{kids[1],0,.3}};GadgetGroup group={.1,0,1};Gadget g;
    assert(gadget_build(&bp.tree,r,2,&group,1,&g));
    assert(g.node_map[kids[2]]==-1);int copied=g.node_map[kids[1]];
    assert(g.game.nodes[copied].type==NODE_PLAYER&&g.game.nodes[copied].player==0);
    assert(gadget_seed_blueprint(&g,&bp.tree,bp.strategy));
    gadget_mode(&g,GADGET_MAXMARGIN);for(int t=1;t<=100;++t)pcfr_iteration(&g.game,t);
    double *strategy=malloc((size_t)g.game.n_slots*sizeof(double));pcfr_current(&g.game,strategy);
    int off=g.game.is_offset[g.infoset_map[1]];assert(strategy[off+1]>.999); /* B was optimized, not frozen */
    assert(exploitability(&g.game,strategy)<1e-8);
    free(strategy);gadget_free(&g);kluss_blueprint_free(&bp);game_free(&src);free(us);free(them);
    puts("test_kluss: passed (one-hop closure, unfrozen B, isolated blueprint and fresh regrets)");return 0;
}
