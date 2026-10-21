#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>

#define MAX_LEVELS 32
#define MAX_HAZARD_POINTERS 8
#define CACHE_LINE_SIZE 64

typedef enum {
    MEMTABLE_SUCCESS = 0,
    MEMTABLE_ERR_NOMEM = -1,
    MEMTABLE_ERR_NOT_FOUND = -2,
    MEMTABLE_ERR_CAS_FAILED = -3,
    MEMTABLE_ERR_FULL = -4
} memtable_status_t;

typedef struct {
    uint8_t* data;
    size_t len;
} slice_t;

typedef struct {
    slice_t key;
    slice_t value;
    uint64_t sequence;
    uint8_t type; // 0 = Put, 1 = Delete
} memtable_entry_t;

struct skip_list_node;

typedef struct {
    _Atomic(struct skip_list_node*) next;
} skip_list_level_t;

typedef struct skip_list_node {
    memtable_entry_t entry;
    int height;
    _Atomic(bool) fully_linked;
    skip_list_level_t levels[1];
} skip_list_node_t;

typedef struct arena_chunk {
    struct arena_chunk* next;
    size_t capacity;
    size_t used;
    uint8_t data[];
} arena_chunk_t;

typedef struct {
    arena_chunk_t* current;
    size_t default_chunk_size;
    pthread_mutex_t lock;
} arena_t;

typedef struct {
    _Atomic(skip_list_node_t*) head;
    _Atomic(size_t) current_size;
    size_t capacity_limit;
    arena_t arena;
    int max_height;
} memtable_t;

typedef struct {
    _Atomic(skip_list_node_t*) pointers[MAX_HAZARD_POINTERS];
} hazard_record_t;

// Global or thread-local hazard pointer registry mockup for lock-free safety
static pthread_key_t hazard_key;
static pthread_once_t hazard_key_once = PTHREAD_ONCE_INIT;

static void hazard_destructor(void* arg) {
    if (arg) {
        free(arg);
    }
}

static void init_hazard_key(void) {
    pthread_key_create(&hazard_key, hazard_destructor);
}

static hazard_record_t* get_hazard_record(void) {
    pthread_once(&hazard_key_once, init_hazard_key);
    hazard_record_t* rec = (hazard_record_t*)pthread_getspecific(hazard_key);
    if (!rec) {
        rec = (hazard_record_t*)calloc(1, sizeof(hazard_record_t));
        if (rec) {
            for (int i = 0; i < MAX_HAZARD_POINTERS; i++) {
                atomic_init(&rec->pointers[i], NULL);
            }
            pthread_setspecific(hazard_key, rec);
        }
    }
    return rec;
}

arena_t* arena_create(size_t default_chunk_size) {
    arena_t* arena = (arena_t*)malloc(sizeof(arena_t));
    if (!arena) return NULL;
    
    arena->default_chunk_size = default_chunk_size > 0 ? default_chunk_size : 65536;
    pthread_mutex_init(&arena->lock, NULL);
    
    arena_chunk_t* chunk = (arena_chunk_t*)malloc(sizeof(arena_chunk_t) + arena->default_chunk_size);
    if (!chunk) {
        pthread_mutex_destroy(&arena->lock);
        free(arena);
        return NULL;
    }
    chunk->next = NULL;
    chunk->capacity = arena->default_chunk_size;
    chunk->used = 0;
    arena->current = chunk;
    
    return arena;
}

void arena_destroy(arena_t* arena) {
    if (!arena) return;
    pthread_mutex_lock(&arena->lock);
    arena_chunk_t* chunk = arena->current;
    while (chunk) {
        arena_chunk_t* next = chunk->next;
        free(chunk);
        chunk = next;
    }
    pthread_mutex_unlock(&arena->lock);
    pthread_mutex_destroy(&arena->lock);
    free(arena);
}

void* arena_alloc(arena_t* arena, size_t size) {
    if (size == 0) return NULL;
    // Align to 8 bytes
    size_t aligned_size = (size + 7) & ~((size_t)7);
    
    pthread_mutex_lock(&arena->lock);
    
    arena_chunk_t* curr = arena->current;
    if (curr->used + aligned_size <= curr->capacity) {
        void* ptr = &curr->data[curr->used];
        curr->used += aligned_size;
        pthread_mutex_unlock(&arena->lock);
        return ptr;
    }
    
    size_t alloc_size = arena->default_chunk_size;
    if (aligned_size > alloc_size) {
        alloc_size = aligned_size;
    }
    
    arena_chunk_t* new_chunk = (arena_chunk_t*)malloc(sizeof(arena_chunk_t) + alloc_size);
    if (!new_chunk) {
        pthread_mutex_unlock(&arena->lock);
        return NULL;
    }
    new_chunk->next = arena->current;
    new_chunk->capacity = alloc_size;
    new_chunk->used = aligned_size;
    arena->current = new_chunk;
    
    void* ptr = &new_chunk->data[0];
    pthread_mutex_unlock(&arena->lock);
    return ptr;
}

static int random_height(int max_height) {
    int height = 1;
    // Simple fast bitwise random generator for level distribution (p = 0.25)
    static uint32_t seed = 2463534242UL;
    seed = seed * 1103515245UL + 12345UL;
    while ((seed & 3) == 0 && height < max_height) {
        height++;
        seed = seed * 1103515245UL + 12345UL;
    }
    return height;
}

static int compare_slices(const slice_t* a, const slice_t* b) {
    size_t min_len = a->len < b->len ? a->len : b->len;
    int cmp = memcmp(a->data, b->data, min_len);
    if (cmp != 0) return cmp;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

static skip_list_node_t* create_node(arena_t* arena, const slice_t* key, const slice_t* value, uint64_t seq, uint8_t type, int height) {
    size_t total_size = sizeof(skip_list_node_t) + sizeof(skip_list_level_t) * (height - 1);
    skip_list_node_t* node = (skip_list_node_t*)arena_alloc(arena, total_size);
    if (!node) return NULL;
    
    uint8_t* key_mem = (uint8_t*)arena_alloc(arena, key->len);
    uint8_t* val_mem = (uint8_t*)arena_alloc(arena, value->len);
    if (!key_mem || !val_mem) return NULL;
    
    memcpy(key_mem, key->data, key->len);
    memcpy(val_mem, value->data, value->len);
    
    node->entry.key.data = key_mem;
    node->entry.key.len = key->len;
    node->entry.value.data = val_mem;
    node->entry.value.len = value->len;
    node->entry.sequence = seq;
    node->entry.type = type;
    node->height = height;
    atomic_init(&node->fully_linked, false);
    
    for (int i = 0; i < height; i++) {
        atomic_init(&node->levels[i].next, NULL);
    }
    
    return node;
}

memtable_t* memtable_create(size_t capacity_limit) {
    memtable_t* mt = (memtable_t*)malloc(sizeof(memtable_t));
    if (!mt) return NULL;
    
    mt->max_height = MAX_LEVELS;
    arena_t* arena = arena_create(1024 * 1024); // 1MB default chunks
    if (!arena) {
        free(mt);
        return NULL;
    }
    mt->arena = *arena;
    free(arena); // copied struct value
    
    slice_t dummy_key = {NULL, 0};
    slice_t dummy_val = {NULL, 0};
    skip_list_node_t* head = create_node(&mt->arena, &dummy_key, &dummy_val, 0, 0, mt->max_height);
    if (!head) {
        arena_destroy(&mt->arena);
        free(mt);
        return NULL;
    }
    atomic_init(&head->fully_linked, true);
    
    atomic_init(&mt->head, head);
    atomic_init(&mt->current_size, 0);
    mt->capacity_limit = capacity_limit;
    
    return mt;
}

void memtable_destroy(memtable_t* mt) {
    if (!mt) return;
    arena_destroy(&mt->arena);
    free(mt);
}

memtable_status_t memtable_insert(memtable_t* mt, const slice_t* key, const slice_t* value, uint64_t sequence, uint8_t type) {
    size_t est_size = key->len + value->len + sizeof(skip_list_node_t);
    size_t current = atomic_load(&mt->current_size);
    if (mt->capacity_limit > 0 && current + est_size > mt->capacity_limit) {
        return MEMTABLE_ERR_FULL;
    }
    
    int height = random_height(mt->max_height);
    skip_list_node_t* new_node = create_node(&mt->arena, key, value, sequence, type, height);
    if (!new_node) return MEMTABLE_ERR_NOMEM;
    
    hazard_record_t* hz = get_hazard_record();
    skip_list_node_t* preds[MAX_LEVELS];
    skip_list_node_t* succs[MAX_LEVELS];
    
    while (true) {
        skip_list_node_t* pred = NULL;
        skip_list_node_t* curr = NULL;
        skip_list_node_t* succ = NULL;
        
        int found = -1;
        
        pred = atomic_load(&mt->head);
        atomic_store(&hz->pointers[0], pred);
        
        for (int level = mt->max_height - 1; level >= 0; level--) {
            curr = atomic_load(&pred->levels[level].next);
            atomic_store(&hz->pointers[1], curr);
            
            // Re-check head consistency
            if (pred != atomic_load(&mt->head)) {
                // Retry traversal from start due to race
                break;
            }
            
            while (true) {
                if (curr == NULL) {
                    succ = NULL;
                } else {
                    succ = atomic_load(&curr->levels[level].next);
                    atomic_store(&hz->pointers[2], succ);
                }
                
                // Check if curr is marked for deletion or unlinked
                if (curr != NULL && !atomic_load(&curr->fully_linked)) {
                    // Help unlink or just break to restart
                    break;
                }
                
                if (curr != NULL) {
                    int cmp = compare_slices(&curr->entry.key, key);
                    if (cmp < 0) {
                        pred = curr;
                        atomic_store(&hz->pointers[0], pred);
                        curr = succ;
                        atomic_store(&hz->pointers[1], curr);
                        continue;
                    } else if (cmp == 0) {
                        found = level;
                    }
                }
                break;
            }
            
            preds[level] = pred;
            succs[level] = curr;
        }
        
        if (found != -1) {
            // Key already exists, update value/sequence via CAS or replace
            // For standard LSM, we append duplicate keys with higher sequence numbers.
            // Here we fall through and execute insert to allow multiple MVCC versions.
        }
        
        // Attempt linking bottom-up
        int stitched_levels = 0;
        for (int level = 0; level < height; level++) {
            skip_list_node_t* succ = succs[level];
            skip_list_node_t* pred = preds[level];
            
            atomic_store(&new_node->levels[level].next, succ);
            
            if (!atomic_compare_exchange_weak(&pred->levels[level].next, &succ, new_node)) {
                // CAS failed, back out and retry
                goto RETRY_INSERT;
            }
            stitched_levels++;
        }
        
        atomic_store(&new_node->fully_linked, true);
        atomic_fetch_add(&mt->current_size, est_size);
        
        // Clear hazard pointers
        atomic_store(&hz->pointers[0], NULL);
        atomic_store(&hz->pointers[1], NULL);
        atomic_store(&hz->pointers[2], NULL);
        
        return MEMTABLE_SUCCESS;
        
    RETRY_INSERT:
        continue;
    }
}

memtable_status_t memtable_get(memtable_t* mt, const slice_t* key, slice_t* out_value, uint8_t* out_type, uint64_t* out_seq) {
    hazard_record_t* hz = get_hazard_record();
    skip_list_node_t* curr = NULL;
    skip_list_node_t* pred = atomic_load(&mt->head);
    atomic_store(&hz->pointers[0], pred);
    
    memtable_status_t status = MEMTABLE_ERR_NOT_FOUND;
    uint64_t max_seq = 0;
    
    for (int level = mt->max_height - 1; level >= 0; level--) {
        curr = atomic_load(&pred->levels[level].next);
        atomic_store(&hz->pointers[1], curr);
        
        while (curr != NULL) {
            skip_list_node_t* succ = atomic_load(&curr->levels[level].next);
            atomic_store(&hz->pointers[2], succ);
            
            int cmp = compare_slices(&curr->entry.key, key);
            if (cmp < 0) {
                pred = curr;
                atomic_store(&hz->pointers[0], pred);
                curr = succ;
                atomic_store(&hz->pointers[1], curr);
            } else {
                break;
            }
        }
    }
    
    // Scan matching keys horizontally at level 0
    curr = atomic_load(&pred->levels[0].next);
    atomic_store(&hz->pointers[1], curr);
    
    while (curr != NULL) {
        int cmp = compare_slices(&curr->entry.key, key);
        if (cmp == 0) {
            if (atomic_load(&curr->fully_linked)) {
                if (curr->entry.sequence >= max_seq) {
                    max_seq = curr->entry.sequence;
                    *out_value = curr->entry.value;