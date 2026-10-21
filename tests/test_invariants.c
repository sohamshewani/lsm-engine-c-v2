#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

/* Include the header for lsm-engine-c if available, otherwise declare stubs/types */
#include "lsm.h" /* Assuming standard header name */

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERTION FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define DB_PATH_PREFIX "/tmp/lsm_stress_test_"

typedef struct {
    lsm_db *db;
    int thread_id;
    int num_ops;
    int key_space_size;
    volatile int *stop_flag;
} thread_arg_t;

/* Helper to generate random string keys */
static void make_key(char *buf, size_t max_len, int id, int key_val) {
    snprintf(buf, max_len, "k_%d_%08x", id, key_val);
}

static void make_val(char *buf, size_t max_len, int key_val, size_t target_len) {
    size_t written = snprintf(buf, max_len, "v_%08x_", key_val);
    while (written < target_len && written < max_len - 1) {
        buf[written++] = (char)('a' + (key_val % 26));
    }
    buf[target_len < max_len ? target_len : max_len - 1] = '\0';
}

/* -------------------------------------------------------------------------
 * 1. UNIT & BOUNDARY TESTS
 * ------------------------------------------------------------------------- */
static void test_unit_boundaries(void) {
    printf("[-] Running Unit & Boundary Tests...\n");
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s_unit_%d", DB_PATH_PREFIX, getpid());
    
    // Clean up any stale files
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s*", db_path);
    int ret = system(cmd);
    (void)ret;

    lsm_db *db = NULL;
    int rc = lsm_open(db_path, &db);
    TEST_ASSERT(rc == LSM_OK);
    TEST_ASSERT(db != NULL);

    /* Edge Case: Empty Key / Empty Value */
    rc = lsm_put(db, "", 0, "val", 3);
    /* Depending on engine semantics, empty keys might be allowed or rejected. 
     * We assert it doesn't crash. */
    
    /* Edge Case: NULL inputs */
    TEST_ASSERT(lsm_put(NULL, "a", 1, "b", 1) != LSM_OK);
    TEST_ASSERT(lsm_get(db, NULL, 1, NULL, NULL) != LSM_OK);

    /* Normal Put & Get */
    const char *k1 = "hello";
    const char *v1 = "world";
    rc = lsm_put(db, k1, strlen(k1), v1, strlen(v1));
    TEST_ASSERT(rc == LSM_OK);

    char val_buf[128];
    memset(val_buf, 0, sizeof(val_buf));
    size_t val_len = sizeof(val_buf);
    rc = lsm_get(db, k1, strlen(k1), val_buf, &val_len);
    TEST_ASSERT(rc == LSM_OK);
    TEST_ASSERT(val_len == strlen(v1));
    TEST_ASSERT(memcmp(val_buf, v1, val_len) == 0);

    /* Overwrite / Update */
    const char *v2 = "universe_extended_version";
    rc = lsm_put(db, k1, strlen(k1), v2, strlen(v2));
    TEST_ASSERT(rc == LSM_OK);

    val_len = sizeof(val_buf);
    rc = lsm_get(db, k1, strlen(k1), val_buf, &val_len);
    TEST_ASSERT(rc == LSM_OK);
    TEST_ASSERT(val_len == strlen(v2));
    TEST_ASSERT(memcmp(val_buf, v2, val_len) == 0);

    /* Delete */
    rc = lsm_del(db, k1, strlen(k1));
    TEST_ASSERT(rc == LSM_OK);

    /* Get after Delete should fail or return NOT_FOUND */
    val_len = sizeof(val_buf);
    rc = lsm_get(db, k1, strlen(k1), val_buf, &val_len);
    TEST_ASSERT(rc != LSM_OK);

    /* Get Non-Existent */
    rc = lsm_get(db, "nonexistent_key_9999", 20, val_buf, &val_len);
    TEST_ASSERT(rc != LSM_OK);

    rc = lsm_close(db);
    TEST_ASSERT(rc == LSM_OK);
    printf("[+] Unit & Boundary Tests Passed.\n");
}

/* -------------------------------------------------------------------------
 * 2. INVARIANT TESTS (DATA INTEGRITY & LIFECYCLE)
 * ------------------------------------------------------------------------- */
static void test_invariants(void) {
    printf("[-] Running Invariant & Lifecycle Tests...\n");
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s_inv_%d", DB_PATH_PREFIX, getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s*", db_path);
    int ret = system(cmd);
    (void)ret;

    lsm_db *db = NULL;
    TEST_ASSERT(lsm_open(db_path, &db) == LSM_OK);

    int num_items = 5000;
    for (int i = 0; i < num_items; ++i) {
        char k[64], v[128];
        make_key(k, sizeof(k), 0, i);
        make_val(v, sizeof(v), i, 64);
        TEST_ASSERT(lsm_put(db, k, strlen(k), v, strlen(v)) == LSM_OK);
    }

    /* Verify all items present */
    for (int i = 0; i < num_items; ++i) {
        char k[64], expected_v[128], read_v[128];
        make_key(k, sizeof(k), 0, i);
        make_val(expected_v, sizeof(expected_v), i, 64);
        size_t vlen = sizeof(read_v);
        
        TEST_ASSERT(lsm_get(db, k, strlen(k), read_v, &vlen) == LSM_OK);
        TEST_ASSERT(vlen == strlen(expected_v));
        TEST_ASSERT(memcmp(read_v, expected_v, vlen) == 0);
    }

    /* Close and Reopen (Recovery/Persistence test) */
    TEST_ASSERT(lsm_close(db) == LSM_OK);

    TEST_ASSERT(lsm_open(db_path, &db) == LSM_OK);

    /* Verify all items still present after restart */
    for (int i = 0; i < num_items; ++i) {
        char k[64], expected_v[128], read_v[128];
        make_key(k, sizeof(k), 0, i);
        make_val(expected_v, sizeof(expected_v), i, 64);
        size_t vlen = sizeof(read_v);
        
        TEST_ASSERT(lsm_get(db, k, strlen(k), read_v, &vlen) == LSM_OK);
        TEST_ASSERT(vlen == strlen(expected_v));
        TEST_ASSERT(memcmp(read_v, expected_v, vlen) == 0);
    }

    TEST_ASSERT(lsm_close(db) == LSM_OK);
    printf("[+] Invariant & Lifecycle Tests Passed.\n");
}

/* -------------------------------------------------------------------------
 * 3. CONCURRENT STRESS TESTS (RACE CONDITION DETECTION)
 * ------------------------------------------------------------------------- */
static void *writer_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    for (int i = 0; i < targ->num_ops && !(*(targ->stop_flag)); ++i) {
        char k[64], v[128];
        int key_val = rand() % targ->key_space_size;
        make_key(k, sizeof(k), targ->thread_id, key_val);
        make_val(v, sizeof(v), key_val, 32);

        int rc = lsm_put(targ->db, k, strlen(k), v, strlen(v));
        /* Depending on design, concurrent puts should succeed */
        (void)rc;
    }
    return NULL;
}

static void *reader_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    char val_buf[128];
    for (int i = 0; i < targ->num_ops && !(*(targ->stop_flag)); ++i) {
        char k[64];
        int key_val = rand() % targ->key_space_size;
        make_key(k, sizeof(k), targ->thread_id, key_val);

        size_t vlen = sizeof(val_buf);
        int rc = lsm_get(targ->db, k, strlen(k), val_buf, &vlen);
        (void)rc;
    }
    return NULL;
}

static void *mixed_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    char val_buf[128];
    for (int i = 0; i < targ->num_ops && !(*(targ->stop_flag)); ++i) {
        char k[64], v[128];
        int key_val = rand() % targ->key_space_size;
        make_key(k, sizeof(k), targ->thread_id, key_val);

        if (i % 2 == 0) {
            make_val(v, sizeof(v), key_val, 32);
            lsm_put(targ->db, k, strlen(k), v, strlen(v));
        } else {
            size_t vlen = sizeof(val_buf);
            lsm_get(targ->db, k, strlen(k), val_buf, &vlen);
        }
    }
    return NULL;
}

static void test_concurrency(void) {
    printf("[-] Running Concurrent Stress Tests...\n");
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s_conc_%d", DB_PATH_PREFIX, getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s*", db_path);
    int ret = system(cmd);
    (void)ret;

    lsm_db *db = NULL;
    TEST_ASSERT(lsm_open(db_path, &db) == LSM_OK);

#define NUM_THREADS 8
    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];
    volatile int stop_flag = 0;

    int ops_per_thread = 2000;
    int key_space = 500;

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].db = db;
        args[i].thread_id = i % 2; /* Share key spaces to force contention */
        args[i].num_ops