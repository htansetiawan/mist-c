/* Paired feature-cost measurement over the same frozen replay positions. */
#include "../src/m4movefeatures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {Position p;Move previous;} Sample;
static Sample sample[4096];
static volatile double sink;
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    fow_init();FILE *in=fopen(argv[1],"r");if(!in)return 2;
    static char line[65536];int n=0,games=0;long seen=0;
    /* A fixed stride covers phases and many games without model-based selection. */
    while(n<4096&&fgets(line,sizeof(line),in)) {
        if(strncmp(line,"game ",5))return 3;
        if(!fgets(line,sizeof(line),in))return 3;
        Position p;pos_start(&p);Move previous=0;++games;
        for(char *tok=strtok(line," \n");tok;tok=strtok(NULL," \n")) {
            char *colon=strchr(tok,':');if(!colon)return 3;*colon='\0';
            MoveList list;generate_moves(&p,&list);int found=-1;
            for(int i=0;i<list.count;++i){char uci[6];move_to_uci(list.moves[i],uci);if(!strcmp(uci,tok))found=i;}
            if(found<0)return 3;
            if(seen++%17==0&&n<4096){sample[n].p=p;sample[n++].previous=previous;}
            Undo undo;previous=list.moves[found];make_move(&p,previous,&undo);
        }
    }
    fclose(in);long actions=0;double elapsed[2]={0,0};
    for(int rep=0;rep<6;++rep)for(int order=0;order<2;++order) {
        int kind=(rep+order)%2;clock_t begin=clock();
        for(int i=0;i<n;++i) {
            MoveList list;generate_moves(&sample[i].p,&list);
            if(kind) {
                float out[256][M4MM_FEATURES];m4_move_features(&sample[i].p,&list,sample[i].previous,out);
                for(int a=0;a<list.count;++a)sink+=out[a][MM_CHECK]+out[a][M4MM_HIDDEN_CHECK];
            } else {
                float out[MM_FEATURES];
                for(int a=0;a<list.count;++a){move_features(&sample[i].p,list.moves[a],sample[i].previous,1,out);sink+=out[MM_CHECK];}
            }
            if(rep==0&&order==0)actions+=list.count;
        }
        elapsed[kind]+=(double)(clock()-begin)/CLOCKS_PER_SEC;
    }
    printf("{\"positions\":%d,\"games_scanned\":%d,\"actions_per_pass\":%ld,\"repeats\":6,\"control_cpu_seconds\":%.6f,\"visibility_cpu_seconds\":%.6f,\"ratio\":%.6f,\"checksum\":%.1f}\n",n,games,actions,elapsed[0],elapsed[1],elapsed[1]/elapsed[0],sink);
    return 0;
}
