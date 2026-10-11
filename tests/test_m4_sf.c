#define _POSIX_C_SOURCE 200809L
#include "../src/m4sf.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>
static void empty(const int *cp,const int *found){for(int i=0;i<256;++i)assert(!cp[i]&&!found[i]);}
int main(int argc,char **argv){
    assert(argc==2);fow_init();Position p;pos_start(&p);MoveList m;generate_moves(&p,&m);
    int cp[256],found[256];struct sigaction before,after;
    assert(!sigaction(SIGPIPE,NULL,&before));
    assert(!m4sf_open("/nonexistent/engine",.1));
    assert(!m4sf_open(argv[1],0));
    setenv("M4SF_FAKE_MODE","startupchat",1);
    double start=m4sf_now();assert(!m4sf_open(argv[1],.06));
    assert(m4sf_now()-start<.3);
    setenv("M4SF_FAKE_MODE","startupslow",1);
    start=m4sf_now();assert(!m4sf_open(argv[1],.06));assert(m4sf_now()-start<.3);
    M4SF *slow=m4sf_open(argv[1],5.0);assert(m4sf_ready(slow));
    assert(m4sf_children(slow,&p,&m,m4sf_now()+.1,cp,found)==2);m4sf_close(slow);
    setenv("M4SF_FAKE_MODE","ok",1);M4SF *sf=m4sf_open(argv[1],.5);assert(m4sf_ready(sf));
    assert(!m4sf_debug_log(sf,"invalid\ncommand",.1));assert(m4sf_ready(sf));
    assert(!m4sf_debug_log(sf,"invalid\rcommand",.1));assert(m4sf_ready(sf));
    assert(!m4sf_debug_log(sf,"/tmp/m4-test-log",0));assert(m4sf_ready(sf));
    assert(m4sf_debug_log(sf,"/tmp/m4-test-log",.1));
    assert(m4sf_children(sf,&p,&m,m4sf_now()+.1,cp,found)==2);
    int matched=0;
    for(int a=0;a<m.count;++a){char u[6];move_to_uci(m.moves[a],u);
        if(!strcmp(u,"e2e4")){assert(found[a]&&cp[a]==17);++matched;}
        else if(!strcmp(u,"g1f3")){assert(found[a]&&cp[a]==19700);++matched;}
        else assert(!found[a]);
    }
    assert(matched==2);
    memset(cp,1,sizeof(cp));memset(found,1,sizeof(found));
    assert(!m4sf_children(sf,&p,&m,m4sf_now()-1,cp,found));empty(cp,found);assert(m4sf_ready(sf));
    Position bad=p;pos_remove(&bad,60);assert(!m4sf_children(sf,&bad,&m,m4sf_now()+.1,cp,found));empty(cp,found);assert(m4sf_ready(sf));
    assert(m4sf_children(sf,&p,&m,m4sf_now()+.1,cp,found)==2);
    M4SFStats stats;m4sf_stats(sf,&stats);assert(stats.calls==2&&stats.completed==2&&stats.skipped==2);m4sf_close(sf);
    const char *modes[]={"silent","dribble","flood","partial","dead","overlong"};
    for(size_t k=0;k<sizeof(modes)/sizeof(*modes);++k){
        setenv("M4SF_FAKE_MODE",modes[k],1);sf=m4sf_open(argv[1],.5);assert(sf);
        start=m4sf_now();assert(!m4sf_children(sf,&p,&m,start+.06,cp,found));
        double elapsed=m4sf_now()-start;assert(elapsed<.3);empty(cp,found);assert(!m4sf_ready(sf));
        assert(!m4sf_children(sf,&p,&m,m4sf_now()+.1,cp,found));empty(cp,found);
        m4sf_stats(sf,&stats);assert(stats.calls==1&&stats.failed==1&&stats.completed==0&&stats.skipped==1);
        m4sf_close(sf);printf("%-10s: %.4fs; partial scores discarded, instance disabled\n",modes[k],elapsed);
    }
    assert(!sigaction(SIGPIPE,NULL,&after));assert(before.sa_handler==after.sa_handler);
    assert(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD); /* all owned children reaped */
    const char *real=getenv("MIST_STOCKFISH");
    if(real){sf=m4sf_open(real,1.0);assert(sf);
        start=m4sf_now();int n=m4sf_children(sf,&p,&m,start+.5,cp,found);assert(n==m.count);
        printf("real Stockfish: %d/%d starting actions, %.4fs\n",n,m.count,m4sf_now()-start);m4sf_close(sf);
    }
    puts("test_m4_sf: whole-request deadlines, atomic scores, malformed input, shutdown and SIGPIPE isolation passed");
}
