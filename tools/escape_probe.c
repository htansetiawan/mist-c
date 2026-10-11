/* Offline stdin: side own-history(hex) rights(hex) ep halfmove board64.
 * Complete positions are diagnostic oracle inputs, never arena observations. */
#include "../src/m4escape.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fow_init();M4Escape *a=m4_escape_create(4096);if(!a)return 2;
    char line[256];int n=0;
    while(fgets(line,sizeof(line),stdin)) {
        Position p;int side,ep,half;uint64_t history,rights;char board[65],extra;
        if(sscanf(line,"%d %" SCNx64 " %" SCNx64 " %d %d %64s %c",&side,&history,&rights,&ep,&half,board,&extra)!=6
           ||strlen(board)!=64||pos_from_board_string(&p,board,side,rights,ep)<0){m4_escape_free(a);return 3;}
        p.halfmove=half;
        if(m4_escape_add(a,&p,history)!=1){m4_escape_free(a);return 4;}
        ++n;
    }
    printf("{\"complete\":%d,\"worlds\":%d,\"groups\":[",m4_escape_complete(a),n);
    for(int i=0;i<m4_escape_groups(a);++i) {
        const M4EscapeGroup *g=m4_escape_group(a,i);
        printf("%s{\"view\":\"%s\",\"history\":\"%" PRIx64 "\",\"worlds\":%zu,\"individually_dead\":%zu,\"safe\":[",i?",":"",g->view,g->history,g->worlds,g->individually_dead);
        for(int j=0;j<g->common.count;++j){char uci[6];move_to_uci(g->common.moves[j],uci);printf("%s\"%s\"",j?",":"",uci);}
        printf("]}");
    }
    puts("]}");m4_escape_free(a);return 0;
}
