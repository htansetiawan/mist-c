#include "../src/m4search.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int sq(int x,int us){return us==WHITE?x:x^56;}
static void terminal(int us,int winning) {
    Position p;pos_clear(&p);p.side=us;
    pos_put(&p,sq(0,us),us,KING);
    if(winning){pos_put(&p,sq(55,us),us,ROOK);pos_put(&p,sq(63,us),us^1,KING);}
    else {pos_put(&p,sq(10,us),us^1,KING);pos_put(&p,sq(17,us),us^1,QUEEN);}
    Belief b={.items=&p,.count=1,.us=us};M4Search *s=m4_create(us);assert(s);
    assert(!m4_set_leaf_scale(s,0));assert(!m4_set_leaf_scale(s,-1));
    assert(!m4_set_leaf_scale(s,NAN));assert(!m4_set_leaf_scale(s,1.1));
    assert(m4_set_leaf_scale(s,.9));char view[65];observation(&p,us,view);
    assert(m4_observe(s,view));if(us==BLACK)assert(m4_observe(s,view));
    M4Params params={1,10000,100,0,2,1.0,23,0};SearchResult r;M4Stats st;
    assert(m4_decide(s,&b,&params,&r,&st));assert(st.snapshot);
    assert(fabs(r.value-(winning?1:-1))<1e-9);
    assert(m4_set_leaf_scale(s,.9));assert(!m4_set_leaf_scale(s,1));
    Undo u;make_move(&p,r.move,&u);
    if(winning)assert(king_square(&p,us^1)<0);else assert(mover_left_king_capturable(&p));
    m4_free(s);
}
int main(void){fow_init();zobrist_init();for(int us=0;us<2;++us){terminal(us,1);terminal(us,0);}
    puts("test_m4_scale: both-color proven wins/losses retain +/-1, invalid scales and midgame changes rejected");return 0;}
