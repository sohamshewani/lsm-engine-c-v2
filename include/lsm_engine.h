#ifndef LSM_ENGINE_H
#define LSM_ENGINE_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ERROR CODES & STATUS HIERARCHY
 * ========================================================================= */
typedef enum {
    LSM_OK = 0,
    LSM_ERR_NOMEM = -1,
    LSM_ERR_IO = -2,
    LSM_ERR_CORRUPT = -3,
    LSM_ERR_NOT_FOUND = -4,
    LSM_ERR_BUSY = -5,
    LSM_ERR_INVALID_ARG = -6,
    LSM_ERR_MAX_LEVEL = -7
} lsm_status_t;

/* =========================================================================
 * PUBLIC CONFIGURATION & DATA STRUCTURES
 * ========================================================================= */
typedef struct {
    char* db_path;
    size_t memtable_capacity_bytes;
    size_t block_size;            /* Default: 4096 */
    bool use_lz4_compression;
    uint32_t max_background_threads;
    uint32_t l0_compaction_trigger;
    size_t target_file_size_bytes;
} lsm_options_t;

typedef struct {
    const uint8_t* data;
    size_t size;
} lsm_slice_t;

typedef struct {
    lsm_slice_t key;
    lsm_slice_t value;
    bool is_tombstone;
    uint64_t sequence_number;
} lsm_entry_t;

/* Opaque Handle to LSM Engine Instance */
typedef struct lsm_db lsm_db_t;

/* =========================================================================
 * CORE ENGINE API
 * ========================================================================= */
lsm_status_t lsm_open(const lsm_options_t* options, lsm_db_t** db_out);
void lsm_close(lsm_db_t* db);

lsm_status_t lsm_put(lsm_db_t* db, const lsm_slice_t* key, const lsm_slice_t* value);
lsm_status_t lsm_get(lsm_db_t* db, const lsm_slice_t* key, uint8_t* val_buf, size_t* val_len);
lsm_status_t lsm_delete(lsm_db_t* db, const lsm_slice_t* key);

lsm_status_t lsm_flush(lsm_db_t* db);
lsm_status_t lsm_compact(lsm_db_t* db);

/* =========================================================================
 * INTERNAL ARCHITECTURE SPECIFICATIONS (IMPLEMENTED IN HEADER)
 * ========================================================================= */

#define LSM_MAX_LEVELS 7
#define LSM_HAZARD_PTR_COUNT 4
#define LSM_SKIP_LIST_MAX_HEIGHT 32
#define LSM_MPSC_RING_SIZE 65536

/* Arena Allocator for Lock-Free MemTable Nodes */
typedef struct lsm_arena_block {
    struct lsm_arena_block* next;
    size_t capacity;
    size_t offset;
    char data[];
} lsm_arena_block_t;

typedef struct {
    pthread_mutex_t lock;
    lsm_arena_block_t* current_block;
    size_t block_size;
    size_t total_allocated;
} lsm_arena_t;

/* Skip-List Node Structure */
typedef struct lsm_skip_node {
    lsm_slice_t key;
    lsm_slice_t value;
    uint64_t sequence_number;
    bool is_tombstone;
    int height;
    struct lsm_skip_node* forward[];
} lsm_skip_node_t;

/* Hazard Pointer Record for Epoch-based GC */
typedef struct lsm_hazard_record {
    _Atomic(lsm_skip_node_t*) hazard[LSM_HAZARD_PTR_COUNT];
    struct lsm_hazard_record* next;
    pthread_t thread_id;
    bool active;
} lsm_hazard_record_t;

/* MemTable Structure */
typedef struct {
    lsm_arena_t arena;
    lsm_skip_node_t* head;
    _Atomic(int32_t) height;
    size_t byte_size;
    uint64_t min_seq;
    uint64_t max_seq;
    _Atomic(int32_t) ref_count;
} lsm_memtable_t;

/* MPSC Write Ring Buffer */
typedef struct {
    lsm_entry_t entries[LSM_MPSC_RING_SIZE];
    _Atomic(uint64_t) head;
    _Atomic(uint64_t) tail;
} lsm_mpsc_ring_t;

/* SSTable Metadata & Manifest */
typedef struct {
    uint32_t file_id;
    uint32_t level;
    uint64_t file_size;
    lsm_slice_t smallest_key;
    lsm_slice_t largest_key;
    uint32_t num_blocks;
    uint64_t* block_offsets;
    uint8_t* bloom_filter_data;
    size_t bloom_filter_size;
} lsm_sstable_meta_t;

typedef struct lsm_level_state {
    lsm_sstable_meta_t** tables;
    uint32_t num_tables;
    uint32_t capacity_tables;
} lsm_level_state_t;

typedef struct {
    uint64_t next_file_id;
    lsm_level_state_t levels[LSM_MAX_LEVELS];
} lsm_manifest_t;

/* Engine Root Structure */
struct lsm_db {
    lsm_options_t options;
    int wal_fd;
    pthread_mutex_t wal_lock;
    uint64_t current_sequence;
    
    lsm_memtable_t* active_memtable;
    lsm_memtable_t* immutable_memtable;
    pthread_mutex_t memtable_swap_lock;

    lsm_mpsc_ring_t write_ring;
    pthread_t wal_thread;
    bool shutdown;

    lsm_manifest_t manifest;
    pthread_rwlock_t manifest_lock;

    lsm_hazard_record_t* hazard_list;
    pthread_mutex_t hazard_mutex;

    pthread_t compaction_thread;
    pthread_cond_t compaction_cond;
    pthread_mutex_t compaction_mutex;
};

#ifdef __cplusplus
}
#endif

#endif /* LSM_ENGINE_H */
EOF

cat << 'EOF' > src/lsm_engine.c
#include "lsm_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <lz4.h>

/* =========================================================================
 * CRC32-C (Castagnoli) Implementation
 * ========================================================================= */
static uint32_t lsm_crc32c(uint32_t crc, const uint8_t *buf, size_t len) {
    static uint32_t table[256];
    static bool table_computed = false;
    if (!table_computed) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1) {
                    c = 0x82F63B78 ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[i] = c;
        }
        table_computed = true;
    }
    crc = ~crc;
    while (len--) {
        crc = table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* =========================================================================
 * MURMURHASH3 (32-bit variant) FOR BLOOM FILTERS
 * ========================================================================= */
static uint32_t lsm_murmur3_32(const void *key, size_t len, uint32_t seed) {
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    uint32_t c1 = 0xcc9e2d51;
    uint32_t c2 = 0x1b873593;

    const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i];
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;

        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xe6546b64;
    }

    const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= tail[2] << 16;
        case 2: k1 ^= tail[1] << 8;
        case 1: k1 ^= tail[0];
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            h1 ^= k1;
    }

    h1 ^= len;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;
    return h1;
}

/* =========================================================================
 * ARENA ALLOCATOR IMPLEMENTATION
 * ========================================================================= */
static lsm_status_t lsm_arena_init(lsm_arena_t* arena, size_t block_size) {
    if (pthread_mutex_init(&arena->lock, NULL) != 0) {
        return LSM_ERR_NOMEM;
    }
    arena->block_size = block_size;
    arena->total_allocated = 0;
    
    lsm_arena_block_t* block = (lsm_arena_block_t*)malloc(sizeof(lsm_arena_block_t) + block_size);
    if (!block) {
        pthread_mutex_destroy(&arena->lock);
        return LSM_ERR_NOMEM;
    }
    block->next = NULL;
    block->capacity = block_size;
    block->offset = 0;
    arena->current_block = block;
    arena->total_allocated += sizeof(lsm_arena_block_t) + block_size;
    return LSM_OK;
}

static void* lsm_arena_alloc(lsm_arena_t* arena, size_t size) {
    // Ensure 8-byte alignment
    size = (size + 7) & ~7;
    pthread_mutex_lock(&arena->lock);
    
    if (arena->current_block->offset + size > arena->current_block->capacity) {
        size_t new_cap = arena->block_size;
        if (size > new_cap) new_cap = size;
        lsm_arena_block_t* block = (lsm_arena_block_t*)malloc(sizeof(lsm_arena_block_t) + new_cap);
        if (!block) {
            pthread_mutex_unlock(&arena->lock);
            return NULL;
        }
        block->next = arena->current_block;
        block->capacity = new_cap;
        block->offset = 0;
        arena->current_block = block;
        arena->total_allocated += sizeof(lsm_arena_block_t) + new_cap;
    }
    
    void* ptr = &arena->current_block->data[arena->current_block->offset];
    arena->current_block->offset += size;
    pthread_mutex_unlock(&arena->lock);
    return ptr;
}

static void lsm_arena_destroy(lsm_arena_t* arena) {
    pthread_mutex_lock(&arena->lock);
    lsm_arena_block_t* block = arena->current_block;
    while (block) {
        lsm_arena_block_t* next = block->next;
        free(block);
        block = next;
    }
    pthread_mutex_unlock(&arena->lock);
    pthread_mutex_destroy(&arena->lock);
}

/* =========================================================================
 * SKIPLIST & MEMTABLE IMPLEMENTATION
 * ========================================================================= */
static int lsm_random_height(void) {
    int height = 1;
    while ((rand() % 4) == 0 && height < LSM_SKIP_LIST_MAX_HEIGHT) {
        height++;
    }
    return height;
}

static lsm_skip_node_t* lsm_skip_node_create(lsm_arena_t* arena, const lsm_slice_t* key, const lsm_slice_t* val, uint64_t seq, bool tombstone, int height) {
    size_t sz = sizeof(lsm_skip_node_t) + sizeof(lsm_skip_node_t*) * height;
    lsm_skip_node_t* node = (lsm_skip_node_t*)lsm_arena_alloc(arena, sz);
    if (!node) return NULL;

    void* k_buf = lsm_arena_alloc(arena, key->size);
    void* v_buf = val->size > 0 ? lsm_arena_alloc(arena, val->size) : NULL;
    if (!k_buf || (val->size > 0 && !v_buf)) return NULL;

    memcpy(k_buf, key->data, key->size);
    if (val->size > 0) memcpy(v_buf, val->data, val->size);

    node->key.data = (const uint8_t*)k_buf;
    node->key.size = key->size;
    node->value.data = (const uint8_t*)v_buf;
    node->value.size = val->size;
    node->sequence_number = seq;
    node->is_tombstone = tombstone;
    node->height = height;
    for (int i = 0; i < height; i++) {
        atomic_init(&node->forward[i], NULL);
    }
    return node;
}

static int lsm_compare_keys(const lsm_slice_t* a, const lsm_slice_t* b) {
    size_t min_len = a->size < b->size ? a->size : b->size;
    int cmp = memcmp(a->data, b->data, min_len);
    if (cmp != 0) return cmp;
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return 0;
}

static lsm_status_t lsm_memtable_init(lsm_memtable_t* mt, size_t arena_block_size) {
    lsm_status_t st = lsm_arena_init(&mt->arena, arena_block_size);
    if (st != LSM_OK)