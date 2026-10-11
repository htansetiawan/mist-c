#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static void nap(void){struct timespec t={0,5000000};nanosleep(&t,NULL);}
int main(void){
    const char *mode=getenv("M4SF_FAKE_MODE");if(!mode)mode="ok";
    setvbuf(stdout,NULL,_IONBF,0);char line[256];
    while(fgets(line,sizeof(line),stdin)){
        if(!strcmp(line,"uci\n")){
            if(!strcmp(mode,"startupchat"))for(;;){puts("id name incomplete startup");nap();}
            puts("uciok");
        }else if(!strcmp(line,"isready\n"))puts("readyok");
        else if(!strcmp(line,"go depth 1\n")){
            if(!strcmp(mode,"dead"))return 0;
            puts("info depth 1 multipv 1 score cp 17 pv e2e4");
            if(!strcmp(mode,"silent"))for(;;)nap();
            if(!strcmp(mode,"dribble"))for(;;){puts("info string still working");nap();}
            if(!strcmp(mode,"flood"))for(;;)puts("info string busy");
            if(!strcmp(mode,"partial"))for(;;){putchar('x');nap();}
            if(!strcmp(mode,"overlong"))for(int i=0;i<17000;++i)putchar('x');
            puts("info depth 1 multipv 2 score cp 9 lowerbound pv d2d4");
            puts("info depth 1 multipv 2 score cp malformed pv d2d4");
            puts("info depth 1 multipv 2 score cp 999999999999999999999999 pv d2d4");
            puts("info depth 1 multipv 2 score mate 3 pv g1f3");
            puts("bestmove e2e4");
        }
    }
    return 0;
}
