/* Frozen replay features; no runtime integration. Input: game ID W B, then UCI:board
 * moves. '-' excludes a side's rows. Output native little-endian uint32
 * feature count; each record uint32 n, played, game-index, ply, side followed
 * by float32[n][features]. IDs/teams are retained in the Python manifest.
 * Stop on bad input rather than silently train on a truncated game. */
#include "../src/m4movefeatures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

int main(int argc,char **argv) {
    if(argc!=3){fprintf(stderr,"usage: m4_move_features IN OUT\n");return 2;}
    FILE *in=fopen(argv[1],"r"),*out=fopen(argv[2],"wb");if(!in||!out){perror("open");return 2;}
    fow_init();uint32_t nf=M4MM_FEATURES;if(fwrite(&nf,4,1,out)!=1)return 3;
    static char line[65536];float x[256][M4MM_FEATURES];int game=-1,expect_moves=0;
    char id[80],white[80],black[80];long records=0,actions=0;clock_t begin=clock();
    while(fgets(line,sizeof(line),in)) {
        if(!strchr(line,'\n')){fprintf(stderr,"line too long or unterminated\n");return 3;}
        if(!expect_moves) {
            if(sscanf(line,"game %79s %79s %79s",id,white,black)!=3)return 3;
            ++game;expect_moves=1;continue;
        }
        Position p;pos_start(&p);Move previous=0;int ply=0;
        for(char *tok=strtok(line," \n");tok;tok=strtok(NULL," \n"),++ply) {
            char *expected=strchr(tok,':');if(!expected||strlen(expected+1)!=64)return 3;
            *expected++='\0';
            MoveList l;generate_moves(&p,&l);int played=-1;
            for(int a=0;a<l.count;++a){char uci[6];move_to_uci(l.moves[a],uci);if(!strcmp(uci,tok))played=a;}
            if(played<0){fprintf(stderr,"illegal move %s game=%s ply=%d\n",tok,id,ply);return 3;}
            if(strcmp(p.side==WHITE?white:black,"-")) {
                m4_move_features(&p,&l,previous,x);
                uint32_t header[5]={(uint32_t)l.count,(uint32_t)played,(uint32_t)game,(uint32_t)ply,(uint32_t)p.side};
                if(fwrite(header,4,5,out)!=5||fwrite(x,sizeof(float),(size_t)l.count*M4MM_FEATURES,out)!=(size_t)l.count*M4MM_FEATURES)return 3;
                ++records;actions+=l.count;
            }
            Move m=l.moves[played];Undo u;make_move(&p,m,&u);previous=m;
            char board[65];pos_to_board_string(&p,board);
            if(strcmp(board,expected)){fprintf(stderr,"board mismatch game=%s ply=%d\n",id,ply+1);return 3;}
        }
        expect_moves=0;
    }
    if(expect_moves||ferror(in)||ferror(out))return 3;
    fclose(in);if(fclose(out))return 3;
    fprintf(stderr,"games=%d positions=%ld actions=%ld cpu_seconds=%.6f\n",game+1,records,actions,(double)(clock()-begin)/CLOCKS_PER_SEC);
    return 0;
}
