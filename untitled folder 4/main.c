/*
 * main.c - Simulation, benchmarking and console dashboard for the
 *          LSM-Tree storage engine used as the data layer of an
 *          AI-enabled micro-channel battery thermal management system.
 *
 * Usage:  ./lsm_demo [N_readings] [--scaling] [--no-color]
 *         ./lsm_demo                -> full demo with 25,000 readings
 *         ./lsm_demo 100000         -> bigger run
 *         ./lsm_demo --scaling      -> complexity experiment (10k..640k)
 */
#include "lsm.h"
#include "datagen.h"
#include "util.h"
#include <string.h>

#define DB_DIR "lsm_data"

/* ----------------------------- helpers ----------------------------- */

static void banner(const char *title) {
    printf("\n%s%s==============================================================================%s\n", C_BOLD, C_CYAN, C_RESET);
    printf("%s%s  %s%s\n", C_BOLD, C_CYAN, title, C_RESET);
    printf("%s%s==============================================================================%s\n", C_BOLD, C_CYAN, C_RESET);
}

static uint64_t rng_state = 0x2545F4914F6CDD1DULL;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static const char *status_str(LookupStatus s) {
    switch (s) {
    case LOOKUP_FOUND_MEMTABLE: return "FOUND in MemTable (RAM)";
    case LOOKUP_FOUND_SSTABLE:  return "FOUND in SSTable (DISK)";
    case LOOKUP_DELETED:        return "DELETED (tombstone)";
    default:                    return "NOT FOUND";
    }
}

static void print_record(const Record *r) {
    printf("      C%-2u ts=%llu  T_batt=%6.2f C  T_cool=%6.2f C  V=%5.3f V  I=%6.2f A  flow=%4.2f L/min\n",
           key_cell_id(r->key), (unsigned long long)key_timestamp(r->key),
           r->battery_temperature, r->coolant_temperature, r->voltage, r->current, r->coolant_flow_rate);
}

/* One traced point lookup with dashboard output. */
static uint64_t traced_get(LSM *db, const char *label, uint64_t key) {
    Record r; LookupTrace tr;
    uint64_t t0 = now_ns();
    LookupStatus st = lsm_get(db, key, &r, &tr);
    uint64_t dt = now_ns() - t0;
    printf("  %s[GET]%s %s  (C%u @ ts=%llu)\n", C_BOLD, C_RESET, label, key_cell_id(key),
           (unsigned long long)key_timestamp(key));
    printf("      -> %s%s%s in %s%llu ns%s\n", st == LOOKUP_FOUND_MEMTABLE ? C_GREEN : (st == LOOKUP_NOT_FOUND || st == LOOKUP_DELETED ? C_RED : C_YELLOW),
           status_str(st), C_RESET, C_BOLD, (unsigned long long)dt, C_RESET);
    if (st == LOOKUP_FOUND_MEMTABLE || st == LOOKUP_FOUND_SSTABLE) print_record(&r);
    if (st != LOOKUP_FOUND_MEMTABLE)
        printf("      probes: %d SSTables checked | %d skipped by key-range | %s%d skipped by BLOOM FILTER (disk read avoided)%s | %d disk block reads\n",
               tr.tables_probed, tr.range_skips, C_GREEN, tr.bloom_skips, C_RESET, tr.disk_reads);
    return dt;
}

/* ----------------------- MemTable micro-benchmark ------------------- */

static void bench_memtable(void) {
    banner("MEMTABLE MICRO-BENCHMARK  (skip list, in isolation, no disk)");
    printf("  Time-series inserts arrive in timestamp order -> O(1) tail append.\n");
    printf("  Random-order inserts must search -> O(log n). Same data structure, two paths:\n\n");
    printf("  %10s | %22s | %22s\n", "entries n", "ordered insert ns/op", "random insert ns/op");
    printf("  -----------+------------------------+-----------------------\n");
    size_t sizes[] = {1000, 10000, 100000, 500000};
    for (int s = 0; s < 4; s++) {
        size_t n = sizes[s];
        Record r; memset(&r, 0, sizeof r);

        SkipList *a = sl_create(1);
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < n; i++) { r.key = (uint64_t)i * 16 + 1; sl_put(a, &r); }
        double ord = (double)(now_ns() - t0) / (double)n;

        SkipList *b = sl_create(1);
        t0 = now_ns();
        for (size_t i = 0; i < n; i++) { r.key = mix64(i) >> 8; sl_put(b, &r); }
        double rndm = (double)(now_ns() - t0) / (double)n;

        printf("  %10zu | %22.1f | %22.1f\n", n, ord, rndm);
        sl_free(a); sl_free(b);
    }
    printf("\n  %s=> ordered ingestion cost stays flat as n grows (O(1)); random-order cost grows (O(log n)).%s\n", C_GREEN, C_RESET);
}

/* ------------------------- scaling experiment ----------------------- */

static void run_scaling(void) {
    banner("SCALING EXPERIMENT  (does the engine behave like the theory says?)");
    printf("  MemTable fixed at 2,000 entries, fanout 4, Bloom 10 bits/key. Only N grows.\n\n");
    printf("  %9s | %7s | %12s | %14s | %16s | %14s\n",
           "N", "tables", "insert ns/op", "lookup(old) ns", "lookup(miss) ns", "range K=1000 us");
    printf("  ----------+---------+--------------+----------------+------------------+----------------\n");
    size_t sizes[] = {20000, 70000, 250000, 900000};
    for (int s = 0; s < 4; s++) {
        size_t N = sizes[s];
        LSMConfig cfg = {DB_DIR, 2000, 4, 10, 0, 0};
        lsm_wipe(DB_DIR);
        LSM *db = lsm_open(&cfg);
        SensorSim sim; sim_init(&sim, 1760000000000ULL, 42);
        uint64_t *keys = malloc(N * sizeof(uint64_t));
        Record r;
        uint64_t t0 = now_ns();
        for (size_t i = 0; i < N; i++) { sim_next(&sim, &r); keys[i] = r.key; lsm_put(db, &r); }
        double ins = (double)(now_ns() - t0) / (double)N;

        int Q = 20000;
        t0 = now_ns();
        for (int q = 0; q < Q; q++) lsm_get(db, keys[rnd() % (N * 9 / 10)], &r, NULL);
        double look = (double)(now_ns() - t0) / Q;

        t0 = now_ns();
        for (int q = 0; q < Q; q++) {
            uint64_t k = keys[rnd() % (N * 9 / 10)];
            lsm_get(db, make_key(key_timestamp(k), 11 + (unsigned)(rnd() % 5)), &r, NULL);
        }
        double miss = (double)(now_ns() - t0) / Q;

        Record *res; double rng_us = 0;
        for (int q = 0; q < 20; q++) {
            uint64_t lo = key_timestamp(keys[rnd() % (N / 2)]);
            t0 = now_ns();
            size_t K = lsm_range(db, lo, lo + 99 * 100 + 99, 0, &res);   /* 100 ticks = 1000 records */
            rng_us += (double)(now_ns() - t0) / 1000.0; (void)K;
            free(res);
        }
        printf("  %9zu | %7zu | %12.0f | %14.0f | %16.0f | %14.1f\n", N, db->ntables, ins, look, miss, rng_us / 20);
        fflush(stdout);
        free(keys);
        lsm_close(db);
    }
    lsm_wipe(DB_DIR);
    printf("\n  Read: insert stays ~flat (O(1) amortised). Lookups grow slowly: each extra tier of\n");
    printf("  SSTables adds one cheap range/Bloom check, and the table count is bounded by\n");
    printf("  (fanout-1) * log_fanout(N/memtable) = O(log N). The 'tables' column fluctuates because it\n");
    printf("  follows the base-4 digits of the flush count (like a counter). A fixed-size range query\n");
    printf("  stays ~constant: O(K + log N) with K fixed.\n");
}

/* ================================ main ================================ */

int main(int argc, char **argv) {
    size_t N = 25000;
    int scaling = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scaling")) scaling = 1;
        else if (!strcmp(argv[i], "--no-color")) g_use_color = 0;
        else if (atol(argv[i]) > 0) N = (size_t)atol(argv[i]);
    }
    if (getenv("NO_COLOR")) g_use_color = 0;
    if (N < 10000) N = 10000;

    printf("%s%s\n  LSM-TREE STORAGE ENGINE  -  EV Battery Thermal Management data layer\n", C_BOLD, C_GREEN);
    printf("  10 x 21700 Li-ion cells (C1..C10), micro-channel liquid cooling, 10 Hz sampling%s\n", C_RESET);

    if (scaling) { bench_memtable(); run_scaling(); return 0; }

    /* ------------------------------------------------------------------ */
    banner("PHASE 1: OPEN DATABASE");
    LSMConfig cfg = { DB_DIR, 2000, 4, 10, /*verbose*/ 1, /*fsync*/ 0 };
    lsm_wipe(DB_DIR);
    LSM *db = lsm_open(&cfg);
    if (!db) { fprintf(stderr, "cannot open database in ./%s (does the directory exist / is it writable?)\n", DB_DIR); return 1; }
    printf("  MemTable threshold : %zu entries (skip list)\n", cfg.memtable_max_entries);
    printf("  Compaction trigger : %u SSTables in one level -> merge into 1 table of the next level\n", cfg.level_fanout);
    printf("  Bloom filter       : %u bits/key (~1%% false-positive rate)\n", cfg.bloom_bits_per_key);
    printf("  Record size        : %zu bytes, key = (timestamp_ms << 4) | cell_id\n", sizeof(Record));

    /* ------------------------------------------------------------------ */
    banner("PHASE 2: STREAMING INGESTION (WAL -> MemTable -> flush -> compaction)");
    printf("  Streaming %zu sensor readings (%zu ticks x 10 cells)...\n\n", N, N / NUM_CELLS);
    SensorSim sim; sim_init(&sim, 1760000000000ULL, 42);
    uint64_t *keys = malloc(N * sizeof(uint64_t));
    uint32_t *lat  = malloc(N * sizeof(uint32_t));
    uint64_t batch_sum = 0, total_ns = 0;
    size_t batch = cfg.memtable_max_entries, batch_no = 0;
    Record r;
    for (size_t i = 0; i < N; i++) {
        sim_next(&sim, &r);
        keys[i] = r.key;
        uint64_t t0 = now_ns();
        lsm_put(db, &r);
        uint64_t dt = now_ns() - t0;
        lat[i] = dt > UINT32_MAX ? UINT32_MAX : (uint32_t)dt;
        batch_sum += dt; total_ns += dt;
        if ((i + 1) % batch == 0) {
            printf("  %s[INGEST ]%s batch %2zu done | %6zu records total | avg %6.0f ns/insert (flush cost amortised in)\n",
                   C_BOLD, C_RESET, ++batch_no, i + 1, (double)batch_sum / (double)batch);
            batch_sum = 0;
        }
    }
    qsort(lat, N, sizeof(uint32_t), cmp_u32);
    printf("\n  %sInsert latency over %zu writes%s (each = WAL append + MemTable insert):\n", C_BOLD, N, C_RESET);
    printf("    mean %.0f ns | p50 %u ns | p90 %u ns | p99 %u ns | max %u ns (= a write that triggered a flush)\n",
           (double)total_ns / (double)N, lat[N / 2], lat[N * 90 / 100], lat[N * 99 / 100], lat[N - 1]);
    printf("    throughput ~ %.0f inserts/second\n\n", 1e9 / ((double)total_ns / (double)N));
    lsm_print_layout(db);

    /* ------------------------------------------------------------------ */
    banner("PHASE 3: POINT LOOKUP  -  RAM (MemTable) vs DISK (SSTable)");
    size_t mem_n = db->mem->count;
    if (mem_n < 10) mem_n = 10;
    uint64_t recent_key = keys[N - 1];
    uint64_t old_key    = keys[N / 50];
    traced_get(db, "most recent reading (should be in RAM)", recent_key);
    printf("\n");
    traced_get(db, "an OLD reading (must come from disk)", old_key);

    int Q = 50000;
    Record rec;
    uint64_t t0 = now_ns();
    for (int q = 0; q < Q; q++) lsm_get(db, keys[N - 1 - rnd() % mem_n], &rec, NULL);
    double ram_ns = (double)(now_ns() - t0) / Q;
    t0 = now_ns();
    for (int q = 0; q < Q; q++) lsm_get(db, keys[rnd() % (N * 9 / 10)], &rec, NULL);
    double disk_ns = (double)(now_ns() - t0) / Q;
    printf("\n  %sAverage over %d random lookups:%s\n", C_BOLD, Q, C_RESET);
    printf("    recent records (MemTable, RAM) : %s%8.0f ns%s\n", C_GREEN, ram_ns, C_RESET);
    printf("    old records    (SSTable, DISK) : %s%8.0f ns%s   -> %.1fx slower than RAM\n", C_YELLOW, disk_ns, C_RESET, disk_ns / ram_ns);
    printf("    (disk figure is optimistic: the OS page cache keeps the files in RAM; a cold SSD/HDD read costs 100 us - 10 ms)\n");

    /* ------------------------------------------------------------------ */
    banner("PHASE 4: BLOOM FILTER  -  lookups for readings that do NOT exist");
    printf("  Query: valid timestamps but cell ids C11..C15 (no such cell) -> key lies INSIDE every\n");
    printf("  table's min/max range, so only the Bloom filter can save us from reading the disk.\n\n");
    int M = 30000;
    LSMStats s0, s1;
    for (int pass = 0; pass < 2; pass++) {
        db->use_bloom = (pass == 0);
        s0 = db->stats;
        t0 = now_ns();
        rng_state = 0xABCDEF12345ULL;
        for (int q = 0; q < M; q++) {
            uint64_t k = keys[rnd() % (N * 9 / 10)];
            lsm_get(db, make_key(key_timestamp(k), 11 + (unsigned)(rnd() % 5)), &rec, NULL);
        }
        double ns = (double)(now_ns() - t0) / M;
        s1 = db->stats;
        printf("  Bloom filter %s%-3s%s : %8.0f ns/lookup | disk block reads: %8llu | reads avoided by Bloom: %8llu",
               pass == 0 ? C_GREEN : C_RED, pass == 0 ? "ON" : "OFF", C_RESET, ns,
               (unsigned long long)(s1.disk_block_reads - s0.disk_block_reads),
               (unsigned long long)(s1.bloom_negatives - s0.bloom_negatives));
        if (pass == 0) {
            uint64_t fp = s1.bloom_false_pos - s0.bloom_false_pos, neg = s1.bloom_negatives - s0.bloom_negatives;
            printf(" | measured false-positive rate %.2f%%", 100.0 * (double)fp / (double)(fp + neg));
        }
        printf("\n");
    }
    db->use_bloom = 1;
    printf("\n  A single traced example with the Bloom filter on:\n");
    {
        uint64_t k = keys[N / 3];
        traced_get(db, "non-existent cell C13 at an existing timestamp", make_key(key_timestamp(k), 13));
    }

    /* ------------------------------------------------------------------ */
    banner("PHASE 5: RANGE SEARCH  -  history windows for the AI model");
    uint64_t base_ts = key_timestamp(keys[N / 4]);
    printf("  Window starts at ts=%llu (old data: spread over several SSTables).\n\n", (unsigned long long)base_ts);
    printf("  %12s | %10s | %12s | %12s\n", "window", "K records", "time (us)", "ns / record");
    printf("  -------------+------------+--------------+-------------\n");
    int ticks_list[] = {1, 10, 100, 1000};
    for (int w = 0; w < 4; w++) {
        int ticks = ticks_list[w];
        if ((size_t)ticks * NUM_CELLS > N / 2) continue;
        Record *res; size_t K = 0; int reps = 30;
        t0 = now_ns();
        for (int q = 0; q < reps; q++) { K = lsm_range(db, base_ts, base_ts + (uint64_t)ticks * 100 - 1, 0, &res); free(res); }
        double us = (double)(now_ns() - t0) / reps / 1000.0;
        printf("  %9.1f s  | %10zu | %12.1f | %12.0f\n", ticks * 0.1, K, us, us * 1000.0 / (double)K);
    }
    printf("\n  Time grows roughly linearly with K (the output size), not with the size of the database.\n");
    {
        Record *res;
        size_t K = lsm_range(db, base_ts, base_ts + 49 * 100, 3, &res);   /* 5 s of cell C3 */
        printf("\n  %sAI feature window:%s last 5 s of cell C3 -> %zu readings. First 3 and last:\n", C_BOLD, C_RESET, K);
        for (size_t i = 0; i < 3 && i < K; i++) print_record(&res[i]);
        if (K) print_record(&res[K - 1]);
        double sum = 0, mx = -1e9;
        for (size_t i = 0; i < K; i++) { sum += res[i].battery_temperature; if (res[i].battery_temperature > mx) mx = res[i].battery_temperature; }
        if (K) printf("      mean T_batt = %.2f C, max T_batt = %.2f C  (ready to feed into a model)\n", sum / (double)K, mx);
        free(res);
    }

    /* ------------------------------------------------------------------ */
    banner("PHASE 6: DELETE  -  lazy deletion with tombstones");
    uint64_t victims[3] = { keys[N / 10], keys[N / 10 + 1], keys[N / 10 + 2] };
    traced_get(db, "before delete (record lives on disk)", victims[0]);
    t0 = now_ns();
    for (int i = 0; i < 3; i++) lsm_delete(db, victims[i]);
    printf("\n  %s[DELETE]%s wrote 3 tombstones in %llu ns total (WAL append + MemTable insert; the disk copy is untouched)\n",
           C_BOLD, C_RESET, (unsigned long long)(now_ns() - t0));
    traced_get(db, "after delete (tombstone in MemTable shadows the disk copy)", victims[0]);
    {
        Record *res;
        uint64_t ts = key_timestamp(victims[0]);
        size_t K = lsm_range(db, ts, ts, 0, &res);
        printf("  range query for that exact timestamp returns %zu readings (10 cells minus the 3 deleted ones = 7: tombstones hide them)\n", K);
        free(res);
    }
    printf("\n  Now flush + MAJOR COMPACTION to physically purge the tombstones:\n");
    lsm_flush(db);                      /* tombstones travel to disk like any other record */
    uint64_t before = 0;
    for (size_t i = 0; i < db->ntables; i++) before += db->tables[i]->count;
    lsm_compact_all(db);
    uint64_t after = 0;
    for (size_t i = 0; i < db->ntables; i++) after += db->tables[i]->count;
    printf("  records on disk before compaction: %llu -> after: %llu (3 deleted records + their 3 tombstones removed)\n",
           (unsigned long long)before, (unsigned long long)after);
    traced_get(db, "after compaction (data physically gone, still NOT FOUND)", victims[0]);
    printf("\n");
    lsm_print_layout(db);

    /* ------------------------------------------------------------------ */
    banner("PHASE 7: CRASH RECOVERY  -  Write-Ahead Log replay");
    uint64_t crash_keys[5];
    for (int i = 0; i < 5; i++) { sim_next(&sim, &r); crash_keys[i] = r.key; lsm_put(db, &r); }
    printf("  Wrote 5 new readings (only in RAM + WAL), then 'power loss' - closing WITHOUT flushing.\n");
    LSMStats saved = db->stats;        /* counters live in RAM; keep them for the summary */
    lsm_close(db);
    cfg.verbose = 1;
    db = lsm_open(&cfg);
    printf("  %s[RECOVER]%s reopened database: WAL replay restored %zu entries into the MemTable\n", C_BOLD, C_RESET, db->recovered_entries);
    int ok = 0;
    for (int i = 0; i < 5; i++) ok += (lsm_get(db, crash_keys[i], &rec, NULL) == LOOKUP_FOUND_MEMTABLE);
    printf("  verification: %d / 5 un-flushed readings found after restart %s\n", ok, ok == 5 ? "(no data lost)" : "(DATA LOSS!)");

    /* ------------------------------------------------------------------ */
    banner("SUMMARY");
    const LSMStats *st = &saved;
    printf("  flushes: %llu | compactions: %llu | stale versions dropped: %llu | tombstones purged: %llu\n",
           (unsigned long long)st->flushes, (unsigned long long)st->compactions,
           (unsigned long long)st->shadowed_dropped, (unsigned long long)st->tombstones_purged);
    printf("  lookups: %llu | MemTable hits: %llu | SSTable hits: %llu\n\n",
           (unsigned long long)st->gets, (unsigned long long)st->memtable_hits, (unsigned long long)st->sstable_hits);
    printf("  Operation        Where it costs                          Complexity\n");
    printf("  ---------------  --------------------------------------  ---------------------------\n");
    printf("  Insert           WAL append + skip-list tail append      O(1) (O(log m) if out of order)\n");
    printf("  Point lookup     MemTable, then range+Bloom+1 block/SST  O(log N)\n");
    printf("  Range search     seek per source + ordered merge         O(K + log N)\n");
    printf("  Delete           tombstone insert (lazy)                 O(1)\n");
    printf("  Flush / compact  sequential merge, amortised over writes O(1) amortised per record\n\n");
    printf("  Tip: run  ./lsm_demo --scaling  to see these complexities measured as N grows.\n");

    bench_memtable();

    free(keys); free(lat);
    lsm_close(db);
    lsm_wipe(DB_DIR);
    return 0;
}
