/* Offline exact one-response information conflicts from a single known board.
 * No engine integration; caller must not treat an oracle board as a belief.
 * Input: id side rights(hex) ep halfmove board64. Output one JSON row per move.
 * Group opponent replies by our resulting observation, then count safe actions
 * shared across each group. Repetition history is deliberately out of scope.
 */
#include "../src/fow.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CODES 32768
typedef struct { Position p; char view[65]; } World;
static World worlds[256];
/* Generation stamps avoid clearing the complete action universe per group. */
static unsigned stamps[CODES], serial;
static unsigned short safe_count[CODES];
static Move touched[CODES];

static int compare_world(const void *a,const void *b) {
    return memcmp(((const World *)a)->view,((const World *)b)->view,64);
}
/* winner: 0 white, 1 black, 2 draw, -1 ongoing; matches local referee order. */
static int terminal(Position *p,MoveList *legal) {
    if(mover_left_king_capturable(p))return p->side;
    if(p->halfmove>=100)return 2;
    generate_moves(p,legal);
    return legal->count?-1:(p->side^1);
}
static const char *result(int winner) {
    return winner==WHITE?"1-0":winner==BLACK?"0-1":"1/2-1/2";
}
static const char *reason(Position *p) {
    return mover_left_king_capturable(p)?"King capturable":p->halfmove>=100?"50-move rule":"No legal moves";
}
static int label(const char *id,const Position *position,Move setup) {
    char uci[6];move_to_uci(setup,uci);Position p=*position;Undo undo;
    make_move(&p,setup,&undo);MoveList replies;
    int end=terminal(&p,&replies);
    printf("{\"position\":\"%s\",\"move\":\"%s\"",id,uci);
    if(end>=0){printf(",\"immediate_outcome\":[\"%s\",\"%s\"]}\n",result(end),reason(&p));return 1;}
    int n=0,terminal_count=0;
    for(int r=0;r<replies.count;++r){
        Position child=p;Undo u;MoveList responses;make_move(&child,replies.moves[r],&u);
        if(terminal(&child,&responses)>=0){++terminal_count;continue;}
        worlds[n].p=child;observation(&child,position->side,worlds[n].view);++n;
    }
    qsort(worlds,(size_t)n,sizeof(World),compare_world);
    int group_count=0,dangerous=0,sum_min=0;
    printf(",\"opponent_legal_replies\":%d,\"terminal_reply_count\":%d,\"continuing_replies\":%d,\"groups\":[",replies.count,terminal_count,n);
    for(int begin=0;begin<n;){
        int stop=begin+1;
        while(stop<n&&!memcmp(worlds[begin].view,worlds[stop].view,64))++stop;
        if(++serial==0){memset(stamps,0,sizeof(stamps));serial=1;}
        int ntouched=0;
        for(int j=begin;j<stop;++j){
            Position child=worlds[j].p;MoveList actions;generate_moves(&child,&actions);
            for(int a=0;a<actions.count;++a){
                Move m=actions.moves[a];
                if(m>=CODES)return 0;
                if(stamps[m]!=serial){stamps[m]=serial;safe_count[m]=0;touched[ntouched++]=m;}
                Undo u;make_move(&child,m,&u);
                if(!mover_left_king_capturable(&child))++safe_count[m];
                unmake_move(&child,m,&u);
            }
        }
        int maximum=0,common=0;
        for(int a=0;a<ntouched;++a){
            int safe=safe_count[touched[a]];if(safe>maximum)maximum=safe;
            common+=safe==stop-begin;
        }
        int minimum=stop-begin-maximum;sum_min+=minimum;dangerous+=minimum>0;
        printf("%s{\"observation\":\"%s\",\"worlds\":%d,\"responses\":%d,\"universal_survival_count\":%d,\"minimum_fatal_worlds\":%d}",
               group_count?",":"",worlds[begin].view,stop-begin,ntouched,common,minimum);
        ++group_count;begin=stop;
    }
    printf("],\"observation_groups\":%d,\"groups_with_no_universal_survival\":%d,\"uniform_reply_min_risk\":",group_count,dangerous);
    if(n)printf("%.17g",(double)sum_min/n);else printf("null");
    puts("}");return !ferror(stdout);
}
int main(void) {
    fow_init();char line[256];long positions=0,actions=0;clock_t begin=clock();
    while(fgets(line,sizeof(line),stdin)){
        char id[65],board[65],extra;int side,ep,half;uint64_t rights;Position p;MoveList legal;
        if(!strchr(line,'\n')||sscanf(line,"%64s %d %" SCNx64 " %d %d %64s %c",id,&side,&rights,&ep,&half,board,&extra)!=6
           ||strlen(board)!=64||(side!=WHITE&&side!=BLACK)||ep< -1||ep>63||half<0||half>=100
           ||strspn(id,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")!=strlen(id)
           ||pos_from_board_string(&p,board,side,rights,ep)<0){fputs("invalid input\n",stderr);return 2;}
        p.halfmove=half;
        if(king_square(&p,WHITE)<0||king_square(&p,BLACK)<0||terminal(&p,&legal)>=0){fputs("terminal input\n",stderr);return 3;}
        for(int i=0;i<legal.count;++i){if(!label(id,&p,legal.moves[i]))return 4;++actions;}
        ++positions;
    }
    if(ferror(stdin)||fflush(stdout))return 5;
    fprintf(stderr,"positions=%ld actions=%ld cpu_seconds=%.6f\n",positions,actions,(double)(clock()-begin)/CLOCKS_PER_SEC);
    return 0;
}
