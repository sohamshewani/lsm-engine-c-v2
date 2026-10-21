#ifndef LSM_TYPES_H
#define LSM_TYPES_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * MACROS, CONSTANTS, AND ERROR CODES
 * ========================================================================= */

#define LSM_MAX_KEY_LEN         65536
#define LSM_MAX_VAL_LEN         (1024 * 1024 * 64)
#define LSM_BLOCK_SIZE          4096
#define LSM_MAX_LEVELS          7
#define LSM_DEFAULT_SPARSE_N    16
#define LSM_HAZARD_PTR_COUNT    8
#define LSM_MAGIC_NUMBER        0x4C534D454E47494EULL /* 'LSMENGINE' */

typedef enum {
    LSM_OK = 0,
    LSM_ERR_NOMEM = -1,
    LSM_ERR_IO = -2,
    LSM_ERR_CORRUPT = -3,
    LSM_ERR_NOT_FOUND = -4,
    LSM_ERR_FULL = -5,
    LSM_ERR_INVALID_ARG = -6,
    LSM_ERR_BUSY = -7,
    LSM_ERR_CHECKSUM = -8
} lsm_status_t;

typedef enum {
    LSM_OP_PUT = 1,
    LSM_OP_DEL = 2
} lsm_op_type_t;

/* =========================================================================
 * CORE DATA STRUCTURES: SLIST, ARENA, MEMTABLE
 * ========================================================================= */

typedef struct lsm_slice {
    uint8_t *data;
    size_t len;
} lsm_slice_t;

typedef struct lsm_arena_chunk {
    struct lsm_arena_chunk *next;
    size_t capacity;
    size_t used;
    uint8_t data[];
} lsm_arena_chunk_t;

typedef struct lsm_arena {
    lsm_arena_chunk_t *head;
    lsm_arena_chunk_t *current;
    size_t default_chunk_size;
    uint64_t total_allocated;
} lsm_arena_t;

typedef struct lsm_skip_node {
    lsm_slice_t key;
    lsm_slice_t val;
    uint64_t sequence;
    uint8_t op_type;
    uint8_t height;
    struct lsm_skip_node *forward[];
} lsm_skip_node_t;

typedef struct lsm_memtable {
    lsm_skip_node_t *head;
    lsm_arena_t arena;
    uint32_t max_height;
    uint32_t current_height;
    intrusive_ref_count_t ref_count; /* Managed via atomic operations */
    size_t byte_size;
    uint64_t min_seq;
    uint64_t max_seq;
    bool is_immutable;
} lsm_memtable_t;

/* =========================================================================
 * HAZARD POINTERS & CONCURRENCY
 * ========================================================================= */

typedef struct lsm_hazard_record {
    _Atomic(void *) ptr;
    struct lsm_hazard_record *next;
    bool active;
} lsm_hazard_record_t;

typedef struct lsm_hp_domain {
    _Atomic(lsm_hazard_record_t *) head;
    _Atomic(uint64_t) thread_count;
} lsm_hp_domain_t;

/* =========================================================================
 * SSTABLE, INDEX, AND BLOOM FILTER FORMATS
 * ========================================================================= */

typedef struct lsm_block_handle {
    uint64_t offset;
    uint32_t size;
    uint32_t checksum;
} lsm_block_handle_t;

typedef struct lsm_index_entry {
    lsm_slice_t key;
    uint32_t block_index;
    uint32_t block_offset;
} lsm_index_entry_t;

typedef struct lsm_sparse_index {
    lsm_index_entry_t *entries;
    size_t count;
    size_t capacity;
} lsm_sparse_index_t;

typedef struct lsm_bloom_filter {
    uint64_t *bits;
    size_t bit_count;
    uint32_t hash_count;
} lsm_bloom_filter_t;

typedef struct lsm_sstable_meta {
    uint64_t id;
    uint64_t level;
    uint64_t num_keys;
    uint64_t file_size;
    lsm_slice_t smallest_key;
    lsm_slice_t largest_key;
    lsm_block_handle_t index_block;
    lsm_bloom_filter_t bloom;
    int fd;
    void *mmap_region;
    size_t mmap_size;
} lsm_sstable_meta_t;

/* =========================================================================
 * MANIFEST & COMPACTION
 * ========================================================================= */

typedef struct lsm_manifest_record {
    uint32_t type; /* 1: Add SST, 2: Delete SST */
    uint64_t level;
    uint64_t sst_id;
    char filename[256];
} lsm_manifest_record_t;

typedef struct lsm_level_state {
    lsm_sstable_meta_t **sstables;
    size_t count;
    size_t capacity;
    uint64_t total_size;
} lsm_level_state_t;

typedef struct lsm_version {
    lsm_level_state_t levels[LSM_MAX_LEVELS];
    intrusive_ref_count_t refs;
    struct lsm_version *next;
} lsm_version_t;

typedef struct lsm_version_set {
    _Atomic(lsm_version_t *) current;
    char base_dir[512];
    uint64_t next_file_number;
    int manifest_fd;
} lsm_version_set_t;

/* =========================================================================
 * WRITE-AHEAD LOG (WAL) & MPSC RING BUFFER
 * ========================================================================= */

typedef struct lsm_wal_header {
    uint64_t magic;
    uint32_t checksum;
    uint32_t length;
    uint64_t sequence;
    uint8_t op_type;
} __attribute__((packed)) lsm_wal_header_t;

typedef struct lsm_mpsc_node {
    _Atomic(struct lsm_mpsc_node *) next;
    lsm_op_type_t op;
    lsm_slice_t key;
    lsm_slice_t val;
    uint64_t sequence;
} lsm_mpsc_node_t;

typedef struct lsm_mpsc_queue {
    _Atomic(lsm_mpsc_node_t *) head;
    lsm_mpsc_node_t *tail;
    lsm_mpsc_node_t stub;
} lsm_mpsc_queue_t;

/* =========================================================================
 * ENGINE CONFIGURATION & CONTEXT
 * ========================================================================= */

typedef struct lsm_options {
    size_t arena_block_size;
    size_t memtable_size_limit;
    size_t block_cache_size;
    bool use_lz4_compression;
    uint32_t bloom_bits_per_key;
    uint32_t sparse_index_interval;
} lsm_options_t;

typedef struct lsm_engine {
    lsm_options_t options;
    char db_path[512];
    
    _Atomic(lsm_memtable_t *) mutable_memtable;
    _Atomic(lsm_memtable_t *) immutable_memtable;
    
    lsm_mpsc_queue_t write_queue;
    lsm_hp_domain_t hp_domain;
    lsm_version_set_t version_set;
    
    int wal_fd;
    uint64_t next_sequence;
    
    pthread_t wal_writer_thread;
    pthread_t compaction_thread;
    _Atomic(bool) shutdown_requested;
    
    pthread_mutex_t flush_mutex;
    pthread_cond_t flush_cond;
} lsm_engine_t;

/* =========================================================================
 * FUNCTION PROTOTYPES (API CONTRACT)
 * ========================================================================= */

/* Engine Lifecycle */
lsm_status_t lsm_open(const char *path, const lsm_options_t *options, lsm_engine_t **engine_out);
void lsm_close(lsm_engine_t *engine);

/* Data Operations */
lsm_status_t lsm_put(lsm_engine_t *engine, const lsm_slice_t *key, const lsm_slice_t *val);
lsm_status_t lsm_get(lsm_engine_t *engine, const lsm_slice_t *key, lsm_slice_t *val_out, lsm_arena_t *out_arena);
lsm_status_t lsm_delete(lsm_engine_t *engine, const lsm_slice_t *key);

/* Arena Allocator */
lsm_status_t lsm_arena_init(lsm_arena_t *arena, size_t chunk_size);
void* lsm_arena_alloc(lsm_arena_t *arena, size_t size);
void lsm_arena_destroy(lsm_arena_t *arena);

/* SkipList & MemTable */
lsm_status_t lsm_memtable_create(size_t max_height, lsm_memtable_t **mem_out);
void lsm_memtable_ref(lsm_memtable_t *mem);
void lsm_memtable_unref(lsm_memtable_t *mem);
bool lsm_memtable_insert(lsm_memtable_t *mem, const lsm_slice_t *key, const lsm_slice_t *val, uint8_t op, uint64_t seq);
bool lsm_memtable_get(lsm_memtable_t *mem, const lsm_slice_t *key, lsm_slice_t *val_out, uint8_t *op_out, lsm_hp_domain_t *domain);

/* Hazard Pointers */
void lsm_hp_init(lsm_hp_domain_t *domain);
void lsm_hp_destroy(lsm_hp_domain_t *domain);
void* lsm_hp_protect(lsm_hp_domain_t *domain, _Atomic(void *) *target_ptr, int hp_index);
void lsm_hp_unprotect(int hp_index);
void lsm_hp_retire(lsm_hp_domain_t *domain, void *ptr, void (*reclaimer)(void *));

/* SSTable & Bloom */
lsm_status_t lsm_bloom_init(lsm_bloom_filter_t *bloom, size_t num_keys, uint32_t bits_per_key);
void lsm_bloom_add(const lsm_bloom_filter_t *bloom, const void *key, size_t len);
bool lsm_bloom_check(const lsm_bloom_filter_t *bloom, const void *key, size_t len);
void lsm_bloom_destroy(lsm_bloom_filter_t *bloom);

lsm_status_t lsm_sstable_build(const char *filename, uint64_t sst_id, uint64_t level, 
                               lsm_skip_node_t *iter_head, const lsm_options_t *opts, 
                               lsm_sstable_meta_t *meta_out);
lsm_status_t lsm_sstable_open(const char *filename, lsm_sstable_meta_t *meta);
void lsm_sstable_close(lsm_sstable_meta_t *meta);
lsm_status_t lsm_sstable_get(const lsm_sstable_meta_t *meta, const lsm_slice_t *key, lsm_slice_t *val_out, lsm_arena_t *arena);

/* Utilities */
uint32_t lsm_crc32c(uint32_t crc, const void *buf, size_t len);
void lsm_murmurhash3_128(const void *key, int len, uint32_t seed, void *out);

#ifdef __cplusplus
}
#endif

#endif /* LSM_TYPES_H */