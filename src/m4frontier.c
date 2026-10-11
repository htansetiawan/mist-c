#include "m4frontier.h"
#include <stdlib.h>
typedef struct {uint64_t own;int ply,head,tail,urgent,queued;} Group;
typedef struct {int node,next;} Entry;
struct M4Frontier {
    int capacity,slots,groups,entries,pending,dropped;
    int *table,*heap;Group *group;Entry *entry;
};
static uint64_t hash(uint64_t x,int ply) {
    x^=(uint64_t)ply*UINT64_C(0x9e3779b97f4a7c15);
    x=(x^(x>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    x=(x^(x>>27))*UINT64_C(0x94d049bb133111eb);return x^(x>>31);
}
static int before(const M4Frontier *q,int a,int b) {
    return q->group[a].ply<q->group[b].ply||(q->group[a].ply==q->group[b].ply&&a<b);
}
M4Frontier *m4_frontier_create(int capacity) {
    if(capacity<1||capacity>65536)return NULL;
    M4Frontier *q=calloc(1,sizeof(*q));if(!q)return NULL;
    q->capacity=capacity;q->slots=2;while(q->slots<2*capacity)q->slots*=2;
    q->table=calloc((size_t)q->slots,sizeof(int));q->heap=malloc((size_t)capacity*sizeof(int));
    q->group=calloc((size_t)capacity,sizeof(Group));q->entry=malloc((size_t)capacity*sizeof(Entry));
    if(!q->table||!q->heap||!q->group||!q->entry){m4_frontier_free(q);return NULL;}return q;
}
void m4_frontier_free(M4Frontier *q){if(q){free(q->table);free(q->heap);free(q->group);free(q->entry);free(q);}}
int m4_frontier_dropped(const M4Frontier *q){return q?q->dropped:0;}
int m4_frontier_add(M4Frontier *q,uint64_t own,int ply,int node,int threatened) {
    if(!q)return 0;
    if(ply<0||node<0||q->entries==q->capacity){++q->dropped;return 0;}
    int slot=(int)(hash(own,ply)&(uint64_t)(q->slots-1));
    while(q->table[slot]){Group *g=&q->group[q->table[slot]-1];if(g->own==own&&g->ply==ply)break;slot=(slot+1)&(q->slots-1);}
    if(!q->table[slot]) {
        int i=q->groups++;q->table[slot]=i+1;q->group[i]=(Group){own,ply,-1,-1,0,0};
    }
    int i=q->table[slot]-1,e=q->entries++;Group *g=&q->group[i];
    q->entry[e]=(Entry){node,-1};
    if(g->tail>=0)q->entry[g->tail].next=e;else g->head=e;
    g->tail=e;g->urgent|=threatened!=0;
    if(g->urgent&&!g->queued) {
        int j=q->pending++;q->heap[j]=i;g->queued=1;
        while(j>0){int p=(j-1)/2;if(!before(q,q->heap[j],q->heap[p]))break;
            int tmp=q->heap[j];q->heap[j]=q->heap[p];q->heap[p]=tmp;j=p;}
    }
    return 1;
}
int m4_frontier_pop(M4Frontier *q) {
    if(!q||!q->pending)return -1;
    Group *g=&q->group[q->heap[0]];int e=g->head;g->head=q->entry[e].next;
    if(g->head<0) {
        g->tail=-1;g->queued=0;q->heap[0]=q->heap[--q->pending];
        int j=0;
        while(2*j+1<q->pending){int a=2*j+1;if(a+1<q->pending&&before(q,q->heap[a+1],q->heap[a]))++a;
            if(!before(q,q->heap[a],q->heap[j]))break;
            int tmp=q->heap[j];q->heap[j]=q->heap[a];q->heap[a]=tmp;j=a;}
    }
    return q->entry[e].node;
}
