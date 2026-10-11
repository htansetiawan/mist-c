/* Astra-owned bounded Stockfish transport. No dependency on sfeval's global
 * process. Absolute CLOCK_MONOTONIC deadlines cover the entire exchange,
 * including chatter and partial lines. A failed request publishes no scores
 * and disables this instance; it never relaunches during a decision. */
#ifndef MIST_M4SF_H
#define MIST_M4SF_H
#include "fow.h"
typedef struct M4SF M4SF;
typedef struct { long calls, completed, failed, skipped; double seconds; } M4SFStats;
M4SF *m4sf_open(const char *path, double startup_seconds);
int m4sf_ready(const M4SF *s);
/* Offline diagnostic only: ask Stockfish to log its UCI exchanges. The caller
 * must verify the log exists; readyok does not acknowledge individual options. */
int m4sf_debug_log(M4SF *s,const char *path,double seconds);
int m4sf_children(M4SF *s,const Position *p,const MoveList *moves,
                  double deadline,int cp[256],int found[256]);
void m4sf_stats(const M4SF *s,M4SFStats *out);
void m4sf_close(M4SF *s);
double m4sf_now(void);
#endif
