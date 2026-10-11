#include "m4escape.h"
#include <stdlib.h>
#include <string.h>

struct M4Escape {
    int limit, count, slots, complete;
    int *table;                    /* group index + 1; zero means vacant */
    M4EscapeGroup *group;
};

static uint64_t hash_byte(uint64_t h, unsigned char x) {
    return (h ^ x) * UINT64_C(1099511628211);
}
static uint64_t key(uint64_t history, bb_t rights, int side, const char view[65]) {
    uint64_t h=UINT64_C(14695981039346656037);
    for(int i=0;i<8;++i){h=hash_byte(h,(unsigned char)(history>>(8*i)));h=hash_byte(h,(unsigned char)(rights>>(8*i)));}
    h=hash_byte(h,(unsigned char)side);
    for(int i=0;i<64;++i)h=hash_byte(h,(unsigned char)view[i]);
    return h;
}
static int move_order(const void *a,const void *b) {
    Move x=*(const Move *)a,y=*(const Move *)b;
    return (x>y)-(x<y);
}
M4Escape *m4_escape_create(int max_groups) {
    if(max_groups<1||max_groups>65536)return NULL;
    M4Escape *a=calloc(1,sizeof(*a));if(!a)return NULL;
    a->limit=max_groups;a->slots=2;a->complete=1;
    while(a->slots<2*max_groups)a->slots*=2;
    a->table=calloc((size_t)a->slots,sizeof(*a->table));
    a->group=calloc((size_t)max_groups,sizeof(*a->group));
    if(!a->table||!a->group){m4_escape_free(a);return NULL;}
    return a;
}
void m4_escape_free(M4Escape *a){if(a){free(a->table);free(a->group);free(a);}}
int m4_escape_complete(const M4Escape *a){return a&&a->complete;}
int m4_escape_groups(const M4Escape *a){return a?a->count:0;}
const M4EscapeGroup *m4_escape_group(const M4Escape *a,int i){return a&&i>=0&&i<a->count?&a->group[i]:NULL;}

int m4_escape_add(M4Escape *a,const Position *p,uint64_t history) {
    if(!a)return -1;
    if(!p||p->side<WHITE||p->side>BLACK||king_square(p,WHITE)<0||king_square(p,BLACK)<0
       ||p->halfmove<0||p->halfmove>=100||p->ep < -1||p->ep > 63||mover_left_king_capturable(p)) {a->complete=0;return -1;}
    MoveList legal;generate_moves(p,&legal);
    if(!legal.count){a->complete=0;return -1;}
    char view[65];observation(p,p->side,view);
    bb_t rights=p->castle_rooks&p->by_color[p->side];
    int slot=(int)(key(history,rights,p->side,view)&(uint64_t)(a->slots-1));
    while(a->table[slot]) {
        M4EscapeGroup *g=&a->group[a->table[slot]-1];
        if(g->history==history&&g->own_castle_rooks==rights&&g->side==p->side&&!memcmp(g->view,view,64))break;
        slot=(slot+1)&(a->slots-1);
    }
    if(!a->table[slot]&&a->count==a->limit){a->complete=0;return 0;}
    MoveList safe={.count=0};Position child=*p;
    for(int k=0;k<legal.count;++k) {
        Undo u;make_move(&child,legal.moves[k],&u);
        /* Capturing the opposing king wins before checking our exposure. */
        if(king_square(&child,p->side^1)<0||!mover_left_king_capturable(&child))safe.moves[safe.count++]=legal.moves[k];
        unmake_move(&child,legal.moves[k],&u);
    }
    qsort(safe.moves,(size_t)safe.count,sizeof(Move),move_order);
    if(!a->table[slot]) {
        int index=a->count++;a->table[slot]=index+1;
        M4EscapeGroup *g=&a->group[index];
        g->history=history;g->own_castle_rooks=rights;g->side=p->side;
        memcpy(g->view,view,65);g->common=safe;
    } else {
        M4EscapeGroup *g=&a->group[a->table[slot]-1];
        int i=0,j=0,n=0;
        while(i<g->common.count&&j<safe.count) {
            Move x=g->common.moves[i],y=safe.moves[j];
            if(x==y){g->common.moves[n++]=x;++i;++j;}
            else if(x<y)++i;else ++j;
        }
        g->common.count=n;
    }
    M4EscapeGroup *g=&a->group[a->table[slot]-1];
    ++g->worlds;g->individually_dead+=safe.count==0;
    return 1;
}
