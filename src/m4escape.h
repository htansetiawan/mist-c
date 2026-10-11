/* Bounded, observation-grouped immediate escape sets. Diagnostic primitive;
 * not a policy, posterior or implementation of the M4 solving algorithm.
 *
 * A different safe response on each hidden board need not be an executable
 * strategy. Intersect safe moves among states with the SAME own information
 * history and observation. An empty intersection with every state escapable
 * is an information conflict, not a physical forced loss.
 */
#ifndef MIST_M4ESCAPE_H
#define MIST_M4ESCAPE_H
#include "fow.h"
#include <stddef.h>

typedef struct M4Escape M4Escape;
typedef struct {
    uint64_t history;              /* caller's perfect-recall information tag */
    bb_t own_castle_rooks;
    int side;
    char view[65];
    size_t worlds, individually_dead;
    MoveList common;               /* sorted Move codes; legal and king-safe in EVERY added state */
} M4EscapeGroup;

/* Allocates all group/hash storage once. max_groups must be 1..65536. */
M4Escape *m4_escape_create(int max_groups);
void m4_escape_free(M4Escape *audit);
/* Add a nonterminal position at OUR turn. Caller excludes historical
 * repetition terminals and supplies the SAME history tag only for genuinely
 * indistinguishable histories. Exact observation and own rights also enter
 * the key. Hidden opponent moves/positions must NOT enter that history tag.
 *
 * Return 1 on add, 0 on capacity exhaustion, -1 on invalid input. A failure
 * permanently marks the audit incomplete; represented conflicts remain valid
 * witnesses, but no absence-of-conflict conclusion covers omitted states.
 * Per add: expected O(1) hash lookup, O(A log A) escape ordering, O(A) merge.
 * Retains no world boards; storage O(max_groups * 256).
 */
int m4_escape_add(M4Escape *audit, const Position *position, uint64_t history);
int m4_escape_complete(const M4Escape *audit);
int m4_escape_groups(const M4Escape *audit);
const M4EscapeGroup *m4_escape_group(const M4Escape *audit, int index);
#endif
