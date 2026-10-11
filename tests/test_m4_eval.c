#include "../src/m4search.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void safe_draw(M4SF *sf,int us){
    Position p;pos_clear(&p);p.side=us;p.halfmove=99;
    pos_put(&p,us==WHITE?6:62,us,KING);
    pos_put(&p,us==WHITE?60:4,us^1,KING);
    pos_put(&p,us==WHITE?57:1,us^1,ROOK);
    Belief b={.items=&p,.count=1,.us=us};M4Search *s=m4_create(us);
    assert(m4_set_leaf_scale(s,.9));
    m4_set_evaluation(s,sf,1);
    char view[65];observation(&p,us,view);assert(m4_observe(s,view));
    /* For Black, give the history context its preceding White observation.
     * Two identical views only establish parity; no opponent move is inferred. */
    if(us==BLACK)assert(m4_observe(s,view));
    M4Params params={1,10000,100,0,2,.5,21,0};SearchResult r;M4Stats st;
    assert(m4_decide(s,&b,&params,&r,&st));
    assert(st.snapshot&&r.value==0&&st.child_alternates==1);
    if(sf)assert(st.sf_calls==1&&st.sf_scored_moves>0&&!st.sf_failed);
    Undo u;make_move(&p,r.move,&u);assert(p.halfmove==100&&!mover_left_king_capturable(&p));
    m4_free(s);
}
static void rare_diagnostic(M4SF *sf){
    Position p[2];pos_clear(p);p[0].side=WHITE;
    pos_put(p,6,WHITE,KING);pos_put(p,22,WHITE,PAWN);pos_put(p,31,BLACK,BISHOP);pos_put(p,60,BLACK,KING);pos_put(p,40,BLACK,QUEEN);
    p[1]=p[0];pos_remove(&p[1],40);pos_put(&p[1],46,BLACK,QUEEN);
    float weights[]={0,-30};Belief b={.items=p,.logw=weights,.count=2,.us=WHITE};
    M4Search *s=m4_create(WHITE);m4_set_evaluation(s,sf,1);char view[65];observation(p,WHITE,view);assert(m4_observe(s,view));
    M4Params params={2,20000,10000,0,2,1.0,21,1};SearchResult r;M4Stats st;
    assert(m4_decide(s,&b,&params,&r,&st));assert(st.snapshot&&r.samples==2&&st.child_alternates==2);
    int fatal=0;for(int i=0;i<2;++i){Position q=p[i];Undo u;make_move(&q,r.move,&u);fatal+=mover_left_king_capturable(&q);}
    char move[6];move_to_uci(r.move,move);printf("child-backed alternate %s: move=%s fatal=%d/2 scored=%d calls=%ld\n",sf?"Stockfish":"internal",move,fatal,st.sf_scored_moves,st.sf_calls);
    m4_free(s);
}
int main(void){
    fow_init();zobrist_init();safe_draw(NULL,WHITE);safe_draw(NULL,BLACK);rare_diagnostic(NULL);
    const char *path=getenv("MIST_STOCKFISH");
    if(path){M4SF *sf=m4sf_open(path,1);assert(sf);safe_draw(sf,WHITE);safe_draw(sf,BLACK);rare_diagnostic(sf);m4sf_close(sf);}
    puts("test_m4_eval: fresh alternatives and two-color rules-first terminal override passed; rare-world policy is diagnostic only");
}
