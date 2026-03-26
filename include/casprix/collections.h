/*
 * Casprix Standard Library - High-Performance Collections
 * Backed by C runtime: nuwan_list_*, nuwan_map_*, nuwan_stack_*,
 *                      nuwan_queue_*, nuwan_pq_*
 */

#ifndef NUWAN_COLLECTIONS_H
#define NUWAN_COLLECTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque types (created on heap, managed by runtime) ---- */
typedef struct NuwanList  NuwanList;
typedef struct NuwanMap   NuwanMap;
typedef struct NuwanQueue NuwanQueue;
typedef struct NuwanPQ    NuwanPQ;
typedef NuwanList         NuwanStack;

/* ---- List: dynamic int64 array, cache-aligned ---- */
NuwanList* nuwan_list_new(void);
bool       nuwan_list_push(NuwanList* l, int64_t val);
int64_t    nuwan_list_get(const NuwanList* l, size_t idx);
bool       nuwan_list_set(NuwanList* l, size_t idx, int64_t val);
int64_t    nuwan_list_pop(NuwanList* l);
bool       nuwan_list_remove(NuwanList* l, size_t idx);
bool       nuwan_list_insert(NuwanList* l, size_t idx, int64_t val);
size_t     nuwan_list_size(const NuwanList* l);
bool       nuwan_list_empty(const NuwanList* l);
void       nuwan_list_clear(NuwanList* l);
void       nuwan_list_sort(NuwanList* l);           /* introsort + asm sort-net4 */
int64_t    nuwan_list_binary_search(const NuwanList* l, int64_t val);
int64_t    nuwan_list_sum(const NuwanList* l);      /* SSE2 vectorized sum */
void       nuwan_list_free(NuwanList* l);

/* ---- Map: Robin Hood hashmap, string key → int64 value ---- */
NuwanMap* nuwan_map_new(void);
void      nuwan_map_put(NuwanMap* m, const char* key, int64_t val);
int64_t   nuwan_map_get(const NuwanMap* m, const char* key);
bool      nuwan_map_has(const NuwanMap* m, const char* key);
bool      nuwan_map_remove(NuwanMap* m, const char* key);
size_t    nuwan_map_size(const NuwanMap* m);
bool      nuwan_map_empty(const NuwanMap* m);
void      nuwan_map_clear(NuwanMap* m);
void      nuwan_map_free(NuwanMap* m);

/* ---- Stack: LIFO, backed by NuwanList ---- */
NuwanStack* nuwan_stack_new(void);
bool        nuwan_stack_push(NuwanStack* s, int64_t v);
int64_t     nuwan_stack_pop(NuwanStack* s);
int64_t     nuwan_stack_peek(const NuwanStack* s);
size_t      nuwan_stack_size(const NuwanStack* s);
bool        nuwan_stack_empty(const NuwanStack* s);
void        nuwan_stack_clear(NuwanStack* s);
void        nuwan_stack_free(NuwanStack* s);

/* ---- Queue: O(1) ring-buffer FIFO ---- */
NuwanQueue* nuwan_queue_new(void);
bool        nuwan_queue_enqueue(NuwanQueue* q, int64_t val);
int64_t     nuwan_queue_dequeue(NuwanQueue* q);
int64_t     nuwan_queue_peek(const NuwanQueue* q);
size_t      nuwan_queue_size(const NuwanQueue* q);
bool        nuwan_queue_empty(const NuwanQueue* q);
void        nuwan_queue_clear(NuwanQueue* q);
void        nuwan_queue_free(NuwanQueue* q);

/* ---- Priority Queue: binary min-heap, dual parallel arrays ---- */
NuwanPQ* nuwan_pq_new(void);
bool     nuwan_pq_push(NuwanPQ* pq, int64_t priority, int64_t value);
int64_t  nuwan_pq_pop(NuwanPQ* pq);
int64_t  nuwan_pq_peek(const NuwanPQ* pq);
int64_t  nuwan_pq_peek_priority(const NuwanPQ* pq);
size_t   nuwan_pq_size(const NuwanPQ* pq);
bool     nuwan_pq_empty(const NuwanPQ* pq);
void     nuwan_pq_free(NuwanPQ* pq);

/* ---- ASM Kernel declarations (optional, for direct use) ---- */
extern uint64_t nuwan_string_hash(const char* str);
extern void     nuwan_array_sort_net4(int64_t* arr_of_4);
extern int64_t  nuwan_array_sum_i64(const int64_t* arr, size_t n);
extern size_t   nuwan_strlen_asm(const char* str);
extern void*    nuwan_memcopy(void* dst, const void* src, size_t n);
extern void*    nuwan_memset_fast(void* ptr, int val, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NUWAN_COLLECTIONS_H */
