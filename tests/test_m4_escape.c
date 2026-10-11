#include "../src/m4escape.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Move move(const char *uci) {
    return MAKE_MOVE((uci[0]-'a')+8*(uci[1]-'1'),(uci[2]-'a')+8*(uci[3]-'1'),NO_PIECE);
}
static void play(Position *p,const char *uci) {
    Undo u;make_move(p,move(uci),&u);
}
static Position mirror(const Position *p) {
    Position q;pos_clear(&q);q.side=p->side^1;q.halfmove=p->halfmove;
    q.ep=p->ep<0?-1:p->ep^56;
    for(int s=0;s<64;++s) {
        if(p->piece[s]!=NO_PIECE)pos_put(&q,s^56,p->color[s]^1,p->piece[s]);
        if(p->castle_rooks&(UINT64_C(1)<<s))q.castle_rooks|=UINT64_C(1)<<(s^56);
    }
    return q;
}
int main(void) {
    fow_init();Position root;pos_start(&root);
    play(&root,"e2e4");play(&root,"d7d5");play(&root,"e4d5");play(&root,"d8d5");play(&root,"d2d4");
    Position diagonal=root,file=root;play(&diagonal,"d5a5");play(&file,"d5e4");
    char av[65],bv[65];observation(&diagonal,WHITE,av);observation(&file,WHITE,bv);assert(!strcmp(av,bv));
    Position before=diagonal;
    M4Escape *a=m4_escape_create(4);assert(a);
    assert(m4_escape_add(a,&diagonal,7)==1);
    assert(!memcmp(&before,&diagonal,sizeof(Position))); /* never mutate caller */
    const M4EscapeGroup *g=m4_escape_group(a,0);
    assert(g&&g->worlds==1&&g->common.count==7&&!g->individually_dead);
    assert(m4_escape_add(a,&file,7)==1);
    g=m4_escape_group(a,0);
    assert(g->worlds==2&&g->common.count==0&&!g->individually_dead);
    assert(m4_escape_complete(a)&&m4_escape_groups(a)==1);
    /* Same view with a different remembered history must not be fused. */
    assert(m4_escape_add(a,&file,8)==1&&m4_escape_groups(a)==2);
    assert(m4_escape_group(a,1)->common.count==5);
    assert(!m4_escape_group(a,-1)&&!m4_escape_group(a,2));
    Position fewer_rights=file;fewer_rights.castle_rooks&=~(UINT64_C(1)<<7);
    observation(&fewer_rights,WHITE,bv);assert(!strcmp(av,bv));
    assert(m4_escape_add(a,&fewer_rights,7)==1&&m4_escape_groups(a)==3);
    m4_escape_free(a);
    /* Intersection is order invariant and duplicate states cannot restore it. */
    a=m4_escape_create(1);
    assert(m4_escape_add(a,&file,7)==1&&m4_escape_add(a,&diagonal,7)==1);
    assert(m4_escape_add(a,&file,7)==1&&m4_escape_groups(a)==1);
    assert(!m4_escape_group(a,0)->common.count&&m4_escape_group(a,0)->worlds==3);
    assert(m4_escape_add(a,&file,8)==0&&!m4_escape_complete(a));
    assert(m4_escape_group(a,0)->worlds==3);m4_escape_free(a);
    Position black_diag=mirror(&diagonal),black_file=mirror(&file);
    a=m4_escape_create(1);assert(m4_escape_add(a,&black_diag,7)==1);
    assert(m4_escape_group(a,0)->common.count==7);
    assert(m4_escape_add(a,&black_file,7)==1);
    assert(!m4_escape_group(a,0)->common.count&&!m4_escape_group(a,0)->individually_dead);
    m4_escape_free(a);
    a=m4_escape_create(1);Position bad=file;bad.halfmove=100;
    assert(m4_escape_add(a,&bad,7)==-1&&!m4_escape_complete(a));
    assert(!m4_escape_groups(a));m4_escape_free(a);
    /* A physical forced loss must remain distinct from an information conflict. */
    Position doomed;pos_clear(&doomed);doomed.side=WHITE;
    pos_put(&doomed,0,WHITE,KING);pos_put(&doomed,17,BLACK,QUEEN);pos_put(&doomed,63,BLACK,KING);
    a=m4_escape_create(1);assert(m4_escape_add(a,&doomed,9)==1);
    assert(!m4_escape_group(a,0)->common.count&&m4_escape_group(a,0)->individually_dead==1);
    m4_escape_free(a);
    assert(!m4_escape_create(0)&&!m4_escape_create(65537));
    puts("test_m4_escape: hidden diagonal/file conflict, perfect recall, immutable input, order invariance and bounds passed");
    return 0;
}
