#define _POSIX_C_SOURCE 199309L
#include "m4search.h"
#include "kluss.h"
#include "eval.h"
#include "m4sample.h"
#include "m4frontier.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    Position pos;
    uint64_t own, opponent, scope;
    int ply, live, valid, group;
    double child_bound;
} BoardNode;
typedef struct {uint64_t key;int infoset,player;uint64_t actions;} InfoEntry;
typedef struct {
    Game *game;
    Gadget *gadget;
    BoardNode *board;int capacity;
    InfoEntry *info;int info_size,info_count;
    int *visits,visit_capacity;
    int us,failed,conservative_leaves,max_nodes;
    double deadline,leaf_scale;
    uint64_t rng;
    M4SF *sf;
    int sf_scored_moves;
    M4Frontier *frontier;
    int scheduled,priority_expanded;
} ChessTree;
struct M4Search {
    KlussBlueprint blueprint;
    BoardNode *board;
    int us,ply,seen,pending;
    Move actual;
    uint64_t own,next_scope;
    char view[65];
    double reference;
    int reference_valid;
    M4SF *sf;
    int child_alternates;
    int tactical_closure;
    int snapshot_ply;
    double leaf_scale;
    double *trace_alternates;
    int trace_groups;
};
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+1e-9*t.tv_nsec;}
static uint64_t random64(uint64_t *s) {
    uint64_t z=(*s+=0x9e3779b97f4a7c15ULL);
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);
}
static double unit(uint64_t *s){return ((random64(s)>>11)+.5)*(1.0/9007199254740992.0);}
static uint64_t their_key(const BoardNode *b){return history_mix(b->opponent,b->scope);}
static int valid_view(const char *v) {
    if(!v||strlen(v)!=64)return 0;
    for(int i=0;i<64;++i)if(!strchr("PNBRQKpnbrqk-?",v[i]))return 0;
    return 1;
}
M4Search *m4_create(int us) {
    if(us!=WHITE&&us!=BLACK)return NULL;
    M4Search *s=calloc(1,sizeof(*s));if(!s)return NULL;
    kluss_blueprint_init(&s->blueprint);s->us=us;s->next_scope=1;s->snapshot_ply=-1;s->leaf_scale=1;return s;
}
void m4_free(M4Search *s){if(s){kluss_blueprint_free(&s->blueprint);free(s->board);free(s->trace_alternates);free(s);}}
void m4_set_evaluation(M4Search *s,M4SF *sf,int child_alternates){if(s){s->sf=sf;s->child_alternates=child_alternates!=0;}}
void m4_set_tactical_closure(M4Search *s,int enabled){if(s)s->tactical_closure=enabled!=0;}
int m4_set_leaf_scale(M4Search *s,double scale) {
    if(!s||!isfinite(scale)||scale<=0||scale>1||(s->blueprint.valid&&scale!=s->leaf_scale))return 0;
    s->leaf_scale=scale;return 1;
}
int m4_observe(M4Search *s,const char view[65]) {
    if(!s||!valid_view(view))return 0;
    if(!s->seen){s->own=history_mix(0,view_hash(view));s->seen=1;}
    else {
        if((s->ply%2==s->us)!=s->pending)return 0;
        if(s->pending)s->own=history_mix(s->own,s->actual);
        s->own=history_mix(s->own,view_hash(view));s->pending=0;++s->ply;
    }
    memcpy(s->view,view,65);return 1;
}
int m4_played(M4Search *s,Move actual) {
    if(!s||!s->seen||s->pending||s->ply%2!=s->us)return 0;
    s->actual=actual;s->pending=1;return 1;
}
uint64_t m4_own_history(const M4Search *s){return s?s->own:0;}
int m4_ply(const M4Search *s){return s?s->ply:-1;}

static int reserve_boards(ChessTree *t,int count) {
    if(t->capacity>=count)return 1;
    int n=t->capacity?t->capacity:256;while(n<count)n*=2;
    BoardNode *p=realloc(t->board,(size_t)n*sizeof(*p));if(!p){t->failed=1;return 0;}
    memset(p+t->capacity,0,(size_t)(n-t->capacity)*sizeof(*p));t->board=p;t->capacity=n;return 1;
}
static void tree_free(ChessTree *t){free(t->board);free(t->info);free(t->visits);m4_frontier_free(t->frontier);memset(t,0,sizeof(*t));}
/* View-proven actions avoid silently giving a player hidden en-passant rights.
 * This is conservative: ambiguous EP can be omitted, never made clairvoyant.
 * The actual rules, rather than this action restriction, determine terminals. */
static void actions(const BoardNode *b,MoveList *l) {
    char view[65];observation(&b->pos,b->pos.side,view);
    view_moves(view,b->pos.side,b->pos.castle_rooks,l);
}
static uint64_t action_key(const MoveList *l) {
    uint64_t k=0;for(int a=0;a<l->count;++a)k=history_mix(k,l->moves[a]);return history_mix(k,(uint64_t)l->count);
}
static int info_reserve(ChessTree *t) {
    if(t->info_size&&t->info_count*2<t->info_size)return 1;
    int n=t->info_size?t->info_size*2:1024;
    InfoEntry *table=calloc((size_t)n,sizeof(*table));if(!table){t->failed=1;return 0;}
    for(int i=0;i<t->info_size;++i)if(t->info[i].infoset) {
        int j=(int)(history_mix(t->info[i].key,t->info[i].player)&(uint64_t)(n-1));
        while(table[j].infoset)j=(j+1)&(n-1);
        table[j]=t->info[i];
    }
    free(t->info);t->info=table;t->info_size=n;return 1;
}
static int infoset(ChessTree *t,const BoardNode *b,const MoveList *l,int existing) {
    if(!info_reserve(t))return -1;
    int player=b->pos.side==t->us?0:1;
    uint64_t key=player?their_key(b):b->own,ak=action_key(l);
    int slot=(int)(history_mix(key,player)&(uint64_t)(t->info_size-1));
    while(t->info[slot].infoset) {
        InfoEntry e=t->info[slot];
        if(e.key==key&&e.player==player) {
            if(e.actions!=ak||(existing>=0&&e.infoset-1!=existing)){t->failed=1;return -1;}
            return e.infoset-1;
        }
        slot=(slot+1)&(t->info_size-1);
    }
    int I=existing>=0?existing:game_infoset(t->game,player,l->count);
    t->info[slot]=(InfoEntry){key,I+1,player,ak};++t->info_count;return I;
}
static int reindex(ChessTree *t) {
    free(t->info);t->info=NULL;t->info_size=t->info_count=0;
    for(int h=0;h<t->game->n_nodes;++h)if(h<t->capacity&&t->board[h].valid&&t->game->nodes[h].type==NODE_PLAYER) {
        MoveList l;actions(&t->board[h],&l);
        if(l.count!=t->game->nodes[h].count||infoset(t,&t->board[h],&l,t->game->nodes[h].infoset)<0)return 0;
    }
    return 1;
}
/* Schedule own-turn aliases as a group; never use hidden state as the
 * actual policy key. The same perfect-recall history drives infoset(). */
static void schedule(ChessTree *t,int h) {
    BoardNode *b=&t->board[h];
    if(!t->frontier||!b->valid||!b->live||b->pos.side!=t->us||t->game->nodes[h].type!=NODE_TERMINAL)return;
    int king=king_square(&b->pos,t->us);
    int danger=king>=0&&is_attacked(&b->pos,king,t->us^1);
    t->scheduled+=m4_frontier_add(t->frontier,b->own,b->ply,h,danger);
}
static int expand(ChessTree *t,int h) {
    if(h>=t->capacity||!t->board[h].valid||!t->board[h].live||t->game->nodes[h].type!=NODE_TERMINAL)return 0;
    if(now()>=t->deadline)return 0;
    BoardNode parent=t->board[h];MoveList l;actions(&parent,&l);
    if(!l.count){t->board[h].live=0;++t->conservative_leaves;return 0;}
    if(t->game->n_nodes>t->max_nodes-l.count)return 0;
    int before=t->game->n_infosets,I=infoset(t,&parent,&l,-1);if(I<0)return 0;
    int kids[256],best=0;double best_value=-INFINITY;
    if(!reserve_boards(t,t->game->n_nodes+l.count))return 0;
    int cp[256]={0},found[256]={0};
    /* Do not start a request with only the tail of a decision left. A normal
     * end-of-search should use cheap leaves, not kill a healthy engine with
     * a 1-3ms deadline and lose it for every subsequent turn. */
    double request_start=now();
    if(m4sf_ready(t->sf)&&t->deadline-request_start>=.026)
        t->sf_scored_moves+=m4sf_children(t->sf,&parent.pos,&l,request_start+.025,cp,found);
    for(int a=0;a<l.count;++a) {
        BoardNode child=parent;Undo undo;int mover=parent.pos.side;
        make_move(&child.pos,l.moves[a],&undo);child.ply++;
        char own[65],opponent[65];observation(&child.pos,t->us,own);observation(&child.pos,t->us^1,opponent);
        child.own=history_mix(mover==t->us?history_mix(parent.own,l.moves[a]):parent.own,view_hash(own));
        child.opponent=history_mix(mover!=t->us?history_mix(parent.opponent,l.moves[a]):parent.opponent,view_hash(opponent));
        double value;
        if(king_square(&child.pos,mover^1)<0){child.live=0;value=mover==t->us?1:-1;}
        else if(mover_left_king_capturable(&child.pos)){child.live=0;value=mover==t->us?-1:1;}
        else if(child.pos.halfmove>=100){child.live=0;value=0;}
        else {
            MoveList legal;generate_moves(&child.pos,&legal);child.live=legal.count>0;
            value=child.live?t->leaf_scale*(found[a]?tanh(cp[a]/400.0)*(mover==t->us?1:-1):evaluate(&child.pos,t->us)):(mover==t->us?1:-1);
        }
        double score=mover==t->us?value:-value;if(score>best_value){best_value=score;best=a;}
        if(t->gadget)value=gadget_leaf_value(t->gadget,child.group,value);
        kids[a]=game_node(t->game,NODE_TERMINAL,0,-1,value);t->board[kids[a]]=child;
        schedule(t,kids[a]);
    }
    game_expand_leaf(t->game,h,parent.pos.side==t->us?0:1,I,kids,l.count);
    t->board[h].child_bound=best_value*(parent.pos.side==t->us?1:-1);
    game_grow(t->game);
    if(I==before)t->game->last[t->game->is_offset[I]+best]=1; /* fresh best-child prediction */
    return 1;
}

typedef struct {int index;double priority;} Sample;
static void heap_down(Sample *h,int n,int k) {
    for(;;){int a=2*k+1;if(a>=n)return;if(a+1<n&&h[a+1].priority>h[a].priority)++a;
        if(h[k].priority>=h[a].priority)return;
        Sample tmp=h[k];h[k]=h[a];h[a]=tmp;k=a;}
}
static int sample_fresh(const Belief *b,int limit,uint64_t *rng,const uint64_t *excluded,int ne,Sample *heap,double deadline) {
    int count=0;
    for(size_t i=0;i<b->count;++i) {
        if((i&1023)==0&&now()>=deadline)return -1; /* never sample only a prefix of P */
        uint64_t k=position_key(&b->items[i]);
        int lo=0,hi=ne;while(lo<hi){int m=lo+(hi-lo)/2;if(excluded[m]<k)lo=m+1;else hi=m;}
        if(lo<ne&&excluded[lo]==k)continue;
        double lw=b->logw?b->logw[i]:0;if(!isfinite(lw))continue;
        double p=log(-log(unit(rng)))-lw;Sample s={(int)i,p};
        if(count<limit) {
            int j=count++;heap[j]=s;
            while(j>0){int parent=(j-1)/2;if(heap[parent].priority>=heap[j].priority)break;Sample v=heap[j];heap[j]=heap[parent];heap[parent]=v;j=parent;}
        } else if(p<heap[0].priority){heap[0]=s;heap_down(heap,count,0);}
    }
    return count;
}
static int key_compare(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);}
static int index_compare(const void *a,const void *b){const Sample *x=a,*y=b;return (x->index>y->index)-(x->index<y->index);}

/* Retain the complete same-ply frontier, not just nodes consistent with us.
 * kluss_prepare then retains the opponent-connected off-I branches. */
static int carry(M4Search *s,ChessTree *t,KlussPrepared *p,M4Stats *stats) {
    KlussBlueprint *b=&s->blueprint;if(!b->valid){stats->fallback=1;return 0;}
    int n=b->tree.n_nodes;
    KlussFrontier *f=malloc((size_t)n*sizeof(*f));
    double *v=malloc((size_t)n*sizeof(double)),*cf=malloc((size_t)n*sizeof(double)),*op=malloc((size_t)n*sizeof(double));
    if(!f||!v||!cf||!op){free(f);free(v);free(cf);free(op);stats->fallback=3;return 0;}
    int count=0;
    for(int h=0;h<n;++h)if(s->board[h].valid&&s->board[h].live&&s->board[h].ply==s->ply&&s->board[h].pos.side==s->us) {
        f[count++]=(KlussFrontier){h,b->ours[h],b->theirs[h]};stats->current_found+=b->ours[h]==s->own;
    }
    stats->frontier=count;
    int ok=count&&kluss_blueprint_stats(b,v,cf,op);
    if(ok)for(int i=0;i<count;++i)if(f[i].ours==s->own&&cf[f[i].node]>0)++stats->current_positive;
    if(!stats->current_found)stats->fallback=1;
    else if(!stats->current_positive)stats->fallback=2;
    else if(!ok||!kluss_prepare(b,f,count,s->own,p))stats->fallback=3;
    else if(p->gadget.game.n_nodes>t->max_nodes){stats->fallback=4;kluss_prepared_free(p);}
    else {
        *t->game=p->gadget.game;game_init(&p->gadget.game); /* transfer ownership */
        if(reserve_boards(t,t->game->n_nodes)) {
            for(int h=0;h<n;++h)if(p->gadget.node_map[h]>=0)t->board[p->gadget.node_map[h]]=s->board[h];
            int *queue=malloc((size_t)t->game->n_nodes*sizeof(int));
            if(!queue)t->failed=1;
            else {
                int used=0;
                for(int r=0;r<p->root_count;++r) {
                    int h=p->gadget.node_map[p->roots[r].node];t->board[h].group=p->roots[r].group;queue[used++]=h;
                }
                for(int k=0;k<used;++k) {
                    int h=queue[k],j=t->board[h].group;GameNode *node=&t->game->nodes[h];
                    if(node->type==NODE_TERMINAL)node->value+=p->gadget.offset[j];
                    for(int a=0;a<node->count;++a){int c=t->game->child[node->first+a];t->board[c].group=j;queue[used++]=c;}
                }
                free(queue);
            }
            stats->carried=p->current_count;stats->off_current=p->root_count-p->current_count;
        }
    }
    free(f);free(v);free(cf);free(op);return stats->carried>0&&!t->failed;
}

static int pick_weight(const double *w,int count,uint64_t *rng) {
    double mass=0;for(int j=0;j<count;++j)mass+=w[j];
    if(!(mass>0))return (int)(random64(rng)%(uint64_t)count);
    double r=unit(rng)*mass;for(int j=0;j<count;++j){r-=w[j];if(r<=0)return j;}return count-1;
}
static int expansion(ChessTree *t,const double *x,int slots,const double *w,int explorer) {
    Gadget *g=t->gadget;int group=explorer?(int)(random64(&t->rng)%(uint64_t)g->groups):pick_weight(w,g->groups,&t->rng);
    int h=g->group_chance[group];
    if(t->visit_capacity<slots) {
        int *v=realloc(t->visits,(size_t)slots*sizeof(int));if(!v){t->failed=1;return 0;}
        memset(v+t->visit_capacity,0,(size_t)(slots-t->visit_capacity)*sizeof(int));t->visits=v;t->visit_capacity=slots;
    }
    for(int depth=0;depth<64;++depth) {
        const GameNode *n=&t->game->nodes[h];
        if(n->type==NODE_TERMINAL)return expand(t,h);
        int a=0;
        if(n->type==NODE_CHANCE)a=pick_weight(t->game->prob+n->first,n->count,&t->rng);
        else {
            int o=t->game->is_offset[n->infoset];if(o+n->count>slots)return 0;
            if(n->player!=explorer)a=pick_weight(x+o,n->count,&t->rng);
            else if(random64(&t->rng)&1) {
                int support[256],count=0;
                for(int k=0;k<n->count;++k)if(x[o+k]>1e-6)support[count++]=k;
                a=count?support[random64(&t->rng)%(uint64_t)count]:0;
            } else {
                double total=0,best=-INFINITY;
                for(int k=0;k<n->count;++k)total+=t->visits[o+k];
                for(int k=0;k<n->count;++k) {
                    double score=x[o+k]+sqrt(total+1)/(1+t->visits[o+k]);
                    if(score>best){best=score;a=k;}
                }
            }
            ++t->visits[o+a];
        }
        h=t->game->child[n->first+a];
    }
    return 0;
}

int m4_decide(M4Search *s,const Belief *b,const M4Params *p,SearchResult *out,M4Stats *stats) {
    if(!out||!stats)return 0;
    memset(out,0,sizeof(*out));memset(stats,0,sizeof(*stats));double start=now();
    if(!s||!b||!p||!s->seen||s->pending||s->ply%2!=s->us||b->us!=s->us||!b->count||p->samples<=0
       ||p->max_nodes<256||p->seconds<=0||p->iterations_per_round<=0||p->expansions_per_round<0)return 0;
    stats->ply=s->ply;Game raw;game_init(&raw);Gadget gadget;memset(&gadget,0,sizeof(gadget));game_init(&gadget.game);
    KlussPrepared prepared;kluss_prepared_init(&prepared);GadgetSolver solver;gadget_solver_init(&solver);
    ChessTree t={0};t.game=&raw;t.us=s->us;t.max_nodes=p->max_nodes;t.rng=p->seed;t.deadline=start+p->seconds*.75;t.sf=s->sf;t.leaf_scale=s->leaf_scale;
    M4SFStats sf_before;m4sf_stats(s->sf,&sf_before);
    GadgetRoot *roots=NULL;GadgetGroup *groups=NULL;Sample *sample=NULL;uint64_t *excluded=NULL;
    double *x=NULL;int nr=0,ng=0,ok=0,root_I=-1;MoveList legal;legal.count=0;
    /* Establish the actual root action ordering before expensive preparation. */
    BoardNode initial={0};initial.pos=b->items[0];actions(&initial,&legal);
    if(!legal.count)goto done;
    out->move=legal.moves[0];out->our_moves=legal.count;out->probability=1;
    int have=carry(s,&t,&prepared,stats);if(t.failed)goto done;
    int carried=have?prepared.root_count:0;
    int old_aux=have?3*prepared.group_count+2:0;
    roots=malloc((size_t)(carried+p->samples)*sizeof(*roots));groups=malloc((size_t)(prepared.group_count+p->samples)*sizeof(*groups));
    excluded=malloc((size_t)(carried+p->samples+1)*sizeof(uint64_t));sample=malloc((size_t)p->samples*sizeof(*sample));
    if(!roots||!groups||!excluded||!sample)goto done;
    int ne=0;
    if(have) {
        ng=prepared.group_count;memcpy(groups,prepared.groups,(size_t)ng*sizeof(*groups));
        for(int r=0;r<carried;++r) {
            GadgetRoot old=prepared.roots[r];int h=prepared.gadget.node_map[old.node];roots[nr++]=(GadgetRoot){h,old.group,old.cf_reach};
            if(t.board[h].own==s->own)excluded[ne++]=position_key(&t.board[h].pos);
        }
        if(!reindex(&t))goto done;
    }
    qsort(excluded,(size_t)ne,sizeof(*excluded),key_compare);
    int witnesses=0;
    if(p->tactical_witnesses) {
        unsigned char known[256]={0};double scan_deadline=start+p->seconds*.2;
        for(int r=0;r<nr&&now()<scan_deadline;++r) {
            int h=roots[r].node;if(t.board[h].own!=s->own||roots[r].cf_reach<=0)continue;
            for(int a=0;a<legal.count;++a)if(!known[a]) {
                Position pos=t.board[h].pos;Undo u;make_move(&pos,legal.moves[a],&u);
                known[a]=king_square(&pos,s->us^1)>=0&&mover_left_king_capturable(&pos);
            }
        }
        int idx[256];M4WitnessStats ws;
        int count=m4_witness_sample(b,&legal,known,p->samples<256?p->samples:256,scan_deadline,idx,&ws);
        stats->witness_scanned=(int)ws.scanned;stats->witness_complete=ws.complete;stats->witness_candidate_moves=ws.represented_moves;
        int old_ne=ne;
        for(int i=0;i<count;++i) {
            uint64_t key=position_key(&b->items[idx[i]]);
            if(bsearch(&key,excluded,(size_t)old_ne,sizeof(*excluded),key_compare))continue;
            sample[witnesses++]=(Sample){idx[i],0};excluded[ne++]=key;
        }
        stats->witness_added=witnesses;
        qsort(excluded,(size_t)ne,sizeof(*excluded),key_compare);
    }
    int need=p->samples-stats->carried;
    int random=need>witnesses?sample_fresh(b,need-witnesses,&t.rng,excluded,ne,sample+witnesses,t.deadline):0;
    if(random<0)goto done;
    int fresh=witnesses+random;
    qsort(sample,(size_t)fresh,sizeof(*sample),index_compare);
    for(int k=0;k<fresh;++k) {
        const Position *pos=&b->items[sample[k].index];char view[65];observation(pos,s->us,view);
        if(pos->side!=s->us||memcmp(view,s->view,64))continue;
        if(raw.n_nodes+1+3*(ng+1)+2-old_aux>p->max_nodes||now()>=t.deadline)break;
        double value=s->leaf_scale*evaluate(pos,s->us);int h=game_node(&raw,NODE_TERMINAL,0,-1,value);
        if(!reserve_boards(&t,raw.n_nodes))goto done;
        BoardNode v={0};v.pos=*pos;v.own=s->own;v.ply=s->ply;v.live=v.valid=1;v.group=ng;
        observation(pos,s->us^1,view);v.opponent=b->hist?b->hist[sample[k].index]:history_mix(0,view_hash(view));
        v.scope=s->next_scope++;t.board[h]=v;
        roots[nr++]=(GadgetRoot){h,ng,1};groups[ng++]=(GadgetGroup){gadget_new_alternate(value,s->reference_valid?s->reference:value),0,1};
        ++stats->fresh;
    }
    if(!nr)goto done;
    t.max_nodes=p->max_nodes-(3*ng+2)+old_aux;
    /* Every current-I root needs the same action set, including carried leaves.
     * Rebuilding a source tree does not change its raw terminal scores. */
    for(int r=0;r<nr;++r)if(t.board[roots[r].node].own==s->own) {
        int h=roots[r].node;expand(&t,h);
        if(raw.nodes[h].type!=NODE_PLAYER)goto done;
        if(s->child_alternates&&r>=carried){
            double value=t.board[h].child_bound;
            groups[roots[r].group].alternate=gadget_new_alternate(value,s->reference_valid?s->reference:value);
            ++stats->child_alternates;
        }
    }
    if(t.failed)goto done;
    game_grow(&raw);x=malloc((size_t)(raw.n_slots+1)*sizeof(double));if(!x)goto done;pcfr_current(&raw,x);
    if(!gadget_build(&raw,roots,nr,groups,ng,&gadget)||!gadget_seed_blueprint(&gadget,&raw,x))goto done;
    if(gadget.game.n_nodes>p->max_nodes)goto done;
    ChessTree next=t;next.board=NULL;next.capacity=0;next.info=NULL;next.info_size=next.info_count=0;next.visits=NULL;next.visit_capacity=0;next.frontier=NULL;
    next.game=&gadget.game;next.gadget=&gadget;next.max_nodes=p->max_nodes;
    if(!reserve_boards(&next,gadget.game.n_nodes)){tree_free(&next);goto done;}
    for(int h=0;h<raw.n_nodes;++h)if(gadget.node_map[h]>=0&&h<t.capacity)next.board[gadget.node_map[h]]=t.board[h];
    tree_free(&t);t=next;game_free(&raw);free(x);x=NULL;
    if(!reindex(&t))goto done;
    if(s->tactical_closure) {
        t.frontier=m4_frontier_create(p->max_nodes<65536?p->max_nodes:65536);
        if(!t.frontier)goto done;
        for(int h=0;h<t.game->n_nodes;++h) {
            if((h&255)==0&&now()>=t.deadline)goto done;
            if(h<t.capacity)schedule(&t,h);
        }
    }
    for(int r=0;r<nr;++r) {
        int h=gadget.node_map[roots[r].node];
        if(t.board[h].own==s->own&&gadget.game.nodes[h].type==NODE_PLAYER) {
            int I=gadget.game.nodes[h].infoset;if(root_I<0)root_I=I;else if(root_I!=I)goto done;
        }
    }
    if(root_I<0)goto done;
    stats->groups=ng;stats->setup_seconds=now()-start;
    int iteration=0,explorer=0,last_mode=-1,stability_started=0;
    unsigned char stable[256];memset(stable,1,sizeof(stable));
    while(now()<t.deadline&&(p->max_iterations<=0||iteration<p->max_iterations)) {
        for(int k=0;k<p->iterations_per_round&&now()<t.deadline&&(p->max_iterations<=0||iteration<p->max_iterations);++k) {
            if(!gadget_iteration(&gadget,++iteration,&solver))goto done;
            int mode=solver.max_enter>1e-9;if(last_mode>=0&&mode!=last_mode)++stats->mode_changes;last_mode=mode;
            if(now()-start>=p->seconds*.5) {
                stability_started=1;int ro=gadget.game.is_offset[root_I];
                for(int a=0;a<legal.count;++a)if(solver.strategy[ro+a]<=1e-6)stable[a]=0;
            }
        }
        if(!iteration)break;
        for(int k=0;k<p->expansions_per_round&&now()<t.deadline;++k) {
            int expanded=0;
            if(t.frontier&&(k&1)==0) {
                int h;
                while(now()<t.deadline&&(h=m4_frontier_pop(t.frontier))>=0) {
                    if(h>=t.capacity||!t.board[h].live||t.game->nodes[h].type!=NODE_TERMINAL)continue;
                    expanded=expand(&t,h);t.priority_expanded+=expanded;break;
                }
            }
            if(!expanded){expanded=expansion(&t,solver.strategy,solver.slots,solver.root_weight,explorer);explorer^=1;}
            stats->expansions+=expanded;
            if(t.failed)goto done;
        }
    }
    x=malloc((size_t)gadget.game.n_slots*sizeof(double));if(!x)goto done;pcfr_current(&gadget.game,x);
    int o=gadget.game.is_offset[root_I],best=0;
    if(gadget.game.is_actions[root_I]!=legal.count)goto done;
    for(int a=1;a<legal.count;++a)if(x[o+a]>x[o+best])best=a;
    double play[256]={0};play[best]=1;stats->purified_support=1;
    if(stability_started&&solver.max_enter<=1e-9) {
        for(int k=0;k<2;++k) {
            int amax=-1;
            for(int a=0;a<legal.count;++a)if(a!=best&&stable[a]&&play[a]==0&&x[o+a]>1e-6
                &&(amax<0||x[o+a]>x[o+amax]))amax=a;
            if(amax<0)break;
            play[amax]=x[o+amax];play[best]-=play[amax];++stats->purified_support;
        }
    }
    int chosen=pick_weight(play,legal.count,&t.rng);
    out->move=legal.moves[chosen];out->probability=play[chosen];out->samples=stats->carried+stats->fresh;
    out->iterations=iteration;out->nodes=gadget.game.n_nodes;out->opponent_infosets=gadget.game.n_infosets;
    stats->solve_seconds=now()-start-stats->setup_seconds;stats->conservative_leaves=t.conservative_leaves;
    /* Capture before deadline; an older still-valid tree is preferable to
     * corrupting it if this atomic snapshot fails. */
    if(now()<start+p->seconds*.9) {
        int n=gadget.game.n_nodes;uint64_t *oh=calloc((size_t)n,sizeof(*oh)),*th=calloc((size_t)n,sizeof(*th));
        if(oh&&th) {
            for(int h=0;h<n;++h)if(h<t.capacity&&t.board[h].valid){oh[h]=t.board[h].own;th[h]=their_key(&t.board[h]);}
            KlussBlueprint snapshot;kluss_blueprint_init(&snapshot);
            if(kluss_blueprint_capture_gadget(&snapshot,&gadget,x,oh,th)) {
                double *v=malloc((size_t)n*sizeof(double)),*cf=malloc((size_t)n*sizeof(double)),*op=malloc((size_t)n*sizeof(double));
                double total=0,value=0;
                int measured=v&&cf&&op&&kluss_blueprint_stats(&snapshot,v,cf,op);
                if(measured)for(int r=0;r<nr;++r) {
                    int h=gadget.node_map[roots[r].node];if(t.board[h].own==s->own){double w=cf[h]*op[h];total+=w;value+=w*v[h];}
                }
                if(total>0){s->reference=value/total;s->reference_valid=1;out->value=s->reference;}
                else s->reference_valid=0;
                const char *dump=getenv("MIST_M4_DUMP_PLY");
                if(dump&&atoi(dump)==s->ply&&measured)for(int r=0;r<nr;++r) {
                    int h=gadget.node_map[roots[r].node];
                    if(t.board[h].own!=s->own)continue;
                    char board[65];pos_to_board_string(&t.board[h].pos,board);
                    Position child=t.board[h].pos;Undo undo;make_move(&child,out->move,&undo);
                    int fatal=king_square(&child,s->us^1)>=0&&mover_left_king_capturable(&child);
                    fprintf(stderr,"m4-root: board=%s ep=%d castle=%016llx halfmove=%d group=%d reach=%.17g value=%.9g chosen_fatal=%d\n",
                        board,t.board[h].pos.ep,(unsigned long long)t.board[h].pos.castle_rooks,t.board[h].pos.halfmove,
                        t.board[h].group,cf[h]*op[h],v[h],fatal);
                }
                if(dump&&atoi(dump)==s->ply&&measured&&total>0)for(int a=0;a<legal.count;++a) {
                    double fatal_mass=0,action_value=0;
                    for(int r=0;r<nr;++r) {
                        int h=gadget.node_map[roots[r].node];if(t.board[h].own!=s->own)continue;
                        double w=cf[h]*op[h];Position child=t.board[h].pos;Undo undo;make_move(&child,legal.moves[a],&undo);
                        if(king_square(&child,s->us^1)>=0&&mover_left_king_capturable(&child))fatal_mass+=w;
                        const GameNode *node=&snapshot.tree.nodes[h];
                        action_value+=w*v[snapshot.tree.child[node->first+a]];
                    }
                    char uci[6];move_to_uci(legal.moves[a],uci);
                    fprintf(stderr,"m4-action: move=%s policy=%.9g played=%.9g fatal=%.9g raw=%.9g\n",
                        uci,x[o+a],play[a],fatal_mass/total,action_value/total);
                }
                stats->current_mass=total;stats->active_mass=snapshot.active_mass;
                free(s->trace_alternates);s->trace_alternates=NULL;s->trace_groups=0;
                if(getenv("MIST_M4_TRACE_PATH")) {
                    s->trace_alternates=malloc((size_t)gadget.groups*sizeof(double));
                    if(s->trace_alternates){memcpy(s->trace_alternates,gadget.offset,(size_t)gadget.groups*sizeof(double));s->trace_groups=gadget.groups;}
                }
                kluss_blueprint_free(&s->blueprint);s->blueprint=snapshot;
                free(s->board);s->board=t.board;t.board=NULL;t.capacity=0;stats->snapshot=1;
                s->snapshot_ply=s->ply;
                free(v);free(cf);free(op);
            }
            free(oh);free(th);
        }
    }
    stats->max_enter=solver.max_enter;stats->snapshot_seconds=now()-start-stats->setup_seconds-stats->solve_seconds;
    ok=1;
done:
    stats->sf_scored_moves=t.sf_scored_moves;
    stats->closure_scheduled=t.scheduled;stats->closure_expanded=t.priority_expanded;
    stats->closure_dropped=m4_frontier_dropped(t.frontier);
    M4SFStats sf_after;m4sf_stats(s->sf,&sf_after);
    stats->sf_calls=sf_after.calls-sf_before.calls;stats->sf_failed=sf_after.failed-sf_before.failed;stats->sf_seconds=sf_after.seconds-sf_before.seconds;
    free(roots);free(groups);free(sample);free(excluded);free(x);tree_free(&t);
    game_free(&raw);gadget_free(&gadget);gadget_solver_free(&solver);kluss_prepared_free(&prepared);
    stats->total_seconds=now()-start;stats->over_budget=stats->total_seconds>p->seconds;
    return ok;
}

static const char *trace_kind(const M4Search *s,int h) {
    return s->blueprint.tree.nodes[h].type==NODE_PLAYER?"decision":s->board[h].live?"frontier":"terminal";
}
static void trace_node(const M4Search *s,FILE *out,int root,int h,const char *path,
                       double probability,const double *v,const double *cf,const double *op) {
    const KlussBlueprint *b=&s->blueprint;const GameNode *n=&b->tree.nodes[h];
    const BoardNode *board=&s->board[h];char text[65];pos_to_board_string(&board->pos,text);
    int losses=0,terminals=0,frontiers=0;
    for(int a=0;a<n->count;++a) {
        int c=b->tree.child[n->first+a];const GameNode *child=&b->tree.nodes[c];
        if(child->type==NODE_TERMINAL) {
            if(s->board[c].live)++frontiers;
            else {++terminals;if(v[c]<=-1.0+1e-10)++losses;}
        }
    }
    fprintf(out,"{\"kind\":\"node\",\"root\":%d,\"node\":%d,\"path\":\"%s\",\"board\":\"%s\",\"side\":%d,\"ep\":%d,\"castle\":\"%016llx\",\"halfmove\":%d,\"node_kind\":\"%s\",\"infoset\":%d,\"own_history\":\"%016llx\",\"raw\":%.17g,\"cf\":%.17g,\"opp\":%.17g,\"probability\":%.17g,\"actions\":%d,\"terminal_children\":%d,\"loss_children\":%d,\"frontier_children\":%d}\n",
        root,h,path,text,board->pos.side,board->pos.ep,(unsigned long long)board->pos.castle_rooks,board->pos.halfmove,
        trace_kind(s,h),n->type==NODE_PLAYER?n->infoset:-1,(unsigned long long)board->own,v[h],cf[h],op[h],probability,n->count,terminals,losses,frontiers);
}
int m4_trace_last(const M4Search *s,Move chosen,FILE *out,int max_records) {
    if(!s||!out||max_records<1||!s->blueprint.valid||s->snapshot_ply!=s->ply)return 0;
    const KlussBlueprint *b=&s->blueprint;const Game *g=&b->tree;int n=g->n_nodes;
    double *v=malloc((size_t)n*sizeof(double)),*cf=malloc((size_t)n*sizeof(double)),*op=malloc((size_t)n*sizeof(double));
    if(!v||!cf||!op||!kluss_blueprint_stats(b,v,cf,op)){free(v);free(cf);free(op);return 0;}
    int records=0,complete=1,roots=0;char own[6];move_to_uci(chosen,own);
    /* Blueprint boundaries are synthetic group-chance nodes, not boards.
     * Locate current-ply board roots in their copied continuation forest. */
    for(int h=0;h<n;++h) {
        if(!s->board[h].valid||s->board[h].ply!=s->ply||s->board[h].own!=s->own||s->board[h].pos.side!=s->us)continue;
        const GameNode *node=&g->nodes[h];MoveList legal;actions(&s->board[h],&legal);
        if(node->type!=NODE_PLAYER||node->count!=legal.count){complete=0;break;}
        int a=0;while(a<legal.count&&legal.moves[a]!=chosen)++a;
        if(a==legal.count){complete=0;break;}
        if(records>=max_records){complete=0;break;}
        ++roots;++records;int group=s->board[h].group;
        fprintf(out,"{\"kind\":\"root\",\"root\":%d,\"group\":%d,\"alternate\":",h,group);
        if(group>=0&&group<s->trace_groups)fprintf(out,"%.17g",s->trace_alternates[group]);else fputs("null",out);
        fprintf(out,",\"raw\":%.17g,\"cf\":%.17g,\"opp\":%.17g,\"own_probability\":%.17g}\n",v[h],cf[h],op[h],b->strategy[g->is_offset[node->infoset]+a]);
        if(records>=max_records){complete=0;break;}
        ++records;trace_node(s,out,h,h,"",1,v,cf,op);
        int c=g->child[node->first+a];const GameNode *reply=&g->nodes[c];
        if(records>=max_records){complete=0;break;}
        ++records;trace_node(s,out,h,c,own,b->strategy[g->is_offset[node->infoset]+a],v,cf,op);
        if(reply->type!=NODE_PLAYER)continue;
        MoveList replies;actions(&s->board[c],&replies);
        if(reply->count!=replies.count||s->board[c].pos.side==s->us){complete=0;break;}
        for(int j=0;j<replies.count;++j) {
            if(records>=max_records){complete=0;break;}
            char uci[6],path[13];move_to_uci(replies.moves[j],uci);snprintf(path,sizeof(path),"%s/%s",own,uci);
            ++records;trace_node(s,out,h,g->child[reply->first+j],path,b->strategy[g->is_offset[reply->infoset]+j],v,cf,op);
        }
        if(!complete)break;
    }
    if(!roots)complete=0;
    fprintf(out,"{\"kind\":\"end\",\"complete\":%s,\"records\":%d,\"roots\":%d,\"ply\":%d}\n",complete?"true":"false",records,roots,s->ply);
    int ok=complete&&!ferror(out);free(v);free(cf);free(op);return ok;
}
