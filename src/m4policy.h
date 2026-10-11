/* Experimental post-solve policy correction. Risks are gadget-weighted
 * fatal shares, not calibrated probabilities or an equilibrium guarantee. */
#ifndef MIST_M4POLICY_H
#define MIST_M4POLICY_H
#include "kluss.h"
/* Map each action to its minimum-risk dominator (ties: value, action order).
 * A dominator has risk lower by >1e-9 and value >= the original value.
 * Push the whole played distribution through that map, including targets
 * with zero solver support. O(A log A), A<=256. Return redirected positive-
 * support actions, or -1 on invalid input; outputs are unchanged on error. */
int m4_policy_redirect(const double *risk,const double *value,const double *play,
                       int count,double *redirected,int *destination);
/* Replace one P0 behavior row and recompute all raw values/reaches and the
 * stored forest mean. On failure restore the old row; scratch arrays may be
 * changed. No regrets or gadget-controller probabilities are changed. */
int m4_policy_retarget(KlussBlueprint *b,int infoset,const double *play,
                       double *value,double *cf,double *opponent);
#endif
