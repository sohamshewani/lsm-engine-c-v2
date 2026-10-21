#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define WAL_MAGIC 0x4C534D57 // "LSMW"
#define WAL_VERSION 1
#define WAL_HEADER_SIZE 16
#define WAL_RECORD_HEADER_SIZE 13

typedef enum {
    WAL_OP_PUT = 1,
    WAL_OP_DELETE = 2
} wal_op_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t sequence;
    uint32_t checksum;
} __attribute__((packed)) wal_file_header_t;

typedef struct {
    uint32_t crc32;
    uint32_t length;
    uint8_t type;
    uint32_t sequence;
} __attribute__((packed)) wal_record_header_t;

struct wal {
    int fd;
    char *filepath;
    pthread_mutex_t mutex;
    uint32_t current_sequence;
    off_t file_size;
};

static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

struct wal *wal_open(const char *filepath) {
    struct wal *w = (struct wal *)malloc(sizeof(struct wal));
    if (!w) return NULL;

    w->filepath = strdup(filepath);
    if (!w->filepath) {
        free(w);
        return NULL;
    }

    if (pthread_mutex_init(&w->mutex, NULL) != 0) {
        free(w->filepath);
        free(w);
        return NULL;
    }

    w->fd = open(filepath, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (w->fd < 0) {
        pthread_mutex_destroy(&w->mutex);
        free(w->filepath);
        free(w);
        return NULL;
    }

    struct stat st;
    if (fstat(w->fd, &st) < 0) {
        close(w->fd);
        pthread_mutex_destroy(&w->mutex);
        free(w->filepath);
        free(w);
        return NULL;
    }

    w->file_size = st.st_size;
    w->current_sequence = 0;

    if (w->file_size == 0) {
        wal_file_header_t header;
        header.magic = WAL_MAGIC;
        header.version = WAL_VERSION;
        header.flags = 0;
        header.sequence = 0;
        header.checksum = calculate_crc32((const uint8_t *)&header, 12);

        if (write(w->fd, &header, WAL_HEADER_SIZE) != WAL_HEADER_SIZE) {
            close(w->fd);
            pthread_mutex_destroy(&w->mutex);
            free(w->filepath);
            free(w);
            return NULL;
        }
        if (fdatasync(w->fd) < 0) {
            close(w->fd);
            pthread_mutex_destroy(&w->mutex);
            free(w->filepath);
            free(w);
            return NULL;
        }
        w->file_size = WAL_HEADER_SIZE;
    }

    return w;
}

int wal_append(struct wal *w, wal_op_t type, const uint8_t *key, uint32_t key_len, const uint8_t *value, uint32_t val_len) {
    if (!w || !key || (type == WAL_OP_PUT && !value)) return -EINVAL;

    pthread_mutex_lock(&w->mutex);

    w->current_sequence++;
    uint32_t payload_len = 4 + key_len + 4 + val_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        pthread_mutex_unlock(&w->mutex);
        return -ENOMEM;
    }

    uint32_t k_len_be = key_len;
    uint32_t v_len_be = val_len;
    
    memcpy(payload, &k_len_be, 4);
    memcpy(payload + 4, key, key_len);
    memcpy(payload + 4 + key_len, &v_len_be, 4);
    memcpy(payload + 8 + key_len, value, val_len);

    wal_record_header_t rec_header;
    rec_header.type = (uint8_t)type;
    rec_header.sequence = w->current_sequence;
    rec_header.length = payload_len;
    
    uint32_t crc_data_len = 1 + 4 + 4 + payload_len;
    uint8_t *crc_buf = (uint8_t *)malloc(crc_data_len);
    if (!crc_buf) {
        free(payload);
        pthread_mutex_unlock(&w->mutex);
        return -ENOMEM;
    }

    crc_buf[0] = rec_header.type;
    memcpy(crc_buf + 1, &rec_header.sequence, 4);
    memcpy(crc_buf + 5, &rec_header.length, 4);
    memcpy(crc_buf + 9, payload, payload_len);

    rec_header.crc32 = calculate_crc32(crc_buf, crc_data_len);
    free(crc_buf);

    size_t total_record_size = WAL_RECORD_HEADER_SIZE + payload_len;
    uint8_t *write_buf = (uint8_t *)malloc(total_record_size);
    if (!write_buf) {
        free(payload);
        pthread_mutex_unlock(&w->mutex);
        return -ENOMEM;
    }

    memcpy(write_buf, &rec_header, WAL_RECORD_HEADER_SIZE);
    memcpy(write_buf + WAL_RECORD_HEADER_SIZE, payload, payload_len);
    free(payload);

    ssize_t written = write(w->fd, write_buf, total_record_size);
    free(write_buf);

    if (written != (ssize_t)total_record_size) {
        pthread_mutex_unlock(&w->mutex);
        return -EIO;
    }

    if (fdatasync(w->fd) < 0) {
        pthread_mutex_unlock(&w->mutex);
        return -EIO;
    }

    w->file_size += total_record_size;
    pthread_mutex_unlock(&w->mutex);

    return 0;
}

int wal_recover(struct wal *w, int (*callback)(wal_op_t type, uint32_t seq, const uint8_t *key, uint32_t key_len, const uint8_t *value, uint32_t val_len)) {
    if (!w || !callback) return -EINVAL;

    pthread_mutex_lock(&w->mutex);

    if (lseek(w->fd, 0, SEEK_SET) < 0) {
        pthread_mutex_unlock(&w->mutex);
        return -EIO;
    }

    wal_file_header_t fheader;
    if (read(w->fd, &fheader, WAL_HEADER_SIZE) != WAL_HEADER_SIZE) {
        pthread_mutex_unlock(&w->mutex);
        return -EIO;
    }

    if (fheader.magic != WAL_MAGIC || fheader.version != WAL_VERSION) {
        pthread_mutex_unlock(&w->mutex);
        return -EINVAL;
    }

    off_t current_offset = WAL_HEADER_SIZE;
    int ret = 0;

    while (current_offset < w->file_size) {
        wal_record_header_t rheader;
        ssize_t r = read(w->fd, &rheader, WAL_RECORD_HEADER_SIZE);
        if (r == 0) {
            break;
        }
        if (r < WAL_RECORD_HEADER_SIZE) {
            // Truncated tail record, stop recovery successfully
            break;
        }

        uint8_t *payload = (uint8_t *)malloc(rheader.length);
        if (!payload) {
            ret = -ENOMEM;
            break;
        }

        ssize_t pr = read(w->fd, payload, rheader.length);
        if (pr != (ssize_t)rheader.length) {
            free(payload);
            // Truncated tail payload, stop recovery successfully
            break;
        }

        uint32_t crc_data_len = 1 + 4 + 4 + rheader.length;
        uint8_t *crc_buf = (uint8_t *)malloc(crc_data_len);
        if (!crc_buf) {
            free(payload);
            ret = -ENOMEM;
            break;
        }

        crc_buf[0] = rheader.type;
        memcpy(crc_buf + 1, &rheader.sequence, 4);
        memcpy(crc_buf + 5, &rheader.length, 4);
        memcpy(crc_buf + 9, payload, rheader.length);

        uint32_t calculated_crc = calculate_crc32(crc_buf, crc_data_len);
        free(crc_buf);

        if (calculated_crc != rheader.crc32) {
            free(payload);
            // Corrupted tail record, stop recovery
            break;
        }

        if (rheader.length < 8) {
            free(payload);
            ret = -EINVAL;
            break;
        }

        uint32_t key_len, val_len;
        memcpy(&key_len, payload, 4);
        if (4 + key_len + 4 > rheader.length) {
            free(payload);
            ret = -EINVAL;
            break;
        }

        uint8_t *key = payload + 4;
        memcpy(&val_len, payload + 4 + key_len, 4);
        if (4 + key_len + 4 + val_len != rheader.length) {
            free(payload);
            ret = -EINVAL;
            break;
        }

        uint8_t *value = payload + 4 + key_len + 4;

        ret = callback((wal_op_t)rheader.type, rheader.sequence, key, key_len, value, val_len);
        
        if (rheader.sequence > w->current_sequence) {
            w->current_sequence = rheader.sequence;
        }

        current_offset += WAL_RECORD_HEADER_SIZE + rheader.length;
        free(payload);

        if (ret != 0) {
            break;
        }
    }

    if (ftruncate(w->fd, current_offset) < 0) {
        ret = -EIO;
    } else {
        w->file_size = current_offset;
    }

    if (lseek(w->fd, 0, SEEK_END) < 0) {
        ret = -EIO;
    }

    pthread_mutex_unlock(&w->mutex);
    return ret;
}

void wal_close(struct wal *w) {
    if (!w) return;

    pthread_mutex_lock(&w->mutex);
    if (w->fd >= 0) {
        close(w->fd);
    }
    pthread_mutex_unlock(&w->mutex);

    pthread_mutex_destroy(&w->mutex);
    free(w->filepath);
    free(w);
}