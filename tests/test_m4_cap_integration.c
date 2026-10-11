/* Compile against the frozen experimental belief-cap.c, not active belief.c. */
#define _POSIX_C_SOURCE 200809L
#include "../src/belief.h"
#include "../src/m4resample.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void prepare(Belief *b) {
    belief_init_start(b,WHITE,137);b->count=137;
    for(size_t i=0;i<b->count;++i){pos_start(&b->items[i]);b->items[i].ep=(int)(i%64);
        b->items[i].side=(int)(i%2);b->hist[i]=1000+i;b->logw[i]=-(float)(i%17)/3;}
}
int main(void) {
    fow_init();zobrist_init();Belief original,heap,corrected;prepare(&original);prepare(&heap);prepare(&corrected);
    size_t indices[17];float weights[17];uint64_t rng=corrected.rng;
    assert(m4_weighted_cap(corrected.logw,corrected.count,17,&rng,indices,weights));
    setenv("MIST_M4_CAP_MODE","0",1);assert(belief_shrink(&original,17)==17);
    setenv("MIST_M4_CAP_MODE","1",1);assert(belief_shrink(&heap,17)==17);
    setenv("MIST_M4_CAP_MODE","2",1);assert(belief_shrink(&corrected,17)==17);
    assert(original.rng==heap.rng&&heap.rng==corrected.rng&&rng==corrected.rng);
    assert(!memcmp(original.items,heap.items,17*sizeof(Position)));
    assert(!memcmp(original.hist,heap.hist,17*sizeof(uint64_t)));
    assert(!memcmp(original.logw,heap.logw,17*sizeof(float)));
    assert(!memcmp(original.items,corrected.items,17*sizeof(Position)));
    assert(!memcmp(original.hist,corrected.hist,17*sizeof(uint64_t)));
    int changed=0;
    for(size_t i=0;i<17;++i){size_t old=(size_t)(corrected.hist[i]-1000);int found=0;
        for(size_t j=0;j<17;++j)if(indices[j]==old){assert(weights[j]==corrected.logw[i]);found=1;}
        assert(found&&isfinite(corrected.logw[i]));changed|=original.logw[i]!=corrected.logw[i];
        if(i)assert(corrected.hist[i]>corrected.hist[i-1]);}
    assert(changed);assert(original.overflowed&&heap.overflowed&&corrected.overflowed);
    rng=corrected.rng;assert(belief_shrink(&corrected,17)==17&&corrected.rng==rng);
    belief_free(&original);belief_free(&heap);belief_free(&corrected);
    puts("cap integration: original/heap shrink exact parity; correction preserves boards, histories, RNG and compaction, changes only weights; no-cap path unchanged");
}
