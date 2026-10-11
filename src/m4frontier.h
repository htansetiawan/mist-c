/* Bounded scheduling index for future own-information-set leaves.
 * A threat in one world schedules all queued aliases, including quiet worlds.
 * Keys are perfect-recall own-history tags, never hidden opponent actions.
 * No values, probabilities, regrets or moves are changed here.
 */
#ifndef MIST_M4FRONTIER_H
#define MIST_M4FRONTIER_H
#include <stdint.h>
typedef struct M4Frontier M4Frontier;
M4Frontier *m4_frontier_create(int capacity);
void m4_frontier_free(M4Frontier *q);
/* Each node must be added once. Fixed total insertion cap (not live size).
 * Returns 1 accepted, 0 full/invalid. Overflow falls back to ordinary search.
 * Own history AND ply identify a group. Threat status persists for that group.
 */
int m4_frontier_add(M4Frontier *q,uint64_t own,int ply,int node,int threatened);
/* Shallowest urgent group first; FIFO within a group, stable group tie order.
 * Caller skips nodes already expanded/terminal; this queue does not own them.
 */
int m4_frontier_pop(M4Frontier *q);
int m4_frontier_dropped(const M4Frontier *q);
#endif
