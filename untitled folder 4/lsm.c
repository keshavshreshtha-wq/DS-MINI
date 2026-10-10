#include "lsm.h"
#include "util.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <direct.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_LEVELS 8

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void event(const LSM *db, const char *color, const char *tag, const char *fmt, ...) {
    if (!db->cfg.verbose) return;
    va_list ap;
    printf("  %s%s[%-7s]%s ", C_BOLD, color, tag, C_RESET);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void table_path(const LSM *db, uint64_t id, char *buf, size_t n) {
    snprintf(buf, n, "%s/sst_%06llu.sst", db->dir, (unsigned long long)id);
}

static int table_cmp(const void *a, const void *b) {       /* level asc, id desc */
    const SSTable *x = *(SSTable *const *)a, *y = *(SSTable *const *)b;
    if (x->level != y->level) return x->level < y->level ? -1 : 1;
    return x->id > y->id ? -1 : (x->id < y->id ? 1 : 0);
}

static void add_table(LSM *db, SSTable *t) {
    if (db->ntables == db->cap) {
        db->cap = db->cap ? db->cap * 2 : 16;
        db->tables = realloc(db->tables, db->cap * sizeof(SSTable *));
    }
    db->tables[db->ntables++] = t;
    qsort(db->tables, db->ntables, sizeof(SSTable *), table_cmp);
}

static size_t count_level(const LSM *db, int level) {
    size_t c = 0;
    for (size_t i = 0; i < db->ntables; i++) if (db->tables[i]->level == level) c++;
    return c;
}

/* ------------------------------------------------------------------ */
/* manifest: tiny text file listing live SSTables (so we can reopen)   */
/* ------------------------------------------------------------------ */

static void save_manifest(const LSM *db) {
    char tmp[300], fin[300];
    snprintf(tmp, sizeof tmp, "%s/MANIFEST.tmp", db->dir);
    snprintf(fin, sizeof fin, "%s/MANIFEST", db->dir);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%llu\n", (unsigned long long)db->next_id);
    for (size_t i = 0; i < db->ntables; i++)
        fprintf(f, "%llu %d\n", (unsigned long long)db->tables[i]->id, db->tables[i]->level);
    fclose(f);
#ifdef _WIN32
    remove(fin);
#endif
    rename(tmp, fin);                    /* atomic replace on POSIX */
}

static void load_manifest(LSM *db) {
    char p[300];
    snprintf(p, sizeof p, "%s/MANIFEST", db->dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    unsigned long long next, id; int level;
    if (fscanf(f, "%llu", &next) == 1) db->next_id = next;
    while (fscanf(f, "%llu %d", &id, &level) == 2) {
        char path[300];
        table_path(db, id, path, sizeof path);
        SSTable *t = sst_open(path, id, level);
        if (t) add_table(db, t);
    }
    fclose(f);
}

void lsm_wipe(const char *dir) {
    LSM tmp; memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.dir, sizeof tmp.dir, "%s", dir);
    char p[300];
    snprintf(p, sizeof p, "%s/MANIFEST", dir);
    FILE *f = fopen(p, "r");
    if (f) {
        unsigned long long next, id; int level;
        if (fscanf(f, "%llu", &next) == 1)
            while (fscanf(f, "%llu %d", &id, &level) == 2) {
                char sp[300]; table_path(&tmp, id, sp, sizeof sp); remove(sp);
            }
        fclose(f);
    }
    remove(p);
    snprintf(p, sizeof p, "%s/wal.log", dir); remove(p);
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

static void replay_cb(const Record *r, void *ud) { sl_put((SkipList *)ud, r); }

LSM *lsm_open(const LSMConfig *cfg) {
    LSM *db = calloc(1, sizeof(LSM));
    db->cfg = *cfg;
    if (!db->cfg.memtable_max_entries) db->cfg.memtable_max_entries = 2000;
    if (db->cfg.level_fanout < 2)      db->cfg.level_fanout = 4;
    if (!db->cfg.bloom_bits_per_key)   db->cfg.bloom_bits_per_key = 10;
    snprintf(db->dir, sizeof db->dir, "%s", cfg->dir);
    MKDIR(db->dir);
    db->use_bloom = 1;
    db->next_id = 1;
    db->mem = sl_create(0x1234567ULL);

    load_manifest(db);

    /* Crash recovery: rebuild the MemTable from the write-ahead log. */
    char wp[300];
    snprintf(wp, sizeof wp, "%s/wal.log", db->dir);
    int clean = 1;
    db->recovered_entries = wal_replay(wp, replay_cb, db->mem, &clean);
    db->wal = wal_open(wp, db->cfg.wal_fsync);
    if (!db->wal) { free(db); return NULL; }
    if (!clean) {                         /* drop the torn tail by rewriting the WAL */
        wal_reset(db->wal);
        for (const SLNode *n = sl_first(db->mem); n; n = n->next[0]) wal_append(db->wal, &n->rec);
    }
    return db;
}

void lsm_close(LSM *db) {
    if (!db) return;
    wal_close(db->wal);
    for (size_t i = 0; i < db->ntables; i++) sst_close(db->tables[i]);
    free(db->tables);
    sl_free(db->mem);
    free(db);
}

/* ------------------------------------------------------------------ */
/* write path                                                          */
/* ------------------------------------------------------------------ */

static int write_internal(LSM *db, const Record *r) {
    /* 1) WAL first (durability) ... 2) only then the MemTable. */
    if (wal_append(db->wal, r) != 0) return -1;
    sl_put(db->mem, r);
    if (db->mem->count >= db->cfg.memtable_max_entries) return lsm_flush(db);
    return 0;
}

int lsm_put(LSM *db, const Record *r) {
    Record x = *r; x.tombstone = 0;
    db->stats.puts++;
    return write_internal(db, &x);
}

/* Lazy delete: write a tombstone marker. Nothing on disk is touched now. */
int lsm_delete(LSM *db, uint64_t key) {
    Record t; memset(&t, 0, sizeof t);
    t.key = key; t.tombstone = 1;
    db->stats.deletes++;
    return write_internal(db, &t);
}

/* ------------------------------------------------------------------ */
/* flush: MemTable -> SSTable                                          */
/* ------------------------------------------------------------------ */

int lsm_flush(LSM *db) {
    if (db->mem->count == 0) return 0;
    uint64_t t0 = now_ns();
    size_t cnt = db->mem->count, tombs = 0;
    uint64_t id = db->next_id++;
    char path[300];
    table_path(db, id, path, sizeof path);

    SSTWriter *w = sstw_begin(path, id, 0, cnt, db->cfg.bloom_bits_per_key);
    if (!w) return -1;
    /* The skip list is already sorted, so the flush is one sequential pass. */
    for (const SLNode *n = sl_first(db->mem); n; n = n->next[0]) {
        sstw_add(w, &n->rec);
        if (n->rec.tombstone) tombs++;
    }
    SSTable *t = sstw_finish(w);
    if (!t) return -1;
    add_table(db, t);
    save_manifest(db);        /* 1) table is durable and registered ...           */
    wal_reset(db->wal);       /* 2) ... only now may the WAL be discarded         */

    sl_free(db->mem);         /* 3) start a fresh MemTable                        */
    db->mem = sl_create(0x1234567ULL + id);
    db->stats.flushes++;

    event(db, C_YELLOW, "FLUSH",
          "MemTable full -> SSTable #%llu (L0)  %zu records (%zu tombstones), %.1f KB, "
          "min..max ts %llu..%llu, took %.2f ms",
          (unsigned long long)id, cnt, tombs, (double)(cnt * sizeof(Record)) / 1024.0,
          (unsigned long long)key_timestamp(t->min_key), (unsigned long long)key_timestamp(t->max_key),
          (double)(now_ns() - t0) / 1e6);

    lsm_compaction_tick(db);  /* "background" maintenance check after every flush */
    return 0;
}

/* ------------------------------------------------------------------ */
/* compaction                                                          */
/* ------------------------------------------------------------------ */

/*
 * K-way merge of `n` SSTables (given NEWEST FIRST) into one new SSTable on
 * `out_level`.
 *  - Equal keys: the newest table wins, older versions are dropped ("shadowed").
 *  - If `purge` is set, winning tombstones are discarded instead of copied:
 *    safe only when no older table below could still hold the deleted key.
 * Cost: O(total records * n) with a linear min-scan (n is tiny, <= fanout).
 * A min-heap would make it O(total * log n); noted in the README.
 */
static void merge_tables(LSM *db, SSTable **in, size_t n, int out_level, int purge) {
    uint64_t t0 = now_ns();
    uint64_t total_in = 0;
    for (size_t i = 0; i < n; i++) total_in += in[i]->count;

    uint64_t id = db->next_id++;
    char path[300];
    table_path(db, id, path, sizeof path);
    SSTWriter *w = sstw_begin(path, id, out_level, total_in, db->cfg.bloom_bits_per_key);
    if (!w) return;

    SSTIter *its  = malloc(n * sizeof(SSTIter));
    Record  *head = malloc(n * sizeof(Record));
    int     *has  = calloc(n, sizeof(int));
    for (size_t i = 0; i < n; i++) {
        sstit_seek(&its[i], in[i], 0);
        has[i] = sstit_next(&its[i], &head[i]);
    }

    uint64_t out_n = 0, purged = 0, shadowed = 0;
    for (;;) {
        int best = -1;                              /* smallest key; ties -> lowest index = newest */
        for (size_t i = 0; i < n; i++)
            if (has[i] && (best < 0 || head[i].key < head[best].key)) best = (int)i;
        if (best < 0) break;

        uint64_t k = head[best].key;
        Record winner = head[best];
        for (size_t i = 0; i < n; i++)              /* consume this key from every table */
            if (has[i] && head[i].key == k) {
                if ((int)i != best) shadowed++;
                has[i] = sstit_next(&its[i], &head[i]);
            }

        if (winner.tombstone && purge) { purged++; continue; }
        sstw_add(w, &winner);
        out_n++;
    }
    free(its); free(head); free(has);

    SSTable *nt = NULL;
    if (out_n == 0) sstw_abort(w);                  /* everything was purged */
    else            nt = sstw_finish(w);

    /* Swap: remove inputs from the live list, add output, persist manifest, delete old files. */
    size_t keep = 0;
    for (size_t i = 0; i < db->ntables; i++) {
        int is_input = 0;
        for (size_t j = 0; j < n; j++) if (db->tables[i] == in[j]) { is_input = 1; break; }
        if (!is_input) db->tables[keep++] = db->tables[i];
    }
    db->ntables = keep;
    if (nt) add_table(db, nt);
    save_manifest(db);
    for (size_t j = 0; j < n; j++) {
        char old[300]; snprintf(old, sizeof old, "%s", in[j]->path);
        sst_close(in[j]);
        remove(old);
    }

    db->stats.compactions++;
    db->stats.tombstones_purged += purged;
    db->stats.shadowed_dropped  += shadowed;
    event(db, C_MAGENTA, "COMPACT",
          "merged %zu SSTables -> 1 (L%d): %llu in, %llu out | %llu stale versions dropped, "
          "%llu tombstones purged | %.2f ms",
          n, out_level, (unsigned long long)total_in, (unsigned long long)out_n,
          (unsigned long long)shadowed, (unsigned long long)purged, (double)(now_ns() - t0) / 1e6);
}

static void compact_level(LSM *db, int level) {
    SSTable *in[256]; size_t n = 0;
    int deeper = 0;
    for (size_t i = 0; i < db->ntables && n < 256; i++) {
        if (db->tables[i]->level == level) in[n++] = db->tables[i];
        else if (db->tables[i]->level > level) deeper = 1;
    }
    /* Tombstones may only be purged if no OLDER data (deeper level) can exist. */
    merge_tables(db, in, n, level + 1, !deeper);
}

void lsm_compaction_tick(LSM *db) {
    for (int lvl = 0; lvl < MAX_LEVELS - 1; lvl++)
        if (count_level(db, lvl) >= db->cfg.level_fanout) compact_level(db, lvl);
}

/* Major compaction: merge every table into one and purge ALL tombstones. */
void lsm_compact_all(LSM *db) {
    if (db->ntables == 0) return;
    size_t n = db->ntables;
    SSTable **in = malloc(n * sizeof(SSTable *));
    memcpy(in, db->tables, n * sizeof(SSTable *));      /* already newest-first */
    int maxlvl = 0;
    for (size_t i = 0; i < n; i++) if (in[i]->level > maxlvl) maxlvl = in[i]->level;
    merge_tables(db, in, n, maxlvl, 1);
    free(in);
}

/* ------------------------------------------------------------------ */
/* read path: point lookup                                             */
/* ------------------------------------------------------------------ */

LookupStatus lsm_get(LSM *db, uint64_t key, Record *out, LookupTrace *trace) {
    LookupTrace local; if (!trace) trace = &local;
    memset(trace, 0, sizeof *trace);
    db->stats.gets++;

    /* 1) MemTable: newest data, O(log m) in RAM. */
    const Record *r = sl_get(db->mem, key);
    if (r) {
        if (r->tombstone) return LOOKUP_DELETED;
        *out = *r;
        db->stats.memtable_hits++;
        return LOOKUP_FOUND_MEMTABLE;
    }

    /* 2) SSTables, newest first: range check -> Bloom -> one disk block. */
    for (size_t i = 0; i < db->ntables; i++) {
        SSTable *t = db->tables[i];
        Record rec;
        SSTGetResult res = sst_get(t, key, &rec, db->use_bloom);
        trace->tables_probed++;
        switch (res) {
        case SST_OUT_OF_RANGE: trace->range_skips++; db->stats.range_skips++; break;
        case SST_BLOOM_SKIP:   trace->bloom_skips++; db->stats.bloom_negatives++; break;
        case SST_DISK_MISS:
            trace->disk_reads++; db->stats.disk_block_reads++;
            if (db->use_bloom) { trace->bloom_false_pos++; db->stats.bloom_false_pos++; }
            break;
        case SST_FOUND:
            trace->disk_reads++; db->stats.disk_block_reads++;
            trace->source_table_id = t->id;
            if (rec.tombstone) return LOOKUP_DELETED;
            *out = rec;
            db->stats.sstable_hits++;
            return LOOKUP_FOUND_SSTABLE;
        }
    }
    db->stats.not_found++;
    return LOOKUP_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* read path: range query                                              */
/* ------------------------------------------------------------------ */
/*
 * One cursor per source (MemTable + each overlapping SSTable). Every cursor is
 * positioned with an O(log n) seek, then a K-way merge streams the keys in
 * order; for duplicate keys the newest source wins; tombstones hide the key.
 * Cost: O(R log N) for the R seeks + O(K * R) for the merge  = O(K + log N)
 * for a fixed (small) number of sources R.
 */
typedef struct { const SLNode *mem_node; SSTIter it; Record head; int has; int is_mem; } Cursor;

static void cursor_advance(Cursor *c, uint64_t hi) {
    if (c->is_mem) {
        if (c->mem_node && c->mem_node->rec.key <= hi) {
            c->head = c->mem_node->rec;
            c->mem_node = c->mem_node->next[0];
            c->has = 1;
        } else c->has = 0;
    } else {
        c->has = sstit_next(&c->it, &c->head);
        if (c->has && c->head.key > hi) c->has = 0;
    }
}

size_t lsm_range(LSM *db, uint64_t ts_lo, uint64_t ts_hi, unsigned cell_filter, Record **out) {
    db->stats.range_queries++;
    uint64_t lo = make_key(ts_lo, 0), hi = make_key(ts_hi, (1u << CELL_BITS) - 1);

    size_t nsrc = 1 + db->ntables, ncur = 0;
    Cursor *cur = calloc(nsrc, sizeof(Cursor));

    cur[ncur].is_mem = 1;                                   /* source 0 = MemTable (newest) */
    cur[ncur].mem_node = sl_seek(db->mem, lo);
    cursor_advance(&cur[ncur], hi);
    ncur++;
    for (size_t i = 0; i < db->ntables; i++) {              /* then SSTables, newest first   */
        SSTable *t = db->tables[i];
        if (t->max_key < lo || t->min_key > hi) { db->stats.range_skips++; continue; }
        sstit_seek(&cur[ncur].it, t, lo);
        cursor_advance(&cur[ncur], hi);
        ncur++;
    }

    size_t cap = 256, n = 0;
    Record *res = malloc(cap * sizeof(Record));
    for (;;) {
        int best = -1;
        for (size_t i = 0; i < ncur; i++)
            if (cur[i].has && (best < 0 || cur[i].head.key < cur[best].head.key)) best = (int)i;
        if (best < 0) break;

        uint64_t k = cur[best].head.key;
        Record winner = cur[best].head;
        for (size_t i = 0; i < ncur; i++)
            if (cur[i].has && cur[i].head.key == k) cursor_advance(&cur[i], hi);

        if (winner.tombstone) continue;                     /* deleted */
        if (cell_filter && key_cell_id(k) != cell_filter) continue;
        if (n == cap) { cap *= 2; res = realloc(res, cap * sizeof(Record)); }
        res[n++] = winner;
    }
    free(cur);
    *out = res;
    return n;
}

/* ------------------------------------------------------------------ */
/* dashboard: current layout                                           */
/* ------------------------------------------------------------------ */

void lsm_print_layout(const LSM *db) {
    printf("  %sLSM layout%s  MemTable: %zu/%zu entries (skip list, %zu O(1) tail-appends, %zu O(log n) inserts)\n",
           C_BOLD, C_RESET, db->mem->count, db->cfg.memtable_max_entries,
           db->mem->fast_appends, db->mem->slow_inserts);
    if (db->ntables == 0) { printf("    (no SSTables on disk)\n"); return; }
    printf("    %-6s %-5s %9s %12s %12s %10s %12s\n",
           "level", "id", "records", "min_ts(ms)", "max_ts(ms)", "bloom(KB)", "size(KB)");
    for (size_t i = 0; i < db->ntables; i++) {
        const SSTable *t = db->tables[i];
        printf("    L%-5d #%-4llu %9llu %12llu %12llu %10.1f %12.1f\n", t->level,
               (unsigned long long)t->id, (unsigned long long)t->count,
               (unsigned long long)(key_timestamp(t->min_key) % 1000000000ULL),
               (unsigned long long)(key_timestamp(t->max_key) % 1000000000ULL),
               (double)bloom_nbytes(t->bloom) / 1024.0, (double)(t->count * sizeof(Record)) / 1024.0);
    }
}
