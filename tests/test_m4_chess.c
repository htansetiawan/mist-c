#include "../src/m4search.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static M4SF *external;

static Move from_uci(const Position *p,const char *uci) {
    MoveList l;generate_moves(p,&l);
    for(int a=0;a<l.count;++a){char s[6];move_to_uci(l.moves[a],s);if(!strcmp(s,uci))return l.moves[a];}
    assert(0);return 0;
}
static void history_conformance(void) {
    Position p;pos_start(&p);M4Search *ctx[2]={m4_create(WHITE),m4_create(BLACK)};
    uint64_t h[2];char view[65];
    for(int c=0;c<2;++c){observation(&p,c,view);assert(m4_observe(ctx[c],view));h[c]=history_mix(0,view_hash(view));}
    assert(!m4_played(ctx[BLACK],0));assert(!m4_observe(ctx[WHITE],view));
    const char *moves[] = {"g1f3","g8f6","f3g1","f6g8"};
    uint64_t start=h[0];
    for(int k=0;k<4;++k) {
        Move move=from_uci(&p,moves[k]);int mover=p.side;assert(m4_played(ctx[mover],move));assert(!m4_played(ctx[mover],move));
        Undo u;make_move(&p,move,&u);
        for(int c=0;c<2;++c) {
            observation(&p,c,view);h[c]=history_mix(c==mover?history_mix(h[c],move):h[c],view_hash(view));
            assert(m4_observe(ctx[c],view));assert(m4_own_history(ctx[c])==h[c]);assert(m4_ply(ctx[c])==k+1);
        }
    }
    assert(start!=h[0]); /* same board is not the same information history */
    for(int c=0;c<2;++c)m4_free(ctx[c]);
    puts("M4 protocol history: both colours, every view, own moves, phase rejection and repeated board passed");
}
static int safe_opponent(Position *p) {
    MoveList l;generate_moves(p,&l);
    for(int a=0;a<l.count;++a){Position q=*p;Undo u;make_move(&q,l.moves[a],&u);if(!mover_left_king_capturable(&q)){*p=q;return 1;}}
    return 0;
}
static void chess_carry(int us) {
    Position p;pos_start(&p);Belief b;belief_init_start(&b,us,20000);
    M4Search *s=m4_create(us);assert(s);char view[65];observation(&p,us,view);assert(m4_observe(s,view));
    m4_set_evaluation(s,external,external!=NULL);
    M4Params params={12,30000,40,16,2,2.0,19,0};
    int carried=0,off=0;
    for(int decision=0;decision<4;++decision) {
        if(p.side!=us) {
            assert(safe_opponent(&p));observation(&p,us,view);assert(m4_observe(s,view));assert(belief_after_opponent(&b,view)>0);
        }
        M4Stats st;SearchResult r;params.seed=19+decision;
        assert(m4_decide(s,&b,&params,&r,&st));
        MoveList l;generate_moves(&p,&l);int found=0;for(int a=0;a<l.count;++a)found|=l.moves[a]==r.move;assert(found);
        assert(st.snapshot);assert(r.iterations>0);assert(r.nodes<=params.max_nodes);
        assert(!st.over_budget);assert(st.purified_support>=1&&st.purified_support<=3);
        if(external)assert(m4sf_ready(external)&&st.sf_calls>0&&!st.sf_failed);
        char uci[6];move_to_uci(r.move,uci);
        printf("M4 chess us=%d turn=%d move=%s carried=%d off-I=%d fresh=%d nodes=%d iters=%d %.4fs fallback=%d sf_calls=%ld\n",us,decision,uci,st.carried,st.off_current,st.fresh,r.nodes,r.iterations,st.total_seconds,st.fallback,st.sf_calls);
        carried+=st.carried;off+=st.off_current;
        assert(m4_played(s,r.move));Undo u;make_move(&p,r.move,&u);assert(!mover_left_king_capturable(&p));
        observation(&p,us,view);assert(m4_observe(s,view));assert(belief_after_own(&b,r.move,view)>0);
    }
    assert(carried>0);if(us==WHITE)assert(off>0);
    m4_free(s);belief_free(&b);
}
static void fifty_move_draw(void) {
    Position p;pos_clear(&p);p.side=WHITE;p.halfmove=99;
    pos_put(&p,6,WHITE,KING);pos_put(&p,60,BLACK,KING);pos_put(&p,57,BLACK,ROOK);
    Belief b={.items=&p,.count=1,.us=WHITE};M4Search *s=m4_create(WHITE);
    m4_set_evaluation(s,external,external!=NULL);
    char view[65];observation(&p,WHITE,view);assert(m4_observe(s,view));
    M4Params params={1,10000,100,16,2,1.0,21,0};SearchResult r;M4Stats st;
    assert(m4_decide(s,&b,&params,&r,&st));assert(st.snapshot&&r.value==0);
    Position q=p;Undo u;make_move(&q,r.move,&u);assert(q.halfmove==100&&!mover_left_king_capturable(&q));
    m4_free(s);puts("M4 50-move terminal: draw overrides heuristic evaluation passed");
}
int main(void){fow_init();zobrist_init();
    if(getenv("MIST_STOCKFISH")){external=m4sf_open(getenv("MIST_STOCKFISH"),1.0);assert(external);}
    history_conformance();chess_carry(WHITE);chess_carry(BLACK);fifty_move_draw();m4sf_close(external);
    puts("test_m4_chess: passed");return 0;}
