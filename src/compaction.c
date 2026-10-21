#include "compaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

#define MAX_LEVELS 7
#define MAX_SSTABLES_PER_LEVEL 64
#define BLOCK_SIZE 4096
#define TARGET_FILE_SIZE (2 * 1024 * 1024) // 2MB per SSTable

struct SSTableMeta {
    uint32_t file_id;
    uint32_t level;
    uint64_t size_bytes;
    uint8_t smallest_key[256];
    uint16_t smallest_key_len;
    uint8_t largest_key[256];
    uint16_t largest_key_len;
};

struct SSTableList {
    struct SSTableMeta sstables[MAX_SSTABLES_PER_LEVEL];
    int count;
};

struct CompactionManifest {
    pthread_rwlock_t rwlock;
    struct SSTableList levels[MAX_LEVELS];
    uint32_t next_file_id;
    char db_path[512];
};

struct CompactionWorkerContext {
    pthread_t thread_id;
    struct CompactionManifest* manifest;
    volatile bool running;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool wakeup_requested;
};

static struct CompactionWorkerContext g_compaction_worker;

static int compare_keys(const uint8_t* a, uint16_t alen, const uint8_t* b, uint16_t blen) {
    uint16_t min_len = (alen < blen) ? alen : blen;
    int cmp = memcmp(a, b, min_len);
    if (cmp != 0) return cmp;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

static bool keys_overlap(const uint8_t* s1, uint16_t l1, const uint8_t* e1, uint16_t le1,
                         const uint8_t* s2, uint16_t l2, const uint8_t* e2, uint16_t le2) {
    if (compare_keys(e1, le1, s2, l2) < 0) return false;
    if (compare_keys(e2, le2, s1, l1) < 0) return false;
    return true;
}

struct CompactionManifest* compaction_manifest_init(const char* db_path) {
    struct CompactionManifest* manifest = (struct CompactionManifest*)malloc(sizeof(struct CompactionManifest));
    if (!manifest) return NULL;

    if (pthread_rwlock_init(&manifest->rwlock, NULL) != 0) {
        free(manifest);
        return NULL;
    }

    for (int i = 0; i < MAX_LEVELS; i++) {
        manifest->levels[i].count = 0;
    }
    manifest->next_file_id = 1;
    snprintf(manifest->db_path, sizeof(manifest->db_path), "%s", db_path);

    return manifest;
}

void compaction_manifest_destroy(struct CompactionManifest* manifest) {
    if (!manifest) return;
    pthread_rwlock_destroy(&manifest->rwlock);
    free(manifest);
}

int compaction_add_sstable(struct CompactionManifest* manifest, uint32_t level, const struct SSTableMeta* meta) {
    if (!manifest || level >= MAX_LEVELS) return -EINVAL;

    pthread_rwlock_wrlock(&manifest->rwlock);
    struct SSTableList* lvl = &manifest->levels[level];
    if (lvl->count >= MAX_SSTABLES_PER_LEVEL) {
        pthread_rwlock_unlock(&manifest->rwlock);
        return -ENOSPC;
    }

    lvl->sstables[lvl->count] = *meta;
    lvl->sstables[lvl->count].level = level;
    lvl->count++;

    // Sort SSTables by smallest key ascending
    for (int i = lvl->count - 1; i > 0; i--) {
        if (compare_keys(lvl->sstables[i].smallest_key, lvl->sstables[i].smallest_key_len,
                         lvl->sstables[i-1].smallest_key, lvl->sstables[i-1].smallest_key_len) < 0) {
            struct SSTableMeta tmp = lvl->sstables[i];
            lvl->sstables[i] = lvl->sstables[i-1];
            lvl->sstables[i-1] = tmp;
        } else {
            break;
        }
    }

    pthread_rwlock_unlock(&manifest->rwlock);
    return 0;
}

static uint64_t calculate_level_size(const struct SSTableList* lvl) {
    uint64_t total = 0;
    for (int i = 0; i < lvl->count; i++) {
        total += lvl->sstables[i].size_bytes;
    }
    return total;
}

static int select_compaction_job(struct CompactionManifest* manifest, int* out_level, 
                                 struct SSTableMeta* inputs_l1, int* count_l1,
                                 struct SSTableMeta* inputs_l2, int* count_l2) {
    pthread_rwlock_rdlock(&manifest->rwlock);

    // Check L0 size or count threshold (Size-Tiered for L0)
    if (manifest->levels[0].count >= 4) {
        *out_level = 0;
        *count_l1 = manifest->levels[0].count;
        for (int i = 0; i < *count_l1; i++) {
            inputs_l1[i] = manifest->levels[0].sstables[i];
        }
        *count_l2 = 0;
        // Also pick overlapping L1 tables if L1 exists
        if (manifest->levels[1].count > 0) {
            // Find overall min and max key across L0 inputs
            uint8_t min_k[256], max_k[256];
            uint16_t min_l = 0, max_l = 0;
            bool first = true;
            for (int i = 0; i < *count_l1; i++) {
                if (first) {
                    memcpy(min_k, inputs_l1[i].smallest_key, inputs_l1[i].smallest_key_len);
                    min_l = inputs_l1[i].smallest_key_len;
                    memcpy(max_k, inputs_l1[i].largest_key, inputs_l1[i].largest_key_len);
                    max_l = inputs_l1[i].largest_key_len;
                    first = false;
                } else {
                    if (compare_keys(inputs_l1[i].smallest_key, inputs_l1[i].smallest_key_len, min_k, min_l) < 0) {
                        memcpy(min_k, inputs_l1[i].smallest_key, inputs_l1[i].smallest_key_len);
                        min_l = inputs_l1[i].smallest_key_len;
                    }
                    if (compare_keys(inputs_l1[i].largest_key, inputs_l1[i].largest_key_len, max_k, max_l) > 0) {
                        memcpy(max_k, inputs_l1[i].largest_key, inputs_l1[i].largest_key_len);
                        max_l = inputs_l1[i].largest_key_len;
                    }
                }
            }

            int l2_matched = 0;
            for (int j = 0; j < manifest->levels[1].count; j++) {
                if (keys_overlap(min_k, min_l, max_k, max_l,
                                 manifest->levels[1].sstables[j].smallest_key,
                                 manifest->levels[1].sstables[j].smallest_key_len,
                                 manifest->levels[1].sstables[j].largest_key,
                                 manifest->levels[1].sstables[j].largest_key_len)) {
                    inputs_l2[l2_matched++] = manifest->levels[1].sstables[j];
                    if (l2_matched >= MAX_SSTABLES_PER_LEVEL) break;
                }
            }
            *count_l2 = l2_matched;
        }

        pthread_rwlock_unlock(&manifest->rwlock);
        return 0;
    }

    // Check Leveled compaction for L1-LN
    for (int lvl = 1; lvl < MAX_LEVELS - 1; lvl++) {
        uint64_t max_bytes = (uint64_t)(10ULL * 1024ULL * 1024ULL * (1ULL << (lvl - 1)));
        uint64_t current_bytes = calculate_level_size(&manifest->levels[lvl]);
        if (current_bytes > max_bytes && manifest->levels[lvl].count > 0) {
            *out_level = lvl;
            // Pick the first SSTable from this level
            inputs_l1[0] = manifest->levels[lvl].sstables[0];
            *count_l1 = 1;

            // Find overlapping SSTables in level + 1
            int l2_matched = 0;
            for (int j = 0; j < manifest->levels[lvl + 1].count; j++) {
                if (keys_overlap(inputs_l1[0].smallest_key, inputs_l1[0].smallest_key_len,
                                 inputs_l1[0].largest_key, inputs_l1[0].largest_key_len,
                                 manifest->levels[lvl+1].sstables[j].smallest_key,
                                 manifest->levels[lvl+1].sstables[j].smallest_key_len,
                                 manifest->levels[lvl+1].sstables[j].largest_key,
                                 manifest->levels[lvl+1].sstables[j].largest_key_len)) {
                    inputs_l2[l2_matched++] = manifest->levels[lvl+1].sstables[j];
                    if (l2_matched >= MAX_SSTABLES_PER_LEVEL) break;
                }
            }
            // If no overlap in LN+1, pick the first table to maintain progress
            if (l2_matched == 0 && manifest->levels[lvl + 1].count > 0) {
                inputs_l2[0] = manifest->levels[lvl + 1].sstables[0];
                l2_matched = 1;
            }
            *count_l2 = l2_matched;

            pthread_rwlock_unlock(&manifest->rwlock);
            return 0;
        }
    }

    pthread_rwlock_unlock(&manifest->rwlock);
    return -ENOENT;
}

static int execute_compaction(struct CompactionManifest* manifest, int level,
                              const struct SSTableMeta* inputs_l1, int count_l1,
                              const struct SSTableMeta* inputs_l2, int count_l2) {
    // Simulate merge-sort compaction producing new SSTables at target level + 1 (or level if L0->L1)
    uint32_t target_level = (level == 0) ? 1 : level + 1;
    if (target_level >= MAX_LEVELS) target_level = MAX_LEVELS - 1;

    char filename[1024];
    pthread_rwlock_wrlock(&manifest->rwlock);
    uint32_t new_file_id = manifest->next_file_id++;
    pthread_rwlock_unlock(&manifest->rwlock);

    snprintf(filename, sizeof(filename), "%s/sst_%u_%u.sst", manifest->db_path, target_level, new_file_id);
    
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -errno;

    // Write a dummy block representing merged output
    uint8_t dummy_block[BLOCK_SIZE];
    memset(dummy_block, 0, BLOCK_SIZE);
    ssize_t written = write(fd, dummy_block, BLOCK_SIZE);
    fsync(fd);
    close(fd);

    if (written != BLOCK_SIZE) {
        unlink(filename);
        return -EIO;
    }

    struct SSTableMeta new_meta;
    new_meta.file_id = new_file_id;
    new_meta.level = target_level;
    new_meta.size_bytes = BLOCK_SIZE;
    
    if (count_l1 > 0) {
        memcpy(new_meta.smallest_key, inputs_l1[0].smallest_key, inputs_l1[0].smallest_key_len);
        new_meta.smallest_key_len = inputs_l1[0].smallest_key_len;
    } else {
        new_meta.smallest_key_len = 0;
    }

    if (count_l2 > 0) {
        memcpy(new_meta.largest_key, inputs_l2[count_l2 - 1].largest_key, inputs_l2[count_l2 - 1].largest_key_len);
        new_meta.largest_key_len = inputs_l2[count_l2 - 1].largest_key_len;
    } else if (count_l1 > 0) {
        memcpy(new_meta.largest_key, inputs_l1[count_l1 - 1].largest_key, inputs_l1[count_l1 - 1].largest_key_len);
        new_meta.largest_key_len = inputs_l1[count_l1 - 1].largest_key_len;
    } else {
        new_meta.largest_key_len = 0;
    }

    // RCU Swap: Remove old inputs and insert new metadata atomically under write lock
    pthread_rwlock_wrlock(&manifest->rwlock);

    // Remove inputs from level
    struct SSTableList* lvl1 = &manifest->levels[level];
    for (int i = 0; i < count_l1; i++) {
        for (int j = 0; j < lvl1->count; j++) {
            if (lvl1->sstables[j].file_id == inputs_l1[i].file_id) {
                for (int k = j; k < lvl1->count - 1; k++) {
                    lvl1->sstables[k] = lvl1->sstables[k+1];
                }
                lvl1->count--;
                break;
            }
        }
    }

    // Remove inputs from level + 1 (or target level)
    struct SSTableList* lvl2 = &manifest->levels[target_level];
    for (int i = 0; i < count_l2; i++) {
        for (int j = 0; j < lvl2->count; j++) {
            if (lvl2->sstables[j].file_id == inputs_l2[i].file_id) {
                for (int k = j; k < lvl2->count - 1; k++) {
                    lvl2->sstables[k] = lvl2->sstables[k+1];
                }
                lvl2->count--;
                break;
            }
        }
    }

    // Insert new SSTable
    if (lvl2->count < MAX_SSTABLES_PER_LEVEL) {
        lvl2->sstables[lvl2->count] = new_meta;
        lvl2->count++;
        // Sort level
        for (int i = lvl2->count - 1; i > 0; i--) {
            if (compare_keys(lvl2->sstables[i].smallest_key, lvl2->sstables[i].smallest_