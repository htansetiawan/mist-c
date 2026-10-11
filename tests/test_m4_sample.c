#include "../src/m4sample.h"
#include "../src/m4search.h"
#include "../src/eval.h"
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fow_init();zobrist_init();
    Position p[2];pos_clear(&p[0]);p[0].side=WHITE;
    pos_put(&p[0],6,WHITE,KING);pos_put(&p[0],22,WHITE,PAWN);
    pos_put(&p[0],31,BLACK,BISHOP);pos_put(&p[0],60,BLACK,KING);
    pos_put(&p[0],40,BLACK,QUEEN);
    p[1]=p[0];pos_remove(&p[1],40);pos_put(&p[1],46,BLACK,QUEEN);
    char seen[2][65];observation(&p[0],WHITE,seen[0]);observation(&p[1],WHITE,seen[1]);assert(!strcmp(seen[0],seen[1]));
    MoveList m={.moves={MAKE_MOVE(22,31,NO_PIECE),MAKE_MOVE(6,7,NO_PIECE)},.count=2};
    float weights[]={0,-30};Belief b={.items=p,.logw=weights,.count=2,.us=WHITE};
    Position q=p[1];Undo u;make_move(&q,m.moves[0],&u);assert(mover_left_king_capturable(&q));
    q=p[0];make_move(&q,m.moves[0],&u);assert(!mover_left_king_capturable(&q));
    int indices[2]={-1,-1};M4WitnessStats st;unsigned char known[2]={0,0};
    int n=m4_witness_sample(&b,&m,known,2,0,indices,&st);
    assert(n==1&&indices[0]==1&&st.complete&&st.scanned==2&&st.represented_moves==1);
    known[0]=1;n=m4_witness_sample(&b,&m,known,2,0,indices,&st);assert(n==0&&st.complete);
    known[0]=0;n=m4_witness_sample(&b,&m,known,2,1e-9,indices,&st);assert(n==0&&!st.complete);
    n=m4_witness_sample(&b,&m,known,0,0,indices,&st);assert(n==0);
    M4Search *search=m4_create(WHITE);assert(search&&m4_observe(search,seen[0]));
    /* Fixed shallow game isolates root coverage/controller integration from
     * the separate, approximate growing-tree leaf evaluator. */
    M4Params params={2,20000,10000,0,2,1.0,21,1};SearchResult result;M4Stats stats;
    assert(m4_decide(search,&b,&params,&result,&stats));
    assert(stats.witness_added>0&&stats.witness_complete&&stats.snapshot);
    assert(result.samples==2); /* both distinct worlds reached the actual solve */
    int fatal=0;
    for(int i=0;i<2;++i) {
        MoveList legal;generate_moves(&p[i],&legal);int found=0;
        for(int a=0;a<legal.count;++a)found|=legal.moves[a]==result.move;
        assert(found);q=p[i];make_move(&q,result.move,&u);fatal+=mover_left_king_capturable(&q);
    }
    /* Coverage is the sampler's contract, not a zero-risk strategy guarantee.
     * In this cold fixture static alternate values are mutually infeasible;
     * Resolve mixes, and purification can choose the hazardous action. Keep
     * the negative policy diagnostic visible rather than calling it a save. */
    char chosen[6];move_to_uci(result.move,chosen);
    printf("M4 cold-alternate diagnostic: chosen=%s fatal_worlds=%d/2 (not a safety gate)\n",chosen,fatal);
    MoveList all;generate_moves(&p[1],&all);double best_child=-INFINITY;
    for(int a=0;a<all.count;++a){q=p[1];make_move(&q,all.moves[a],&u);
        double v=mover_left_king_capturable(&q)?-1:evaluate(&q,WHITE);
        if(v>best_child)best_child=v;
    }
    printf("M4 cold-alternate feasibility: rare-world alternate=%.9f best_child=%.9f\n",evaluate(&p[1],WHITE),best_child);
    assert(best_child<evaluate(&p[1],WHITE)); /* no mixture can meet this static alternate */
    m4_free(search);
    puts("test_m4_sample: rare hidden-slider witness, identical observations, existing coverage and deadline passed");
    return 0;
}
