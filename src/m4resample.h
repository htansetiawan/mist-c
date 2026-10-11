#ifndef MIST_M4RESAMPLE_H
#define MIST_M4RESAMPLE_H
#include <stddef.h>
#include <stdint.h>
/* Experimental weighted sample without replacement. Returns 1 on success.
 * Writes min(n,keep) indices and threshold-corrected log importance weights.
 * The unnormalized weighted sums are unbiased; self-normalized ratios are not
 * unbiased at finite sample size. No integration into the live belief yet.
 * Input weights must be finite. NULL weights mean uniform. keep must be > 0.
 * O(n log(keep+1)) work / O(keep+1) auxiliary storage when n > keep. */
int m4_weighted_cap(const float *logw,size_t n,size_t keep,uint64_t *rng,
                    size_t *indices,float *corrected_logw);
#endif
