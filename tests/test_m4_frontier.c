#include "../src/m4frontier.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    M4Frontier *q=m4_frontier_create(100);assert(q);
    assert(m4_frontier_add(q,17,6,10,0)); /* quiet alias queued before threat */
    assert(m4_frontier_pop(q)==-1);
    assert(m4_frontier_add(q,18,6,11,0)); /* different perfect-recall history */
    assert(m4_frontier_add(q,17,8,12,1)); /* same hash, different ply */
    assert(m4_frontier_add(q,17,6,13,1));
    assert(m4_frontier_add(q,17,6,14,0));
    assert(m4_frontier_pop(q)==10);assert(m4_frontier_pop(q)==13);
    assert(m4_frontier_pop(q)==14);assert(m4_frontier_pop(q)==12);
    assert(m4_frontier_pop(q)==-1);
    assert(m4_frontier_add(q,17,6,15,0)); /* threat flag survives empty queue */
    assert(m4_frontier_pop(q)==15);
    assert(m4_frontier_add(q,18,6,16,1));
    assert(m4_frontier_pop(q)==11);assert(m4_frontier_pop(q)==16);
    for(int i=0;i<80;++i)assert(m4_frontier_add(q,100+i,2+(79-i)/4,1000+i,1));
    for(int ply=2;ply<22;++ply)for(int j=0;j<4;++j)assert(m4_frontier_pop(q)==1000+76-4*(ply-2)+j);
    assert(m4_frontier_pop(q)==-1);assert(m4_frontier_dropped(q)==0);m4_frontier_free(q);
    q=m4_frontier_create(1);assert(q);assert(m4_frontier_add(q,0,0,1,1));
    assert(!m4_frontier_add(q,1,0,2,1));assert(m4_frontier_dropped(q)==1);
    assert(m4_frontier_pop(q)==1);assert(!m4_frontier_add(q,1,0,2,1));
    m4_frontier_free(q);assert(!m4_frontier_create(0));assert(!m4_frontier_create(65537));
    puts("test_m4_frontier: alias closure, history/ply isolation, heap ordering and capacity passed");return 0;
}
