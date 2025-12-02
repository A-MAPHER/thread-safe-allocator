#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "allocator.h"

// --- Constants & Macros ---
#define ALIGN      16UL
#define ALIGN_UP(x) (((x) + (ALIGN-1)) & ~(ALIGN-1))
#define MIN_SPLIT  64UL 

// Segregated List Constants
#define NUM_BUCKETS 10

typedef struct block {
    size_t size;
    struct block* prev_free;
    struct block* next_free;
} block_t;

// --- Helper Functions ---
static inline size_t pack(size_t sz, bool used) { return used ? (sz | 1ULL) : (sz & ~1ULL); }
static inline bool   is_used(size_t sz)         { return (sz & 1ULL) != 0; }
static inline size_t bsize(size_t sz)           { return sz & ~1ULL; }
static inline uint8_t* footer_ptr(block_t* b)   { return ((uint8_t*)b) + bsize(b->size) - sizeof(size_t); }
static inline void write_footer(block_t* b)     { *(size_t*)footer_ptr(b) = b->size; }

// --- Global State ---
static uint8_t* g_heap = NULL;
static size_t   g_heap_sz = 0;
// Replaced single head with an array of heads
static block_t* g_free_heads[NUM_BUCKETS] = { NULL }; 
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

// Buckets (divided blocks)
static int get_bucket_index(size_t sz) {
    if (sz < 64)    return 0;
    if (sz < 128)   return 1;
    if (sz < 256)   return 2;
    if (sz < 512)   return 3;
    if (sz < 1024)  return 4;
    if (sz < 2048)  return 5;
    if (sz < 4096)  return 6;
    if (sz < 8192)  return 7;
    if (sz < 16384) return 8;
    return 9; // Large blocks
}

static void freelist_insert(block_t* b) {
    int idx = get_bucket_index(bsize(b->size));
    
    b->prev_free = NULL;
    b->next_free = g_free_heads[idx];
    
    if (g_free_heads[idx]) {
        g_free_heads[idx]->prev_free = b;
    }
    g_free_heads[idx] = b;
}

static void freelist_remove(block_t* b) {
    if (b->prev_free) {
        b->prev_free->next_free = b->next_free;
    } else {
        int idx = get_bucket_index(bsize(b->size));
        g_free_heads[idx] = b->next_free;
    }

    if (b->next_free) {
        b->next_free->prev_free = b->prev_free;
    }

    b->prev_free = b->next_free = NULL;
}

// --- Heap Management ---
void ts_heap_init(size_t bytes) {
    pthread_mutex_lock(&g_heap_lock);
    if (g_heap) { pthread_mutex_unlock(&g_heap_lock); return; }
    
    if (bytes == 0) bytes = (16UL << 20); // 16 MB default
    bytes = ALIGN_UP(bytes);
    
    void* mem = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        g_heap = NULL;
        pthread_mutex_unlock(&g_heap_lock);
        return;
    }
    g_heap = (uint8_t*)mem;
    g_heap_sz = bytes;

    block_t* b = (block_t*)g_heap;
    b->size = pack(bytes, false);
    write_footer(b);
    freelist_insert(b);
    pthread_mutex_unlock(&g_heap_lock);
}

static block_t* split_block(block_t* b, size_t need) {
    size_t bsz = bsize(b->size);
    
    freelist_remove(b);

    if (bsz >= need + MIN_SPLIT) {
        uint8_t* base = (uint8_t*)b;
        block_t* rem  = (block_t*)(base + need);
        rem->size = pack(bsz - need, false);
        write_footer(rem);
        
        freelist_insert(rem);

        b->size = pack(need, true);
        write_footer(b);
        return b;
    } else {
        b->size = pack(bsz, true);
        write_footer(b);
        return b;
    }
}

void* ts_malloc(size_t size) {
    if (!g_heap) ts_heap_init(16UL << 20);
    if (size == 0) size = 1;
    size = ALIGN_UP(size);
    size_t need = size + ALIGN_UP(sizeof(block_t)) + sizeof(size_t);
    need = ALIGN_UP(need);

    pthread_mutex_lock(&g_heap_lock);
    
    int start_idx = get_bucket_index(need);
    for (int i = start_idx; i < NUM_BUCKETS; i++) {
        for (block_t* cur = g_free_heads[i]; cur; cur = cur->next_free) {
            if (bsize(cur->size) >= need) {
                block_t* used = split_block(cur, need);
                pthread_mutex_unlock(&g_heap_lock);
                return (uint8_t*)used + ALIGN_UP(sizeof(block_t));
            }
        }
    }

    pthread_mutex_unlock(&g_heap_lock);
    return NULL;
}

static block_t* ptr_to_block(void* p) {
    return (block_t*)((uint8_t*)p - ALIGN_UP(sizeof(block_t)));
}

static block_t* coalesce(block_t* b) {
    uint8_t* base = (uint8_t*)b;
    
    // Merge Right
    uint8_t* nextp = base + bsize(b->size);
    if (nextp < g_heap + g_heap_sz) {
        block_t* nb = (block_t*)nextp;
        if (!is_used(nb->size)) {
            freelist_remove(nb); 
            b->size = pack(bsize(b->size) + bsize(nb->size), false);
            write_footer(b);
        }
    }
    
    // Merge Left
    if (base > g_heap) {
        size_t prev_sz = *(size_t*)(base - sizeof(size_t));
        block_t* pb = (block_t*)(base - bsize(prev_sz));
        if (!is_used(pb->size)) {
            freelist_remove(pb);
            pb->size = pack(bsize(pb->size) + bsize(b->size), false);
            write_footer(pb);
            b = pb;
        }
    }
    return b;
}

void ts_free(void* ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&g_heap_lock);
    
    block_t* b = ptr_to_block(ptr);
    
    if (!is_used(b->size)) {
        pthread_mutex_unlock(&g_heap_lock);
        return;
    }

    b->size = pack(bsize(b->size), false);
    write_footer(b);
    
    b = coalesce(b);
    
    freelist_insert(b);
    
    pthread_mutex_unlock(&g_heap_lock);
}

void* ts_realloc(void* p, size_t n) {
    if (!p) return ts_malloc(n);
    if (n == 0) { ts_free(p); return NULL; }
    void* q = ts_malloc(n);
    if (!q) return NULL;
    block_t* b = ptr_to_block(p);
    size_t old_payload = bsize(b->size) - (ALIGN_UP(sizeof(block_t)) + sizeof(size_t));
    memcpy(q, p, old_payload < n ? old_payload : n);
    ts_free(p);
    return q;
}

void* ts_calloc(size_t nmemb, size_t sz) {
    size_t total = nmemb * sz;
    void* p = ts_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void ts_heap_stats(size_t* tot, size_t* fre, size_t* big) {
    pthread_mutex_lock(&g_heap_lock);
    if (tot) *tot = g_heap_sz;
    size_t free_total = 0, largest = 0;
    
    for(int i = 0; i < NUM_BUCKETS; i++) {
        for (block_t* cur = g_free_heads[i]; cur; cur = cur->next_free) {
            size_t s = bsize(cur->size);
            free_total += s;
            if (s > largest) largest = s;
        }
    }
    
    if (fre) *fre = free_total;
    if (big) *big = largest;
    pthread_mutex_unlock(&g_heap_lock);
}
