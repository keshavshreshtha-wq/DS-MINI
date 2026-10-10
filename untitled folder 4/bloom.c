#include "bloom.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

Bloom *bloom_alloc(uint64_t nbits, uint32_t k) {
    if (nbits < 64) nbits = 64;
    Bloom *b = malloc(sizeof(Bloom));
    b->nbits = nbits;
    b->k = k ? k : 1;
    b->bits = calloc((nbits + 7) / 8, 1);
    return b;
}

Bloom *bloom_create(uint64_t expected_keys, unsigned bits_per_key) {
    if (expected_keys == 0) expected_keys = 1;
    if (bits_per_key == 0) bits_per_key = 10;
    /* Optimal number of hash functions: k = (m/n) * ln 2  ~ 0.69 * bits_per_key */
    uint32_t k = (uint32_t)(bits_per_key * 0.69);
    if (k < 1)  k = 1;
    if (k > 30) k = 30;
    return bloom_alloc(expected_keys * bits_per_key, k);
}

/* Kirsch-Mitzenmacher double hashing: the i-th probe position is
 *     (h1 + i*h2) mod m
 * which behaves like k independent hash functions but needs only 2 real hashes. */
static inline void probe_hashes(uint64_t key, uint64_t *h1, uint64_t *h2) {
    *h1 = mix64(key);
    *h2 = mix64(key ^ 0xA5A5A5A5DEADBEEFULL) | 1;   /* |1 => odd, never 0 */
}

void bloom_add(Bloom *b, uint64_t key) {
    uint64_t h1, h2;
    probe_hashes(key, &h1, &h2);
    for (uint32_t i = 0; i < b->k; i++) {
        uint64_t pos = (h1 + (uint64_t)i * h2) % b->nbits;
        b->bits[pos >> 3] |= (uint8_t)(1u << (pos & 7));
    }
}

int bloom_may_contain(const Bloom *b, uint64_t key) {
    uint64_t h1, h2;
    probe_hashes(key, &h1, &h2);
    for (uint32_t i = 0; i < b->k; i++) {
        uint64_t pos = (h1 + (uint64_t)i * h2) % b->nbits;
        if (!(b->bits[pos >> 3] & (1u << (pos & 7))))
            return 0;                      /* one zero bit => definitely absent */
    }
    return 1;                              /* all bits set => possibly present  */
}

size_t bloom_nbytes(const Bloom *b) { return (size_t)((b->nbits + 7) / 8); }

void bloom_free(Bloom *b) {
    if (!b) return;
    free(b->bits);
    free(b);
}
