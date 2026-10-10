#include "skiplist.h"
#include <stdlib.h>
#include <string.h>

/* xorshift64: tiny fast PRNG, plenty good for choosing node heights. */
static uint64_t rng_next(SkipList *sl) {
    uint64_t x = sl->rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    sl->rng = x;
    return x;
}

/* Geometric distribution: each extra level is taken with probability 1/2. */
static int random_level(SkipList *sl) {
    uint64_t r = rng_next(sl);
    int lv = 1;
    while (lv < SL_MAX_LEVEL && (r & 1)) { lv++; r >>= 1; }
    return lv;
}

static SLNode *node_new(const Record *r, int level) {
    SLNode *n = malloc(sizeof(SLNode) + (size_t)level * sizeof(SLNode *));
    n->rec = *r;
    n->level = level;
    for (int i = 0; i < level; i++) n->next[i] = NULL;
    return n;
}

SkipList *sl_create(uint64_t seed) {
    SkipList *sl = calloc(1, sizeof(SkipList));
    Record dummy;
    memset(&dummy, 0, sizeof dummy);
    sl->head = node_new(&dummy, SL_MAX_LEVEL);
    for (int i = 0; i < SL_MAX_LEVEL; i++) sl->tail[i] = sl->head;
    sl->level = 1;
    sl->rng = seed ? seed : 88172645463325252ULL;
    return sl;
}

void sl_free(SkipList *sl) {
    if (!sl) return;
    SLNode *n = sl->head;
    while (n) { SLNode *nx = n->next[0]; free(n); n = nx; }
    free(sl);
}

int sl_put(SkipList *sl, const Record *r) {
    SLNode *update[SL_MAX_LEVEL];   /* update[i] = node after which we splice at level i */
    uint64_t key = r->key;

    int fast = (sl->count == 0 || key > sl->max_key);
    if (fast) {
        /* ---- O(1) path: new key is the largest => splice after the tails ---- */
        for (int i = 0; i < SL_MAX_LEVEL; i++) update[i] = sl->tail[i];
        sl->fast_appends++;
    } else {
        /* ---- O(log n) path: classic skip-list descent ---- */
        SLNode *x = sl->head;
        for (int i = sl->level - 1; i >= 0; i--) {
            while (x->next[i] && x->next[i]->rec.key < key) x = x->next[i];
            update[i] = x;
        }
        for (int i = sl->level; i < SL_MAX_LEVEL; i++) update[i] = sl->head;

        SLNode *cand = update[0]->next[0];
        if (cand && cand->rec.key == key) {      /* same key: newest value wins */
            cand->rec = *r;
            sl->slow_inserts++;
            return 0;
        }
        sl->slow_inserts++;
    }

    int lv = random_level(sl);
    if (lv > sl->level) sl->level = lv;
    SLNode *node = node_new(r, lv);
    for (int i = 0; i < lv; i++) {
        node->next[i] = update[i]->next[i];
        update[i]->next[i] = node;
        if (node->next[i] == NULL) sl->tail[i] = node;   /* new last node of level i */
    }
    if (fast) sl->max_key = key;
    sl->count++;
    return 1;
}

const Record *sl_get(const SkipList *sl, uint64_t key) {
    const SLNode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--)
        while (x->next[i] && x->next[i]->rec.key < key) x = x->next[i];
    x = x->next[0];
    return (x && x->rec.key == key) ? &x->rec : NULL;
}

const SLNode *sl_seek(const SkipList *sl, uint64_t key) {
    const SLNode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--)
        while (x->next[i] && x->next[i]->rec.key < key) x = x->next[i];
    return x->next[0];
}

const SLNode *sl_first(const SkipList *sl) { return sl->head->next[0]; }
