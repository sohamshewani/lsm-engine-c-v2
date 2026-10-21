#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

#include "lsm.h"

#define NUM_OPERATIONS 100000
#define WARMUP_OPERATIONS 10000
#define KEY_SIZE 16
#define VAL_SIZE 64

static uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

int main(void) {
    const char *db_path = "./benchmark_lsm.db";
    
    // Cleanup any preexisting test database
    system("rm -rf ./benchmark_lsm.db");

    lsm_db *db = NULL;
    int rc = lsm_open(&db, db_path);
    if (rc != 0 || !db) {
        fprintf(stderr, "Failed to open lsm-engine-c database: rc=%d\n", rc);
        return 1;
    }

    uint64_t *latencies_put = malloc(NUM_OPERATIONS * sizeof(uint64_t));
    uint64_t *latencies_get = malloc(NUM_OPERATIONS * sizeof(uint64_t));
    if (!latencies_put || !latencies_get) {
        fprintf(stderr, "Memory allocation failed\n");
        lsm_close(db);
        return 1;
    }

    char keys[NUM_OPERATIONS][KEY_SIZE];
    char vals[NUM_OPERATIONS][VAL_SIZE];

    for (int i = 0; i < NUM_OPERATIONS; i++) {
        snprintf(keys[i], KEY_SIZE, "key%012d", i);
        snprintf(vals[i], VAL_SIZE, "val%012d", i);
    }

    // --- WARMUP PUT ---
    for (int i = 0; i < WARMUP_OPERATIONS; i++) {
        int idx = i % NUM_OPERATIONS;
        lsm_put(db, keys[idx], KEY_SIZE, vals[idx], VAL_SIZE);
    }

    // --- BENCHMARK: PUT ---
    uint64_t start_total_put = get_nanos();
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        uint64_t start_op = get_nanos();
        lsm_put(db, keys[i], KEY_SIZE, vals[i], VAL_SIZE);
        uint64_t end_op = get_nanos();
        latencies_put[i] = end_op - start_op;
    }
    uint64_t end_total_put = get_nanos();

    // --- BENCHMARK: GET ---
    uint64_t start_total_get = get_nanos();
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        char val_out[VAL_SIZE];
        size_t val_out_len = VAL_SIZE;
        uint64_t start_op = get_nanos();
        lsm_get(db, keys[i], KEY_SIZE, val_out, &val_out_len);
        uint64_t end_op = get_nanos();
        latencies_get[i] = end_op - start_op;
    }
    uint64_t end_total_get = get_nanos();

    // Compute metrics
    qsort(latencies_put, NUM_OPERATIONS, sizeof(uint64_t), compare_uint64);
    qsort(latencies_get, NUM_OPERATIONS, sizeof(uint64_t), compare_uint64);

    uint64_t put_total_ns = end_total_put - start_total_put;
    uint64_t get_total_ns = end_total_get - start_total_get;

    double put_ops_sec = ((double)NUM_OPERATIONS / (double)put_total_ns) * 1e9;
    double get_ops_sec = ((double)NUM_OPERATIONS / (double)get_total_ns) * 1e9;

    double put_p50 = (double)latencies_put[(int)(NUM_OPERATIONS * 0.50)] / 1000.0;
    double put_p99 = (double)latencies_put[(int)(NUM_OPERATIONS * 0.99)] / 1000.0;

    double get_p50 = (double)latencies_get[(int)(NUM_OPERATIONS * 0.50)] / 1000.0;
    double get_p99 = (double)latencies_get[(int)(NUM_OPERATIONS * 0.99)] / 1000.0;

    // Print Formatted Tabular Output
    printf("\n");
    printf("========================================================================\n");
    printf("                 LSM-ENGINE-C PERFORMANCE BENCHMARK                     \n");
    printf("========================================================================\n");
    printf(" Iterations : %d\n", NUM_OPERATIONS);
    printf(" Key Size   : %d bytes\n", KEY_SIZE);
    printf(" Value Size : %d bytes\n", VAL_SIZE);
    printf("------------------------------------------------------------------------\n");
    printf(" Operation  | Throughput (ops/sec) | p50 Latency (us) | p99 Latency (us)\n");
    printf("------------------------------------------------------------------------\n");
    printf(" PUT        | %19.2f | %16.2f | %16.2f\n", put_ops_sec, put_p50, put_p99);
    printf(" GET        | %19.2f | %16.2f | %16.2f\n", get_ops_sec, get_p50, get_p99);
    printf("========================================================================\n\n");

    // Cleanup
    lsm_close(db);
    free(latencies_put);
    free(latencies_get);
    system("rm -rf ./benchmark_lsm.db");

    return 0;
}