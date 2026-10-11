/* Offline sequential-belief experiment. Truth is used only by audit code;
 * belief updates consume our recorded moves and observations. No search. */
#include "../src/m4movefeatures.h"
#include "../src/front.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static float theta[M4MM_FEATURES];
static int width;
static long full_calls,cheap_calls;
void __real_move_logits(const Position*,const MoveList*,Move,int,float*);
void __wrap_move_logits(const Position *p,const MoveList *l,Move previous,int full,float *out) {
    if(!full){++cheap_calls;__real_move_logits(p,l,previous,full,out);return;}
    ++full_calls;
    if(width==M4MM_FEATURES) {
        float x[256][M4MM_FEATURES];m4_move_features(p,l,previous,x);
        for(int a=0;a<l->count;++a){out[a]=0;for(int k=0;k<width;++k)out[a]+=theta[k]*x[a][k];}
    } else {
        float x[MM_FEATURES];
        for(int a=0;a<l->count;++a){move_features(p,l->moves[a],previous,1,x);out[a]=0;
            for(int k=0;k<width;++k)out[a]+=theta[k]*x[k];}
    }
}
static Move parse_move(const char *uci) {
    if(strlen(uci)<4||strlen(uci)>5)return 0;
    int promo=NO_PIECE;
    if(uci[4]){static const char promotions[]="nbrq";const char *p=strchr(promotions,uci[4]);
        if(!p)return 0;
        promo=KNIGHT+(int)(p-promotions);}
    if(uci[0]<'a'||uci[0]>'h'||uci[2]<'a'||uci[2]>'h'||uci[1]<'1'||uci[1]>'8'||uci[3]<'1'||uci[3]>'8')return 0;
    return MAKE_MOVE((uci[1]-'1')*8+uci[0]-'a',(uci[3]-'1')*8+uci[2]-'a',promo);
}
static void audit(const Belief *b,const Position *truth,const char *view,const char *id,int ply,Move chosen,
                  int rebuilds,int collapsed,double update_seconds) {
    double total=0,attack=0,fatal=0,share=0;int supported=0,inconsistent=0;
    float max=-INFINITY;for(size_t i=0;i<b->count;++i)if(b->logw[i]>max)max=b->logw[i];
    uint64_t key=position_key(truth);
    for(size_t i=0;i<b->count;++i) {
        const Position *p=&b->items[i];double w=exp((double)b->logw[i]-max);total+=w;
        if(position_key(p)==key){share+=w;supported=1;}
        int king=king_square(p,b->us);if(king>=0&&is_attacked(p,king,b->us^1))attack+=w;
        char observed[65];observation(p,b->us,observed);if(strcmp(view,observed))++inconsistent;
        Position q=*p;Undo u;make_move(&q,chosen,&u);if(mover_left_king_capturable(&q))fatal+=w;
    }
    Position child=*truth;Undo u;make_move(&child,chosen,&u);int king=king_square(truth,b->us);
    char uci[6];move_to_uci(chosen,uci);
    printf("{\"game\":\"%s\",\"side\":%d,\"ply\":%d,\"move\":\"%s\",\"worlds\":%zu,\"overflowed\":%d,\"truth_present\":%d,\"truth_share\":%.12g,\"attack_probability\":%.12g,\"fatal_probability\":%.12g,\"actual_attack\":%d,\"actual_fatal\":%d,\"inconsistent\":%d,\"rebuilds\":%d,\"collapses\":%d,\"update_cpu_seconds\":%.6f,\"full_calls\":%ld,\"cheap_calls\":%ld}\n",
        id,b->us,ply,uci,b->count,b->overflowed,supported,total?share/total:0,total?attack/total:0,total?fatal/total:0,
        king>=0&&is_attacked(truth,king,b->us^1),mover_left_king_capturable(&child),inconsistent,rebuilds,collapsed,
        update_seconds,full_calls,cheap_calls);fflush(stdout);
    if(inconsistent){fprintf(stderr,"observation mismatch\n");exit(4);}
}
int main(int argc,char **argv) {
    if(argc!=5){fprintf(stderr,"usage: m4_prior_replay INPUT MODEL_TEXT CAP SEED\n");return 2;}
    FILE *weights=fopen(argv[2],"r");if(!weights)return 2;
    if(fscanf(weights,"%d",&width)!=1||(width!=26&&width!=32))return 3;
    for(int i=0;i<width;++i)if(fscanf(weights,"%f",&theta[i])!=1||!isfinite(theta[i]))return 3;
    fclose(weights);FILE *in=fopen(argv[1],"r");if(!in)return 2;
    size_t cap=(size_t)strtoull(argv[3],NULL,10);if(!cap||cap>1000000)return 2;
    uint64_t seed=strtoull(argv[4],NULL,10);fow_init();zobrist_init();
    char line[512],id[16]="",view[65];int have=0,us=0,ply=0,rebuilds=0,collapsed=0,terminal=0;
    Belief b={0};Front front;Position truth;double update_seconds=0;
    while(fgets(line,sizeof(line),in)) {
        if(!strncmp(line,"game ",5)) {
            if(sscanf(line,"game %8s %d",id,&us)!=2||(us!=0&&us!=1))return 3;
            if(have)belief_free(&b);
            belief_init_start(&b,us,cap);b.rng^=seed;front_init(&front,us);pos_start(&truth);
            observation(&truth,us,view);have=1;ply=rebuilds=collapsed=terminal=0;update_seconds=0;full_calls=cheap_calls=0;continue;
        }
        char uci[8],board[65],white[65],black[65];
        if(!have||terminal||sscanf(line,"%7s %64s %64s %64s",uci,board,white,black)!=4)return 3;
        Move move=parse_move(uci);if(!move)return 3;
        int own=truth.side==us;
        if(own) {
            MoveList allowed;view_moves(view,us,front.castle,&allowed);int legal=0;
            for(int i=0;i<allowed.count;++i)if(allowed.moves[i]==move)legal=1;
            if(!legal){fprintf(stderr,"own move not observation-legal %s ply%d\n",id,ply);return 3;}
            audit(&b,&truth,view,id,ply,move,rebuilds,collapsed,update_seconds);
            front_play(&front,view,move);
        }
        /* Truth moves validate the replay; this Position is never passed to a belief updater. */
        MoveList actual;generate_moves(&truth,&actual);int legal=0;
        for(int i=0;i<actual.count;++i)if(actual.moves[i]==move)legal=1;
        if(!legal)return 3;
        Undo undo;make_move(&truth,move,&undo);++ply;char check[65];pos_to_board_string(&truth,check);
        if(strcmp(board,check))return 3;
        observation(&truth,WHITE,check);if(strcmp(check,white))return 3;
        observation(&truth,BLACK,check);if(strcmp(check,black))return 3;
        memcpy(view,us==WHITE?white:black,65);
        terminal=mover_left_king_capturable(&truth)||king_square(&truth,WHITE)<0||king_square(&truth,BLACK)<0;
        if(terminal)continue;
        clock_t begin=clock();size_t before=b.count;
        if(b.count){if(own)belief_after_own(&b,move,view);else belief_after_opponent(&b,view);}
        if(before&&!b.count)++collapsed;
        front_see(&front,view);
        if(!b.count&&!own){belief_resample(&b,view,front.their,front.castle,cap<2000?cap:2000,60000);++rebuilds;}
        update_seconds+=(double)(clock()-begin)/CLOCKS_PER_SEC;
    }
    if(have)belief_free(&b);
    fclose(in);return 0;
}
