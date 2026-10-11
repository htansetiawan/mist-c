#define _POSIX_C_SOURCE 200809L
#include "m4sf.h"
#include "sfeval.h" /* pure FEN formatter only; global UCI client is unused */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct M4SF { int fd,ready; pid_t pid; char buf[16384]; size_t start,have; M4SFStats stats; };
double m4sf_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
static void reap(M4SF *s){if(s->pid>0){pid_t p=waitpid(s->pid,NULL,WNOHANG);if(p==s->pid||(p<0&&errno==ECHILD))s->pid=-1;}}
static void disable(M4SF *s){
    s->ready=0;if(s->fd>=0){close(s->fd);s->fd=-1;}
    if(s->pid>0)kill(s->pid,SIGKILL);
    reap(s);
}
void m4sf_close(M4SF *s){
    if(!s)return;
    disable(s);
    /* Reaping is outside search. Even shutdown has a bounded wait; no blocked
     * quit write or unbounded waitpid. The bot process exits after close. */
    double until=m4sf_now()+.05;
    while(s->pid>0&&m4sf_now()<until){struct timespec pause={0,1000000};nanosleep(&pause,NULL);reap(s);}
    free(s);
}
int m4sf_ready(const M4SF *s){return s&&s->ready;}
void m4sf_stats(const M4SF *s,M4SFStats *out){if(out){memset(out,0,sizeof(*out));if(s)*out=s->stats;}}
static int wait_io(M4SF *s,short events,double deadline){
    for(;;){
        double left=deadline-m4sf_now();if(!(left>0))return 0;
        int ms=left>1?1000:(int)ceil(left*1000);
        struct pollfd fd={s->fd,events,0};int r=poll(&fd,1,ms);
        if(r<0&&errno==EINTR)continue;
        if(r<0)return 0;
        if(!r)continue;
        return (fd.revents&events)!=0; /* HUP alone is a failure */
    }
}
static int send_all(M4SF *s,const char *line,double deadline){
    size_t n=strlen(line),off=0;
    while(off<n){
        if(m4sf_now()>=deadline)return 0;
        ssize_t w=send(s->fd,line+off,n-off,MSG_NOSIGNAL);
        if(w>0){off+=(size_t)w;continue;}
        if(w<0&&errno==EINTR)continue;
        if(w<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&wait_io(s,POLLOUT,deadline))continue;
        return 0;
    }
    return 1;
}
static int get_line(M4SF *s,char *out,size_t cap,double deadline){
    for(;;){
        /* Check even with a perpetually nonempty buffer: chatter cannot reset
         * the deadline or starve the caller's time check. */
        if(m4sf_now()>=deadline)return 0;
        for(size_t i=s->start;i<s->have;++i)if(s->buf[i]=='\n'){
            size_t n=i-s->start;if(n>=cap)return 0;
            memcpy(out,s->buf+s->start,n);out[n]='\0';s->start=i+1;return 1;
        }
        if(s->start){memmove(s->buf,s->buf+s->start,s->have-s->start);s->have-=s->start;s->start=0;}
        if(s->have==sizeof(s->buf))return 0; /* reject overlong line */
        ssize_t r=recv(s->fd,s->buf+s->have,sizeof(s->buf)-s->have,0);
        if(r>0){s->have+=(size_t)r;continue;}
        if(r<0&&errno==EINTR)continue;
        if(r<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&wait_io(s,POLLIN,deadline))continue;
        return 0;
    }
}
static int wait_word(M4SF *s,const char *word,double deadline){
    char line[16384];
    while(get_line(s,line,sizeof(line),deadline)){
        size_t n=strlen(line);if(n&&line[n-1]=='\r')line[--n]=0;
        if(!strcmp(line,word))return 1;
    }
    return 0;
}
M4SF *m4sf_open(const char *path,double seconds){
    if(!path||!path[0]||!isfinite(seconds)||seconds<=0)return NULL;
    const double started=m4sf_now(),deadline=started+seconds;
    M4SF *s=calloc(1,sizeof(*s));if(!s)return NULL;s->fd=-1;s->pid=-1;
    int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)){free(s);return NULL;}
    if(fcntl(pair[0],F_SETFD,FD_CLOEXEC)<0||fcntl(pair[1],F_SETFD,FD_CLOEXEC)<0){close(pair[0]);close(pair[1]);free(s);return NULL;}
    s->pid=fork();
    if(s->pid<0){close(pair[0]);close(pair[1]);free(s);return NULL;}
    if(s->pid==0){
        close(pair[0]);
        if(dup2(pair[1],STDIN_FILENO)<0||dup2(pair[1],STDOUT_FILENO)<0)_exit(126);
        if(pair[1]>STDOUT_FILENO)close(pair[1]);
        execl(path,path,(char *)NULL);_exit(127);
    }
    close(pair[1]);s->fd=pair[0];
    int flags=fcntl(s->fd,F_GETFL,0);
    const char *phase="nonblocking setup";
    if(flags<0||fcntl(s->fd,F_SETFL,flags|O_NONBLOCK)<0)goto failed;
    phase="uci handshake";
    if(!send_all(s,"uci\n",deadline)||!wait_word(s,"uciok",deadline))goto failed;
    phase="ready handshake";
    if(!send_all(s,"setoption name Threads value 1\nsetoption name Hash value 16\nsetoption name MultiPV value 256\nisready\n",deadline)||
       !wait_word(s,"readyok",deadline))goto failed;
    s->ready=1;return s;
failed:
    fprintf(stderr,"m4sf: startup failed phase=%s elapsed=%.6f deadline_expired=%d\n",
            phase,m4sf_now()-started,m4sf_now()>=deadline);
    m4sf_close(s);return NULL;
}
int m4sf_debug_log(M4SF *s,const char *path,double seconds) {
    if(!m4sf_ready(s)||!path||!*path||strlen(path)>1024||strchr(path,'\n')||
       strchr(path,'\r')||!isfinite(seconds)||seconds<=0)return 0;
    char command[1100];double deadline=m4sf_now()+seconds;
    snprintf(command,sizeof(command),"setoption name Debug Log File value %s\nisready\n",path);
    if(send_all(s,command,deadline)&&wait_word(s,"readyok",deadline))return 1;
    disable(s);return 0;
}
static int supported(const Position *p){
    if(__builtin_popcountll(p->by_type[KING]&p->by_color[WHITE])!=1||
       __builtin_popcountll(p->by_type[KING]&p->by_color[BLACK])!=1||
       (p->by_type[PAWN]&0xFF000000000000FFULL))return 0;
    return !is_attacked(p,king_square(p,p->side^1),p->side);
}
static int integer(const char *str,int *out){
    char *end;errno=0;long v=strtol(str,&end,10);
    if(end==str||(*end&&*end!=' '&&*end!='\r')||errno||v<INT_MIN||v>INT_MAX)return 0;
    *out=(int)v;return 1;
}
int m4sf_children(M4SF *s,const Position *p,const MoveList *l,double deadline,int cp[256],int found[256]){
    if(!cp||!found)return 0;
    memset(cp,0,256*sizeof(*cp));memset(found,0,256*sizeof(*found));
    if(!s)return 0;
    reap(s);
    if(!s->ready||!p||!l||l->count<0||l->count>256||!isfinite(deadline)||
       m4sf_now()>=deadline||!supported(p)){++s->stats.skipped;return 0;}
    const double start=m4sf_now();++s->stats.calls;
    char fen[100],cmd[180],uci[256][6],line[16384];
    int scratch[256]={0},seen[256]={0},scored=0;
    for(int i=0;i<l->count;++i)move_to_uci(l->moves[i],uci[i]);
    sf_fen(p,fen);snprintf(cmd,sizeof(cmd),"position fen %s\ngo depth 1\n",fen);
    if(!send_all(s,cmd,deadline))goto fail;
    while(get_line(s,line,sizeof(line),deadline)){
        if(strstr(line,"CRITICAL ERROR"))goto fail;
        if(!strncmp(line,"bestmove ",9)){
            memcpy(cp,scratch,sizeof(scratch));memcpy(found,seen,sizeof(seen));
            ++s->stats.completed;s->stats.seconds+=m4sf_now()-start;return scored;
        }
        if(strncmp(line,"info ",5)||!strstr(line," multipv ")||
           strstr(line,"lowerbound")||strstr(line,"upperbound"))continue;
        const char *score=strstr(line," score "),*pv=strstr(line," pv ");
        if(!score||!pv)continue;
        int value;
        if(!strncmp(score+7,"cp ",3)){if(!integer(score+10,&value))continue;}
        else if(!strncmp(score+7,"mate ",5)){
            int distance;if(!integer(score+12,&distance))continue;
            /* Bound the conversion even for malformed remote numbers. */
            long long mag=llabs((long long)distance);if(mag>100)mag=100;
            value=(distance>0?1:-1)*(20000-(int)mag*100);
        }else continue;
        char move[8]={0};if(sscanf(pv+4,"%7s",move)!=1)continue;
        for(int i=0;i<l->count;++i)if(!strcmp(move,uci[i])){
            scored+=!seen[i];seen[i]=1;scratch[i]=value;break;
        }
    }
fail:
    fprintf(stderr,"m4sf: request aborted elapsed=%.6f deadline_expired=%d; partial scores discarded\n",m4sf_now()-start,m4sf_now()>=deadline);
    ++s->stats.failed;s->stats.seconds+=m4sf_now()-start;disable(s);return 0;
}
