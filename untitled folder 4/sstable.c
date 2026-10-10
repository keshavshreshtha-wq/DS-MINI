#include "sstable.h"
#include <stdlib.h>
#include <string.h>

#define SST_MAGIC "LSMSST01"

/* ===================================================================== */
/*                               WRITER                                  */
/* ===================================================================== */

SSTWriter *sstw_begin(const char *path, uint64_t id, int level,
                      uint64_t max_entries, unsigned bloom_bits_per_key) {
    SSTWriter *w = calloc(1, sizeof(SSTWriter));
    snprintf(w->path, sizeof w->path, "%s", path);
    w->id = id; w->level = level;
    w->fp = fopen(path, "wb");
    if (!w->fp) { free(w); return NULL; }
    /* The Bloom filter must be sized before we know the exact record count,
     * so we size it for the upper bound `max_entries`. */
    w->bloom = bloom_create(max_entries, bloom_bits_per_key);
    w->min_key = UINT64_MAX;

    /* Reserve space for header + Bloom bits; they are filled in at finish(). */
    SSTHeader hdr; memset(&hdr, 0, sizeof hdr);
    fwrite(&hdr, sizeof hdr, 1, w->fp);
    uint8_t *zeros = calloc(bloom_nbytes(w->bloom), 1);
    fwrite(zeros, 1, bloom_nbytes(w->bloom), w->fp);
    free(zeros);
    w->data_offset = sizeof(SSTHeader) + bloom_nbytes(w->bloom);
    return w;
}

int sstw_add(SSTWriter *w, const Record *r) {
    if (w->count > 0 && r->key <= w->last_key) return -1;   /* SSTables must be sorted */
    if (fwrite(r, sizeof(Record), 1, w->fp) != 1) return -1;
    bloom_add(w->bloom, r->key);
    if (r->key < w->min_key) w->min_key = r->key;
    w->max_key = w->last_key = r->key;
    w->count++;
    return 0;
}

SSTable *sstw_finish(SSTWriter *w) {
    SSTHeader hdr; memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, SST_MAGIC, 8);
    hdr.count = w->count; hdr.min_key = w->min_key; hdr.max_key = w->max_key;
    hdr.bloom_nbits = w->bloom->nbits; hdr.bloom_k = w->bloom->k;
    fseek(w->fp, 0, SEEK_SET);
    fwrite(&hdr, sizeof hdr, 1, w->fp);
    fwrite(w->bloom->bits, 1, bloom_nbytes(w->bloom), w->fp);
    fflush(w->fp);
    fclose(w->fp);
    SSTable *t = sst_open(w->path, w->id, w->level);
    bloom_free(w->bloom);
    free(w);
    return t;
}

void sstw_abort(SSTWriter *w) {
    fclose(w->fp);
    remove(w->path);
    bloom_free(w->bloom);
    free(w);
}

/* ===================================================================== */
/*                               READER                                  */
/* ===================================================================== */

SSTable *sst_open(const char *path, uint64_t id, int level) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    SSTHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || memcmp(hdr.magic, SST_MAGIC, 8) != 0) {
        fclose(f); return NULL;
    }
    SSTable *t = calloc(1, sizeof(SSTable));
    snprintf(t->path, sizeof t->path, "%s", path);
    t->fp = f; t->id = id; t->level = level;
    t->count = hdr.count; t->min_key = hdr.min_key; t->max_key = hdr.max_key;

    t->bloom = bloom_alloc(hdr.bloom_nbits, hdr.bloom_k);
    if (fread(t->bloom->bits, 1, bloom_nbytes(t->bloom), f) != bloom_nbytes(t->bloom)) { sst_close(t); return NULL; }
    t->data_offset = sizeof(SSTHeader) + bloom_nbytes(t->bloom);

    /* Build the sparse index: read the key of every SST_SPARSE-th record. */
    t->index_n = (size_t)((t->count + SST_SPARSE - 1) / SST_SPARSE);
    t->index_keys = malloc((t->index_n ? t->index_n : 1) * sizeof(uint64_t));
    for (size_t i = 0; i < t->index_n; i++) {
        Record r;
        fseek(f, (long)(t->data_offset + (uint64_t)i * SST_SPARSE * sizeof(Record)), SEEK_SET);
        if (fread(&r, sizeof r, 1, f) != 1) { sst_close(t); return NULL; }
        t->index_keys[i] = r.key;
    }
    return t;
}

void sst_close(SSTable *t) {
    if (!t) return;
    if (t->fp) fclose(t->fp);
    bloom_free(t->bloom);
    free(t->index_keys);
    free(t);
}

/* Largest sparse-index slot whose key <= `key` (caller guarantees key >= min_key). */
static size_t index_floor(const SSTable *t, uint64_t key) {
    size_t lo = 0, hi = t->index_n;          /* find first slot with index_keys > key */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->index_keys[mid] <= key) lo = mid + 1; else hi = mid;
    }
    return lo ? lo - 1 : 0;
}

SSTGetResult sst_get(SSTable *t, uint64_t key, Record *out, int use_bloom) {
    /* Step 1: key range check - free, no I/O, no hashing. */
    if (key < t->min_key || key > t->max_key) return SST_OUT_OF_RANGE;

    /* Step 2: Bloom filter - O(k) memory probes, avoids the disk read on "no". */
    if (use_bloom && !bloom_may_contain(t->bloom, key)) return SST_BLOOM_SKIP;

    /* Step 3: binary search the in-memory sparse index -> one 32-record block. */
    size_t slot  = index_floor(t, key);
    uint64_t first = (uint64_t)slot * SST_SPARSE;
    uint64_t n = t->count - first;
    if (n > SST_SPARSE) n = SST_SPARSE;

    /* Step 4: ONE disk read, then binary search inside the block. */
    Record block[SST_SPARSE];
    fseek(t->fp, (long)(t->data_offset + first * sizeof(Record)), SEEK_SET);
    if (fread(block, sizeof(Record), (size_t)n, t->fp) != n) return SST_DISK_MISS;

    size_t lo = 0, hi = (size_t)n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (block[mid].key < key) lo = mid + 1; else hi = mid;
    }
    if (lo < n && block[lo].key == key) { *out = block[lo]; return SST_FOUND; }
    return SST_DISK_MISS;
}

/* ---------------------------- iterator ------------------------------- */

void sstit_seek(SSTIter *it, SSTable *t, uint64_t key) {
    it->t = t;
    it->buf_n = it->buf_i = 0;
    it->skip_below = key;
    if (key <= t->min_key)      it->pos = 0;
    else if (key > t->max_key)  it->pos = t->count;                 /* nothing to read */
    else                        it->pos = (uint64_t)index_floor(t, key) * SST_SPARSE;
}

int sstit_next(SSTIter *it, Record *out) {
    for (;;) {
        if (it->buf_i == it->buf_n) {                                /* refill buffer */
            if (it->pos >= it->t->count) return 0;
            uint64_t n = it->t->count - it->pos;
            if (n > SST_ITER_BUF) n = SST_ITER_BUF;
            fseek(it->t->fp, (long)(it->t->data_offset + it->pos * sizeof(Record)), SEEK_SET);
            if (fread(it->buf, sizeof(Record), (size_t)n, it->t->fp) != n) return 0;
            it->buf_n = (size_t)n; it->buf_i = 0; it->pos += n;
        }
        const Record *r = &it->buf[it->buf_i++];
        if (r->key < it->skip_below) continue;                       /* skip the block prefix */
        *out = *r;
        return 1;
    }
}
