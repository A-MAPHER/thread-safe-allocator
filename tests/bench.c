#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <time.h>
#include "allocator.h"

typedef enum { OP_ALLOC=0, OP_FREE=1 } op_t;

typedef struct {
    op_t op;
    size_t size;
    size_t slot;
} job_t;

typedef struct {
    job_t* buf;
    size_t cap, head, tail;
    pthread_mutex_t mu;
    sem_t slots_free;
    sem_t slots_used;
} queue_t;

static void q_init(queue_t* q, size_t cap) {
    q->buf = malloc(cap*sizeof(job_t));
    q->cap = cap; q->head = q->tail = 0;
    pthread_mutex_init(&q->mu, NULL);
    sem_init(&q->slots_free, 0, cap);
    sem_init(&q->slots_used, 0, 0);
}
static void q_push(queue_t* q, job_t j) {
    sem_wait(&q->slots_free);
    pthread_mutex_lock(&q->mu);
    q->buf[q->tail] = j;
    q->tail = (q->tail + 1) % q->cap;
    pthread_mutex_unlock(&q->mu);
    sem_post(&q->slots_used);
}
static job_t q_pop(queue_t* q) {
    sem_wait(&q->slots_used);
    pthread_mutex_lock(&q->mu);
    job_t j = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    pthread_mutex_unlock(&q->mu);
    sem_post(&q->slots_free);
    return j;
}

typedef struct {
    queue_t* q;
    void** slots;
    size_t nslots;
    unsigned int seed;
    size_t jobs;
} prod_args_t;

typedef struct {
    queue_t* q;
    void** slots;
    size_t nslots;
    size_t processed;
} cons_args_t;

static size_t rand_size(unsigned int* seed) {
    int r = rand_r(seed) & 1023;
    if (r < 700) return 16 + (rand_r(seed) % 112);
    if (r < 950) return 128 + (rand_r(seed) % (32*1024));
    return 33*1024 + (rand_r(seed) % (64*1024));
}

static void* producer(void* vp) {
    prod_args_t* a = (prod_args_t*)vp;
    for (size_t i=0;i<a->jobs;i++) {
        size_t idx = rand_r(&a->seed) % a->nslots;
        job_t j;
        if (a->slots[idx]) {
            j.op = OP_FREE; j.slot = idx; j.size = 0;
            a->slots[idx] = (void*)(((uintptr_t)a->slots[idx]) | 1ULL);
        } else {
            j.op = OP_ALLOC; j.slot = idx; j.size = rand_size(&a->seed);
        }
        q_push(a->q, j);
    }
    return NULL;
}

static void* consumer(void* vp) {
    cons_args_t* a = (cons_args_t*)vp;
    for (;;) {
        job_t j = q_pop(a->q);
        if (j.op == OP_ALLOC) {
            void* p = ts_malloc(j.size);
            a->slots[j.slot] = p;
            a->processed++;
        } else if (j.op == OP_FREE) {
            void* p = a->slots[j.slot];
            if (p && (((uintptr_t)p) & 1ULL) == 0) {
                ts_free(p);
                a->slots[j.slot] = NULL;
                a->processed++;
            } else if (p) {
                a->slots[j.slot] = (void*)(((uintptr_t)p) & ~1ULL);
            }
        } else {
            break;
        }
    }
    return NULL;
}

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec/1e9;
}

int main(int argc, char** argv) {
    int nprod   = (argc > 1) ? atoi(argv[1]) : 4;
    int ncons   = (argc > 2) ? atoi(argv[2]) : 4;
    size_t heap_mb = (argc > 3) ? (size_t)atol(argv[3]) : 64;
    size_t jobs_per_prod = (argc > 4) ? (size_t)atol(argv[4]) : 250000;

    printf("Producer-Consumer bench: producers=%d consumers=%d heap=%zu MiB jobs/prod=%zu\n",
           nprod, ncons, heap_mb, jobs_per_prod);

    ts_heap_init(heap_mb << 20);

    queue_t q; q_init(&q, 1<<16);
    size_t nslots = 1<<15;
    void** slots = calloc(nslots, sizeof(void*));

    pthread_t* P = calloc(nprod, sizeof(*P));
    pthread_t* C = calloc(ncons, sizeof(*C));
    prod_args_t* pa = calloc(nprod, sizeof(*pa));
    cons_args_t* ca = calloc(ncons, sizeof(*ca));

    for (int i=0;i<nprod;i++) {
        pa[i].q = &q; pa[i].slots = slots; pa[i].nslots = nslots;
        pa[i].jobs = jobs_per_prod; pa[i].seed = 0xBEEF + i*777;
        pthread_create(&P[i], NULL, producer, &pa[i]);
    }
    for (int i=0;i<ncons;i++) {
        ca[i].q = &q; ca[i].slots = slots; ca[i].nslots = nslots;
        ca[i].processed = 0;
        pthread_create(&C[i], NULL, consumer, &ca[i]);
    }

    double t0 = now_sec();
    for (int i=0;i<nprod;i++) pthread_join(P[i], NULL);
    for (int i=0;i<ncons;i++) { job_t j = {.op=(op_t)2,.size=0,.slot=0}; q_push(&q,j); }
    for (int i=0;i<ncons;i++) pthread_join(C[i], NULL);
    double t1 = now_sec();

    size_t processed = 0;
    for (int i=0;i<ncons;i++) processed += ca[i].processed;
    printf("Processed jobs: %zu in %.3fs  =>  %.2f Mops/s\n",
           processed, t1 - t0, (processed / (t1 - t0)) / 1e6);

    for (size_t i=0;i<nslots;i++) if (slots[i] && (((uintptr_t)slots[i]) & 1ULL)==0) ts_free(slots[i]);
    size_t total=0, freeb=0, big=0; ts_heap_stats(&total,&freeb,&big);
    printf("Heap total=%zu free=%zu largest_free=%zu\n", total, freeb, big);

    free(slots); free(P); free(C); free(pa); free(ca);
    return 0;
}
