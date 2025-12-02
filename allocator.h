#ifndef TS_ALLOCATOR_H
#define TS_ALLOCATOR_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  ts_heap_init(size_t bytes);
void* ts_malloc(size_t size);
void  ts_free(void* ptr);
void* ts_realloc(void* ptr, size_t n);
void* ts_calloc(size_t nmemb, size_t size);
void  ts_heap_stats(size_t* out_total, size_t* out_free, size_t* out_largest_free);

#ifdef __cplusplus
}
#endif

#endif
