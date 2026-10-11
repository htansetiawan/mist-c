#include "m4movefeatures.h"
#include <string.h>

static int same_view(const Position *p,const Position *q,bb_t before,bb_t after) {
    if(before!=after)return 0;
    for(bb_t b=before;b;b&=b-1) {
        int square=__builtin_ctzll(b);
        if(p->piece[square]!=q->piece[square]||p->color[square]!=q->color[square])return 0;
    }
    return 1;
}
void m4_move_features(const Position *p,const MoveList *l,Move previous,float out[256][M4MM_FEATURES]) {
    int us=p->side,them=us^1;
    bb_t own_before=visible_squares(p,us),their_before=visible_squares(p,them);
    for(int a=0;a<l->count;++a) {
        memset(out[a],0,sizeof(out[a]));move_features(p,l->moves[a],previous,1,out[a]);
        Position q=*p;Undo undo;make_move(&q,l->moves[a],&undo);
        bb_t own_after=visible_squares(&q,us),their_after=visible_squares(&q,them);
        int king=king_square(&q,them),to=MOVE_TO(l->moves[a]);
        bb_t checkers=0;
        if(king>=0)for(bb_t pieces=q.by_color[us];pieces;pieces&=pieces-1) {
            int square=__builtin_ctzll(pieces);
            if(attacks_from(&q,square)&((bb_t)1<<king))checkers|=(bb_t)1<<square;
        }
        int silent=same_view(p,&q,their_before,their_after);
        int hidden=!(their_after&((bb_t)1<<to));
        out[a][M4MM_HIDDEN_CHECK]=checkers&&!(checkers&their_after);
        out[a][M4MM_SILENT_CHECK]=checkers&&silent;
        out[a][M4MM_SILENT_MOVE]=silent;
        out[a][M4MM_CONCEALED_MOVER]=hidden;
        out[a][M4MM_NEW_VISION]=(float)__builtin_popcountll(own_after&~own_before)/64.0f;
        out[a][M4MM_CONCEALED_KING_ZONE]=hidden*out[a][MM_KING_ZONE];
    }
}
