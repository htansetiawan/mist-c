#include "../src/m4movefeatures.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static void replay(Position *p,const char *text) {
    MoveList l;generate_moves(p,&l);
    for(int a=0;a<l.count;++a){char uci[6];move_to_uci(l.moves[a],uci);if(!strcmp(text,uci)){Undo u;make_move(p,l.moves[a],&u);return;}}
    assert(0);
}
static void compare(const Position *p) {
    Position saved=*p;MoveList l;generate_moves(p,&l);float x[256][M4MM_FEATURES],old[MM_FEATURES];
    m4_move_features(p,&l,0,x);assert(!memcmp(p,&saved,sizeof(saved)));
    for(int a=0;a<l.count;++a) {
        move_features(p,l.moves[a],0,1,old);assert(!memcmp(x[a],old,sizeof(old)));
        Position q=*p;Undo u;make_move(&q,l.moves[a],&u);char before[65],after[65];
        observation(p,p->side^1,before);observation(&q,p->side^1,after);
        assert(x[a][M4MM_SILENT_MOVE]==!strcmp(before,after));
        int king=king_square(&q,p->side^1),all_hidden=1,checks=0;
        if(king>=0)for(int sq=0;sq<64;++sq)if(q.color[sq]==p->side&&(attacks_from(&q,sq)&((bb_t)1<<king))) {
            ++checks;if(after[sq]!='?')all_hidden=0;
        }
        assert(x[a][M4MM_HIDDEN_CHECK]==(checks>0&&all_hidden));
        assert(x[a][M4MM_SILENT_CHECK]==(checks>0&&!strcmp(before,after)));
        assert(x[a][M4MM_CONCEALED_MOVER]==(after[MOVE_TO(l.moves[a])]=='?'));
        for(int k=0;k<M4MM_FEATURES;++k)assert(isfinite(x[a][k]));
    }
}
int main(void) {
    fow_init();Position p;pos_start(&p);compare(&p);
    const char *moves[]={"d2d4","d7d5","c2c3","g8f6","g1f3","b8c6","e2e3","a7a6","f1d3","e7e5","d4e5","f6d7","e1g1","c6e5","f3e5","d7e5"};
    for(unsigned i=0;i<sizeof(moves)/sizeof(*moves);++i){replay(&p,moves[i]);compare(&p);}
    MoveList l;generate_moves(&p,&l);float x[256][M4MM_FEATURES];m4_move_features(&p,&l,0,x);int found=0;
    for(int a=0;a<l.count;++a){char uci[6];move_to_uci(l.moves[a],uci);if(!strcmp(uci,"d1a4")){
        assert(x[a][M4MM_HIDDEN_CHECK]==1&&x[a][M4MM_SILENT_CHECK]==1&&x[a][M4MM_SILENT_MOVE]==1);found=1;}}
    assert(found);
    /* King capture leaves no victim king: do not invent a post-capture check. */
    pos_clear(&p);p.side=WHITE;pos_put(&p,0,WHITE,KING);pos_put(&p,4,WHITE,ROOK);pos_put(&p,60,BLACK,KING);compare(&p);
    puts("test_m4_movefeatures: baseline bytes, both-color visibility, true silent-Qa4 motif and king capture passed");
}
