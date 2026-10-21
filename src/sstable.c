#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <byteswap.h>

#include "sstable.h"

#define SSTABLE_BLOCK_SIZE 4096
#define SSTABLE_MAGIC 0x535354425BL // "SSTBL"
#define SSTABLE_VERSION 1

/* CRC32-C (Castagnoli) implementation using standard table-lookup or hardware */
static uint32_t crc32c_table[256];
static int crc32c_initialized = 0;

static void crc32c_init(void) {
    uint32_t polynomial = 0x82F63B78;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc = crc >> 1;
            }
        }
        crc32c_table[i] = crc;
    }
    crc32c_initialized = 1;
}

uint32_t sstable_crc32c(uint32_t crc, const uint8_t *buf, size_t len) {
    if (!crc32c_initialized) {
        crc32c_init();
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32c_table[(crc ^ buf[i]) & 0xFF];
    }
    return ~crc;
}

/* Varint encoding and decoding */
int sstable_encode_varint32(uint32_t val, uint8_t *dst) {
    int count = 0;
    while (val >= 0x80) {
        dst[count++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    dst[count++] = (uint8_t)(val & 0x7F);
    return count;
}

int sstable_decode_varint32(const uint8_t *src, size_t max_len, uint32_t *val) {
    uint32_t result = 0;
    size_t shift = 0;
    size_t count = 0;
    while (count < max_len) {
        uint8_t byte = src[count];
        if (shift >= 32) {
            return -1; /* Overflow */
        }
        result |= ((uint32_t)(byte & 0x7F)) << shift;
        count++;
        if (!(byte & 0x80)) {
            *val = result;
            return (int)count;
        }
        shift += 7;
    }
    return -1; /* Underrun */
}

int sstable_encode_varint64(uint64_t val, uint8_t *dst) {
    int count = 0;
    while (val >= 0x80) {
        dst[count++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    dst[count++] = (uint8_t)(val & 0x7F);
    return count;
}

int sstable_decode_varint64(const uint8_t *src, size_t max_len, uint64_t *val) {
    uint64_t result = 0;
    size_t shift = 0;
    size_t count = 0;
    while (count < max_len) {
        uint8_t byte = src[count];
        if (shift >= 64) {
            return -1; /* Overflow */
        }
        result |= ((uint64_t)(byte & 0x7F)) << shift;
        count++;
        if (!(byte & 0x80)) {
            *val = result;
            return (int)count;
        }
        shift += 7;
    }
    return -1; /* Underrun */
}

/* Bloom Filter Implementation */
struct sstable_bloom_filter {
    uint8_t *bits;
    size_t byte_size;
    uint32_t num_hashes;
};

/* MurmurHash3 32-bit variant */
static uint32_t murmur3_32(const void *key, size_t len, uint32_t seed) {
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = (int)(len / 4);
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
    };

    h1 ^= (uint32_t)len;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;

    return h1;
}

sstable_bloom_t *sstable_bloom_create(size_t num_entries, double false_positive_rate) {
    if (num_entries == 0) num_entries = 1;
    if (false_positive_rate <= 0.0 || false_positive_rate >= 1.0) {
        false_positive_rate = 0.01;
    }
    
    /* m = - (n * ln(p)) / (ln(2)^2) */
    double ln2 = 0.69314718056;
    size_t bit_size = (size_t)(-((double)num_entries * log(false_positive_rate)) / (ln2 * ln2));
    if (bit_size < 64) bit_size = 64;
    
    size_t byte_size = (bit_size + 7) / 8;
    
    /* k = (m / n) * ln(2) */
    uint32_t num_hashes = (uint32_t)(((double)bit_size / (double)num_entries) * ln2 + 0.5);
    if (num_hashes < 1) num_hashes = 1;
    if (num_hashes > 30) num_hashes = 30;

    sstable_bloom_t *bloom = (sstable_bloom_t *)malloc(sizeof(sstable_bloom_t));
    if (!bloom) return NULL;

    bloom->byte_size = byte_size;
    bloom->num_hashes = num_hashes;
    bloom->bits = (uint8_t *)calloc(1, byte_size);
    if (!bloom->bits) {
        free(bloom);
        return NULL;
    }

    return bloom;
}

void sstable_bloom_destroy(sstable_bloom_t *bloom) {
    if (bloom) {
        free(bloom->bits);
        free(bloom);
    }
}

void sstable_bloom_add(sstable_bloom_t *bloom, const uint8_t *key, size_t key_len) {
    if (!bloom || !bloom->bits) return;
    uint32_t h1 = murmur3_32(key, key_len, 0x9747b28c);
    uint32_t h2 = murmur3_32(key, key_len, h1);
    size_t total_bits = bloom->byte_size * 8;

    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint32_t combined = h1 + i * h2;
        size_t bit_pos = combined % total_bits;
        bloom->bits[bit_pos / 8] |= (1 << (bit_pos % 8));
    }
}

bool sstable_bloom_check(const sstable_bloom_t *bloom, const uint8_t *key, size_t key_len) {
    if (!bloom || !bloom->bits || bloom->byte_size == 0) return true;
    uint32_t h1 = murmur3_32(key, key_len, 0x9747b28c);
    uint32_t h2 = murmur3_32(key, key_len, h1);
    size_t total_bits = bloom->byte_size * 8;

    for (uint32_t i = 0; i < bloom->num_hashes; i++) {
        uint32_t combined = h1 + i * h2;
        size_t bit_pos = combined % total_bits;
        if (!(bloom->bits[bit_pos / 8] & (1 << (bit_pos % 8)))) {
            return false;
        }
    }
    return true;
}

int sstable_bloom_serialize(const sstable_bloom_t *bloom, uint8_t *dst, size_t max_len, size_t *written) {
    if (!bloom || !dst) return -EINVAL;
    size_t needed = sizeof(uint32_t) + sizeof(uint32_t) + bloom->byte_size;
    if (max_len < needed) return -ENOSPC;

    uint8_t *ptr = dst;
    *(uint32_t *)ptr = bloom->byte_size;
    ptr += sizeof(uint32_t);
    *(uint32_t *)ptr = bloom->num_hashes;
    ptr += sizeof(uint32_t);
    memcpy(ptr, bloom->bits, bloom->byte_size);
    ptr += bloom->byte_size;

    *written = (size_t)(ptr - dst);
    return 0;
}

sstable_bloom_t *sstable_bloom_deserialize(const uint8_t *src, size_t len, size_t *read_bytes) {
    if (!src || len < sizeof(uint32_t) * 2) return NULL;

    const uint8_t *ptr = src;
    uint32_t byte_size = *(const uint32_t *)ptr;
    ptr += sizeof(uint32_t);
    uint32_t num_hashes = *(const uint32_t *)ptr;
    ptr += sizeof(uint32_t);

    if (len < sizeof(uint32_t) * 2 + byte_size) return NULL;

    sstable_bloom_t *bloom = (sstable_bloom_t *)malloc(sizeof(sstable_bloom_t));
    if (!bloom) return NULL;

    bloom->byte_size = byte_size;
    bloom->num_hashes = num_hashes;
    bloom->bits = (uint8_t *)malloc(byte_size);
    if (!bloom->bits) {
        free(bloom);
        return NULL;
    }
    memcpy(bloom->bits, ptr, byte_size);
    ptr += byte_size;

    *read_bytes = (size_t)(ptr - src);
    return bloom;
}

/* SSTable Builder Implementation */
struct sstable_builder {
    int fd;
    char *filepath;
    uint32_t options;
    
    /* Current Block */
    uint8_t block_buf[SSTABLE_BLOCK_SIZE];
    uint32_t block_offset;
    uint32_t block_entries_count;
    
    /* Last Key for internal sorting and sparse index */
    uint8_t *last_key;
    size_t last_key_len;

    /* Sparse Index data */
    uint32_t index_interval;
    uint32_t current_entry_index;
    
    struct index_entry {
        uint8_t *key;
        size_t key_len;
        uint64_t file_offset;
        uint32_t block_size;
    } *index_entries;
    size_t index_count;
    size_t index_capacity;

    /* General stats */
    uint64_t current_file_offset;
    uint64_t num_keys;
    
    /* Bloom filter tracking */
    sstable_bloom_t *bloom;
    size_t estimated_keys;
};

sstable_builder_t *sstable_builder_create(const char *filepath, uint32_t options, size_t estimated_keys) {
    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return NULL;

    sstable_builder_t *builder = (sstable_builder_t *)calloc(1, sizeof(sstable_builder_t));
    if (!builder) {
        close(fd);
        return NULL;
    }

    builder->fd = fd;
    builder->filepath = strdup(filepath);
    builder->options = options;
    builder->block_offset = 0;
    builder->block_entries_count = 0;
    builder->current_file_offset = 0;
    builder->index_interval = 16; /* Map every 16th key */
    builder->estimated_keys = estimated_keys > 0 ? estimated_keys : 10000;
    
    builder->bloom = sstable_bloom_create(builder->estimated_keys, 0.01);
    if (!builder->bloom) {
        free(builder->filepath);
        free(builder);
        close(fd);
        return NULL;
    }

    builder->index_capacity = 128;
    builder->index_entries = (struct index_entry *)malloc(builder->index_capacity * sizeof(struct index_entry));
    if (!builder->index_entries) {
        sstable_bloom_destroy(builder->bloom);
        free(builder->filepath);
        free(builder);
        close(fd);
        return NULL;
    }

    return builder;
}

static int sstable_builder_flush_block(sstable_builder_t *builder) {
    if (builder->block_offset == 0) return 0;

    /* Compute CRC32-C over the block payload */
    uint32_t crc = sstable_crc32c(0, builder->block_buf, builder->block_offset);
    
    /* Write block data */
    ssize_t written = write(builder->fd, builder->block_buf, builder->block_offset);
    if (written < 0 || (size_t)written != builder->block_offset) {
        return -EIO;
    }

    /* Write block trailer (CRC