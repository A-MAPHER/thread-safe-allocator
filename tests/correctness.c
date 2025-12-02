#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include "allocator.h"

typedef struct {
    void** slots;
    size_t nslots;
    unsigned int seed;
    size_t ops;
} worker_args_t;

static size_t rand_size(unsigned int* seed) {
    int r = rand_r(seed) & 1023;
    if (r < 700) return 16 + (rand_r(seed) % 112);        // mostly small
    if (r < 950) return 128 + (rand_r(seed) % (32*1024)); // medium
    return 33*1024 + (rand_r(seed) % (64*1024));          // occasionally big
}

static void* worker(void* vp) {
    worker_args_t* a = (worker_args_t*)vp;
    for (size_t i = 0; i < a->ops; i++) {
        size_t idx = rand_r(&a->seed) % a->nslots;
        if (a->slots[idx]) {
            ts_free(a->slots[idx]);
            a->slots[idx] = NULL;
        } else {
            size_t sz = rand_size(&a->seed);
            void* p = ts_malloc(sz);
            if (p) {
                memset(p, 0xA5, sz < 64 ? sz : 64);
                a->slots[idx] = p;
            }
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 8;
    size_t heap_mb = (argc > 2) ? (size_t)atol(argv[2]) : 32;
    size_t ops_per_thread = (argc > 3) ? (size_t)atol(argv[3]) : 200000;
    printf("Correctness & stress: threads=%d, heap=%zu MiB, ops/thread=%zu\n",
           nthreads, heap_mb, ops_per_thread);

    ts_heap_init(heap_mb << 20);

    pthread_t* th = calloc(nthreads, sizeof(*th));
    worker_args_t* args = calloc(nthreads, sizeof(*args));
    for (int t = 0; t < nthreads; t++) {
        args[t].nslots = 8192;
        args[t].slots = calloc(args[t].nslots, sizeof(void*));
        args[t].seed = 0xC0FFEE + t * 1337;
        args[t].ops = ops_per_thread;
        pthread_create(&th[t], NULL, worker, &args[t]);
    }
    for (int t = 0; t < nthreads; t++) pthread_join(th[t], NULL);

    for (int t = 0; t < nthreads; t++) {
        for (size_t i = 0; i < args[t].nslots; i++) if (args[t].slots[i]) ts_free(args[t].slots[i]);
        free(args[t].slots);
    }
    free(args); free(th);

    size_t total=0, freeb=0, big=0;
    ts_heap_stats(&total, &freeb, &big);
    printf("Heap total=%zu, free=%zu, largest_free=%zu\n", total, freeb, big);

    if (freeb != total || big != total) {
        fprintf(stderr, "ERROR: fragmentation/leak detected (free=%zu total=%zu big=%zu)\n", freeb, total, big);
        return 2;
    }
    printf("OK: allocator returned to a single free block.\n");
    return 0;
}
