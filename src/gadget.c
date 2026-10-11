#include "gadget.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void gadget_free(Gadget *g) {
    game_free(&g->game);
    free(g->node_map); free(g->infoset_map); free(g->entry_infoset);
    free(g->group_chance); free(g->alpha); free(g->offset);
    memset(g, 0, sizeof(*g)); g->game.root = -1;
}

static int distribution(const double *p, int n) {
    double sum = 0;
    for (int i=0;i<n;++i) { if (!isfinite(p[i]) || p[i]<0) return 0; sum+=p[i]; }
    return n>0 && fabs(sum-1.0)<1e-8;
}

static int valid_node(const Game *g, int v) {
    if (v<0 || v>=g->n_nodes) return 0;
    const GameNode *n=&g->nodes[v];
    if (n->type==NODE_TERMINAL) return isfinite(n->value) && n->count==0;
    if (n->first<0 || n->count<=0 || n->first>g->n_edges-n->count) return 0;
    if (n->type==NODE_CHANCE) return distribution(g->prob+n->first,n->count);
    return n->type==NODE_PLAYER && (n->player==0 || n->player==1)
        && n->infoset>=0 && n->infoset<g->n_infosets
        && g->is_player[n->infoset]==n->player && g->is_actions[n->infoset]==n->count;
}

int gadget_build(const Game *s, const GadgetRoot *r, int nr,
                 const GadgetGroup *groups, int ng, Gadget *o) {
    memset(o,0,sizeof(*o)); game_init(&o->game);
    if (!s || !r || !groups || nr<=0 || ng<=0 || ng>nr || s->n_nodes<=0) return 0;
    o->groups=ng; o->source_nodes=s->n_nodes; o->source_infosets=s->n_infosets;
    o->node_map=malloc((size_t)s->n_nodes*sizeof(int));
    o->infoset_map=malloc((size_t)(s->n_infosets+1)*sizeof(int));
    o->entry_infoset=malloc((size_t)ng*sizeof(int)); o->group_chance=malloc((size_t)ng*sizeof(int));
    o->alpha=malloc((size_t)ng*sizeof(double)); o->offset=malloc((size_t)ng*sizeof(double));
    int *owner=malloc((size_t)s->n_nodes*sizeof(int)), *order=malloc((size_t)s->n_nodes*sizeof(int));
    int *counts=calloc((size_t)ng,sizeof(int)); double *mass=calloc((size_t)ng,sizeof(double));
    int *opp_group=malloc((size_t)(s->n_infosets+1)*sizeof(int));
    int *starts=NULL,*next=NULL,*members=NULL,*kids=NULL; double *prob=NULL;
    int ok=0,total=0;
    if (!o->node_map || !o->infoset_map || !o->entry_infoset || !o->group_chance
        || !o->alpha || !o->offset || !owner || !order || !counts || !mass || !opp_group) goto end;
    for(int i=0;i<s->n_nodes;++i) { owner[i]=-1; o->node_map[i]=-1; }
    for(int i=0;i<s->n_infosets;++i) {o->infoset_map[i]=-1;opp_group[i]=-1;}
    double max_y=0,sum_y=0;
    for(int j=0;j<ng;++j) {
        if (!isfinite(groups[j].alternate) || !isfinite(groups[j].gift) || groups[j].gift<0
            || !isfinite(groups[j].opponent_reach) || groups[j].opponent_reach<0) goto end;
        o->offset[j]=groups[j].alternate-groups[j].gift;
        if (!isfinite(o->offset[j])) goto end;
        if(groups[j].opponent_reach>max_y)max_y=groups[j].opponent_reach;
    }
    if(max_y>0)for(int j=0;j<ng;++j)sum_y+=groups[j].opponent_reach/max_y;
    for(int j=0;j<ng;++j)o->alpha[j]=0.5*((max_y>0?(groups[j].opponent_reach/max_y)/sum_y:1.0/ng)+1.0/ng);
    /* Queue expansion verifies a disjoint forest, independent of node order. */
    for(int i=0;i<nr;++i) {
        int v=r[i].node,j=r[i].group;
        if(v<0 || v>=s->n_nodes || j<0 || j>=ng || owner[v]>=0
           || !isfinite(r[i].cf_reach) || r[i].cf_reach<0) goto end;
        owner[v]=j; order[total++]=v; counts[j]++; mass[j]+=r[i].cf_reach;
    }
    int max_actions=ng;
    for(int k=0;k<total;++k) {
        int v=order[k]; if(!valid_node(s,v))goto end;
        const GameNode *n=&s->nodes[v];
        /* An opponent must remember the root J it observed. A downstream
         * infoset shared across different Js violates perfect recall. */
        if(n->type==NODE_PLAYER && n->player==1) {
            if(opp_group[n->infoset]>=0 && opp_group[n->infoset]!=owner[v])goto end;
            opp_group[n->infoset]=owner[v];
        }
        if(n->count>max_actions)max_actions=n->count;
        for(int a=0;a<n->count;++a) {
            int c=s->child[n->first+a];
            if(c<0 || c>=s->n_nodes || owner[c]>=0)goto end;
            owner[c]=owner[v]; order[total++]=c;
        }
    }
    for(int j=0;j<ng;++j) {
        if(!counts[j] || !(mass[j]>0) || !isfinite(mass[j]))goto end;
        if(counts[j]>max_actions)max_actions=counts[j];
    }
    if(max_actions<2)max_actions=2;
    kids=malloc((size_t)max_actions*sizeof(int)); prob=malloc((size_t)max_actions*sizeof(double));
    starts=malloc((size_t)(ng+1)*sizeof(int)); next=malloc((size_t)ng*sizeof(int)); members=malloc((size_t)nr*sizeof(int));
    if(!kids || !prob || !starts || !next || !members)goto end;
    starts[0]=0; for(int j=0;j<ng;++j){starts[j+1]=starts[j]+counts[j];next[j]=starts[j];}
    for(int i=0;i<nr;++i)members[next[r[i].group]++]=i;
    for(int k=0;k<total;++k) {
        int v=order[k]; const GameNode *n=&s->nodes[v]; int is=-1;
        if(n->type==NODE_PLAYER) {
            if(o->infoset_map[n->infoset]<0)o->infoset_map[n->infoset]=game_infoset(&o->game,n->player,n->count);
            is=o->infoset_map[n->infoset];
        }
        double value=n->type==NODE_TERMINAL?n->value-o->offset[owner[v]]:0;
        if(!isfinite(value))goto end;
        o->node_map[v]=game_node(&o->game,n->type,n->player,is,value);
    }
    for(int k=0;k<total;++k) {
        const GameNode *n=&s->nodes[order[k]];
        if(n->type==NODE_TERMINAL)continue;
        for(int a=0;a<n->count;++a)kids[a]=o->node_map[s->child[n->first+a]];
        game_children(&o->game,o->node_map[order[k]],kids,n->type==NODE_CHANCE?s->prob+n->first:NULL,n->count);
    }
    for(int j=0;j<ng;++j) {
        o->group_chance[j]=game_node(&o->game,NODE_CHANCE,0,-1,0);
        for(int k=starts[j];k<starts[j+1];++k) {
            int i=members[k],a=k-starts[j];kids[a]=o->node_map[r[i].node];prob[a]=r[i].cf_reach/mass[j];
        }
        game_children(&o->game,o->group_chance[j],kids,prob,counts[j]);
    }
    o->selector=game_infoset(&o->game,1,ng);
    o->maxmargin_root=game_node(&o->game,NODE_PLAYER,1,o->selector,0);
    game_children(&o->game,o->maxmargin_root,o->group_chance,NULL,ng);
    o->resolve_root=game_node(&o->game,NODE_CHANCE,0,-1,0);
    for(int j=0;j<ng;++j) {
        o->entry_infoset[j]=game_infoset(&o->game,1,2);
        kids[j]=game_node(&o->game,NODE_PLAYER,1,o->entry_infoset[j],0);
        int choices[2]={game_node(&o->game,NODE_TERMINAL,0,-1,0),o->group_chance[j]};
        game_children(&o->game,kids[j],choices,NULL,2);
    }
    game_children(&o->game,o->resolve_root,kids,o->alpha,ng);
    game_finalize(&o->game);o->game.root=o->resolve_root;ok=1;
end:
    free(owner);free(order);free(counts);free(mass);free(opp_group);free(starts);free(next);free(members);free(kids);free(prob);
    if(!ok)gadget_free(o);
    return ok;
}

void gadget_mode(Gadget *g,int mode){g->game.root=mode==GADGET_MAXMARGIN?g->maxmargin_root:g->resolve_root;}
double gadget_leaf_value(const Gadget *g,int group,double raw){return group>=0&&group<g->groups?raw-g->offset[group]:NAN;}
double gadget_gift_step(double gift,double before,double after){return gift+fmax(0,after-before);}
double gadget_new_alternate(double leaf,double previous){return fmin(leaf,previous);}

void gadget_solver_init(GadgetSolver *s){memset(s,0,sizeof(*s));}
void gadget_solver_free(GadgetSolver *s) {
    free(s->strategy);free(s->instant);free(s->root_weight);free(s->root_value);free(s->visited);
    gadget_solver_init(s);
}
static int solver_reserve(GadgetSolver *s,const Gadget *g) {
    if(s->slots>=g->game.n_slots && s->infosets>=g->game.n_infosets && s->groups>=g->groups)return 1;
    GadgetSolver fresh;gadget_solver_init(&fresh);
    fresh.slots=g->game.n_slots;fresh.infosets=g->game.n_infosets;fresh.groups=g->groups;
    fresh.strategy=malloc((size_t)fresh.slots*sizeof(double));fresh.instant=malloc((size_t)fresh.slots*sizeof(double));
    fresh.root_weight=malloc((size_t)fresh.groups*sizeof(double));fresh.root_value=malloc((size_t)fresh.groups*sizeof(double));
    fresh.visited=malloc((size_t)fresh.infosets);
    if(!fresh.strategy||!fresh.instant||!fresh.root_weight||!fresh.root_value||!fresh.visited){gadget_solver_free(&fresh);return 0;}
    gadget_solver_free(s);*s=fresh;return 1;
}
double gadget_root_weights(const Gadget *g,const double *x,double *w) {
    if(!g||!x||!w||g->groups<=0)return NAN;
    const Game *tree=&g->game;int q=tree->is_offset[g->selector];double pmax=0;
    if(!distribution(x+q,g->groups))return NAN;
    for(int j=0;j<g->groups;++j) {
        int e=tree->is_offset[g->entry_infoset[j]];
        if(!distribution(x+e,2))return NAN;
        if(x[e+1]>pmax)pmax=x[e+1];
    }
    for(int j=0;j<g->groups;++j)w[j]=pmax*g->alpha[j]*x[tree->is_offset[g->entry_infoset[j]]+1]+(1-pmax)*x[q+j];
    return pmax;
}

/* Counterfactual traversal of the CONTINUATION only. Root reach belongs to
 * the opponent for P0; P1 must examine each J counterfactually even when its
 * current root policy avoids that J. Unvisited infosets retain their PRM+
 * prediction; replacing it with zero would alter a pruned policy. */
static double continuation(const Game *g,int v,int player,double reach,
                           GadgetSolver *s,int *ok) {
    const GameNode *n=&g->nodes[v];
    if(n->type==NODE_TERMINAL)return player==0?n->value:-n->value;
    if(n->type==NODE_CHANCE) {
        double value=0;
        for(int a=0;a<n->count;++a) {
            double p=g->prob[n->first+a];
            if(p>0)value+=p*continuation(g,g->child[n->first+a],player,reach*p,s,ok);
        }
        return value;
    }
    int I=n->infoset,o=g->is_offset[I];double value=0;
    if(n->player!=player) {
        for(int a=0;a<n->count;++a) {
            double p=s->strategy[o+a];
            if(p>0)value+=p*continuation(g,g->child[n->first+a],player,reach*p,s,ok);
        }
        return value;
    }
    double buf[64],*values=n->count<=64?buf:malloc((size_t)n->count*sizeof(double));
    if(!values){*ok=0;return 0;}
    s->visited[I]=1;
    for(int a=0;a<n->count;++a){values[a]=continuation(g,g->child[n->first+a],player,reach,s,ok);value+=s->strategy[o+a]*values[a];}
    for(int a=0;a<n->count;++a)s->instant[o+a]+=reach*(values[a]-value);
    if(values!=buf)free(values);
    return value;
}
static void update_infoset(Game *g,int I,int t,const GadgetSolver *s) {
    int o=g->is_offset[I];
    for(int a=0;a<g->is_actions[I];++a) {
        g->regret[o+a]=fmax(0,g->regret[o+a]+s->instant[o+a]);
        g->last[o+a]=s->instant[o+a];
        g->strategy_sum[o+a]+=(double)t*s->strategy[o+a];
    }
}
int gadget_iteration(Gadget *g,int t,GadgetSolver *s) {
    if(!g||!s||t<=0||g->groups<=0||g->game.finalized_slots<g->game.n_slots)return 0;
    if(!solver_reserve(s,g))return 0;
    Game *tree=&g->game;int ok=1;
    for(int player=0;player<2;++player) {
        pcfr_current(tree,s->strategy);
        if(player==0 && !isfinite(gadget_root_weights(g,s->strategy,s->root_weight)))return 0;
        memset(s->instant,0,(size_t)tree->n_slots*sizeof(double));memset(s->visited,0,(size_t)tree->n_infosets);
        for(int j=0;j<g->groups;++j) {
            double reach=player==0?s->root_weight[j]:1;
            if(reach>0)s->root_value[j]=continuation(tree,g->group_chance[j],player,reach,s,&ok);
        }
        if(!ok)return 0;
        for(int I=0;I<tree->n_infosets;++I)if(tree->is_player[I]==player && s->visited[I])update_infoset(tree,I,t,s);
        if(player==1) {
            /* In centered P0 utilities, the opponent's enter payoff is
             * -margin. Exit is zero. Each root RM sees this same vector. */
            int q=tree->is_offset[g->selector];double expected=0;
            for(int j=0;j<g->groups;++j)expected+=s->strategy[q+j]*s->root_value[j];
            for(int j=0;j<g->groups;++j)s->instant[q+j]=s->root_value[j]-expected;
            update_infoset(tree,g->selector,t,s);
            for(int j=0;j<g->groups;++j) {
                int I=g->entry_infoset[j],o=tree->is_offset[I];double enter=s->strategy[o+1],v=s->root_value[j];
                s->instant[o]=-enter*v;s->instant[o+1]=(1-enter)*v;
                update_infoset(tree,I,t,s);
            }
        }
    }
    pcfr_current(tree,s->strategy);s->max_enter=gadget_root_weights(g,s->strategy,s->root_weight);s->active_mass=0;
    for(int j=0;j<g->groups;++j)s->active_mass+=s->root_weight[j];
    return isfinite(s->max_enter);
}

int gadget_seed_blueprint(Gadget *g,const Game *s,const double *x) {
    if(!s || !x || s->n_infosets!=g->source_infosets)return 0;
    for(int i=0;i<s->n_infosets;++i)if(g->infoset_map[i]>=0)
        if(!distribution(x+s->is_offset[i],s->is_actions[i]))return 0;
    memset(g->game.regret,0,(size_t)g->game.n_slots*sizeof(double));
    memset(g->game.last,0,(size_t)g->game.n_slots*sizeof(double));
    memset(g->game.strategy_sum,0,(size_t)g->game.n_slots*sizeof(double));
    memset(g->game.strategy,0,(size_t)g->game.n_slots*sizeof(double));
    for(int i=0;i<s->n_infosets;++i)if(g->infoset_map[i]>=0) {
        int dest=g->game.is_offset[g->infoset_map[i]],src=s->is_offset[i];
        /* Seed only the FIRST prediction. Cumulative regret must start at
         * zero; putting x in regret would leave a unit-strength bias after
         * the first update, especially harmful at tiny CF reaches. */
        memcpy(g->game.last+dest,x+src,(size_t)s->is_actions[i]*sizeof(double));
    }
    return 1;
}

int gadget_blueprint_stats(const Game *g,const double *x,double *value,double *cf,double *opp) {
    if(!g)return 0;
    GadgetBoundary root={g->root,1,1};
    return gadget_forest_stats(g,x,&root,1,value,cf,opp);
}
int gadget_forest_stats(const Game *g,const double *x,const GadgetBoundary *roots,int nr,
                        double *value,double *cf,double *opp) {
    if(!g || !x || !value || !cf || !opp || !roots || nr<=0 || nr>g->n_nodes)return 0;
    int *order=malloc((size_t)g->n_nodes*sizeof(int)); unsigned char *seen=calloc((size_t)g->n_nodes,1);
    if(!order || !seen){free(order);free(seen);return 0;}
    memset(value,0,(size_t)g->n_nodes*sizeof(double));memset(cf,0,(size_t)g->n_nodes*sizeof(double));memset(opp,0,(size_t)g->n_nodes*sizeof(double));
    int count=0,ok=0;
    for(int i=0;i<nr;++i) {
        int h=roots[i].node;
        if(h<0||h>=g->n_nodes||seen[h]||!isfinite(roots[i].cf_reach)||roots[i].cf_reach<0
           ||!isfinite(roots[i].opponent_reach)||roots[i].opponent_reach<0)goto done;
        order[count++]=h;seen[h]=1;cf[h]=roots[i].cf_reach;opp[h]=roots[i].opponent_reach;
    }
    for(int i=0;i<count;++i) {
        int v=order[i];if(!valid_node(g,v))goto done;const GameNode *n=&g->nodes[v];
        const double *p=n->type==NODE_CHANCE?g->prob+n->first:n->type==NODE_PLAYER?x+g->is_offset[n->infoset]:NULL;
        if(n->type==NODE_PLAYER&&!distribution(p,n->count))goto done;
        for(int a=0;a<n->count;++a) {
            int c=g->child[n->first+a];if(c<0 || c>=g->n_nodes || seen[c])goto done;
            seen[c]=1;order[count++]=c;
            cf[c]=cf[v]*(n->type==NODE_PLAYER&&n->player==1?1:p[a]);
            opp[c]=opp[v]*(n->type==NODE_PLAYER&&n->player==1?p[a]:1);
            if(!isfinite(cf[c])||!isfinite(opp[c]))goto done;
        }
    }
    for(int i=count-1;i>=0;--i) {
        int v=order[i];const GameNode *n=&g->nodes[v];
        if(n->type==NODE_TERMINAL){value[v]=n->value;continue;}
        const double *p=n->type==NODE_CHANCE?g->prob+n->first:x+g->is_offset[n->infoset];
        for(int a=0;a<n->count;++a)value[v]+=p[a]*value[g->child[n->first+a]];
        if(!isfinite(value[v]))goto done;
    }
    ok=1;
done:free(order);free(seen);return ok;
}

int gadget_group_blueprint(const GadgetRoot *roots,int nr,int group,const double *value,
                           const double *opp,double *alt,double *y) {
    double mass=0,v=0,reach=-1;
    for(int i=0;i<nr;++i)if(roots[i].group==group) {
        int n=roots[i].node;double w=roots[i].cf_reach;
        if(!isfinite(w)||w<0||!isfinite(value[n])||!isfinite(opp[n])||opp[n]<0)return 0;
        if(reach<0)reach=opp[n];
        /* Perfect recall: our opponent's own sequence reach is constant in J. */
        else if(fabs(opp[n]-reach)>1e-10*fmax(1,reach))return 0;
        mass+=w;v+=w*value[n];
    }
    if(!(mass>0)||!isfinite(mass)||!isfinite(v))return 0;
    *alt=v/mass;*y=reach;return 1;
}
