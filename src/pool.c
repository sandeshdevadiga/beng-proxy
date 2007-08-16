/*
 * Memory pool.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "pool.h"
#include "list.h"
#include "compiler.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) || defined(__PPC64__)
#define ALIGN 8
#define ALIGN_BITS 0x7
#else
#define ALIGN 4
#define ALIGN_BITS 0x3
#endif

#define RECYCLER_MAX_POOLS 256
#define RECYCLER_MAX_LINEAR_AREAS 256
#define RECYCLER_MAX_LINEAR_SIZE 65536

enum pool_type {
    POOL_LIBC,
    POOL_LINEAR,
};

struct libc_pool_chunk {
    struct libc_pool_chunk *next;
    unsigned char data[sizeof(size_t)];
};

struct linear_pool_area {
    struct linear_pool_area *prev;
    size_t size, used;
    unsigned char data[sizeof(size_t)];
};

struct pool {
    struct list_head siblings, children;
    pool_t parent;
    unsigned ref;
    int trashed;
    enum pool_type type;
    const char *name;
    union {
        struct libc_pool_chunk *libc;
        struct linear_pool_area *linear;
        struct pool *recycler;
    } current_area;
};

#ifndef NDEBUG
static LIST_HEAD(trash);
#endif

static struct {
    unsigned num_pools;
    pool_t pools;
    unsigned num_linear_areas;
    struct linear_pool_area *linear_areas;
} recycler;

static void * attr_malloc
xmalloc(size_t size)
{
    void *p = malloc(size);
    if (p == NULL) {
        fputs("Out of memory\n", stderr);
        abort();
    }
    return p;
}

static inline size_t
align_size(size_t size)
{
    return ((size - 1) | ALIGN_BITS) + 1;
}

void
pool_recycler_clear(void)
{
    pool_t pool;
    struct linear_pool_area *linear;

    while (recycler.pools != NULL) {
        pool = recycler.pools;
        recycler.pools = pool->current_area.recycler;
        free(pool);
    }

    recycler.num_pools = 0;

    while (recycler.linear_areas != NULL) {
        linear = recycler.linear_areas;
        recycler.linear_areas = linear->prev;
        free(linear);
    }

    recycler.num_linear_areas = 0;
}

static void
pool_recycler_put_linear(struct linear_pool_area *area)
{
    assert(area != 0);
    assert(area->size > 0);

    if (recycler.num_linear_areas < RECYCLER_MAX_LINEAR_AREAS &&
        area->size <= RECYCLER_MAX_LINEAR_SIZE) {
        area->prev = recycler.linear_areas;
        recycler.linear_areas = area;
        ++recycler.num_linear_areas;
    } else {
        free(area);
    }
}

static struct linear_pool_area *
pool_recycler_get_linear(size_t size)
{
    struct linear_pool_area **linear_p, *linear;

    assert(size > 0);

    for (linear_p = &recycler.linear_areas, linear = *linear_p;
         linear != NULL;
         linear_p = &linear->prev, linear = *linear_p) {
        if (linear->size == size) {
            assert(recycler.num_linear_areas > 0);
            --recycler.num_linear_areas;
            *linear_p = linear->prev;
            return linear;
        }
    }

    return NULL;
}

static inline void
pool_add_child(pool_t pool, pool_t child)
{
    assert(child->parent == NULL);

    child->parent = pool;
    list_add(&child->siblings, &pool->children);
}

static inline void
pool_remove_child(pool_t pool, pool_t child)
{
    (void)pool;

    assert(child->parent == pool);

    list_remove(&child->siblings);
    child->parent = NULL;
}

static pool_t attr_malloc
pool_new(pool_t parent, const char *name)
{
    pool_t pool;

    if (recycler.pools == NULL)
        pool = xmalloc(sizeof(*pool));
    else {
        pool = recycler.pools;
        recycler.pools = pool->current_area.recycler;
        --recycler.num_pools;
    }

    list_init(&pool->children);
    pool->ref = 1;
    pool->trashed = 0;
    pool->name = name;

    pool->parent = NULL;
    if (parent != NULL)
        pool_add_child(parent, pool);

    return pool;
}

pool_t attr_malloc
pool_new_libc(pool_t parent, const char *name)
{
    pool_t pool = pool_new(parent, name);
    pool->type = POOL_LIBC;
    pool->current_area.libc = NULL;
    return pool;
}

static struct linear_pool_area * attr_malloc
pool_new_linear_area(struct linear_pool_area *prev, size_t size)
{
    struct linear_pool_area *area = xmalloc(sizeof(*area) - sizeof(area->data) + size);
    if (area == NULL)
        abort();
    area->prev = prev;
    area->size = size;
    area->used = 0;
    return area;
}

static inline struct linear_pool_area *
pool_get_linear_area(struct linear_pool_area *prev, size_t size)
{
    struct linear_pool_area *area = pool_recycler_get_linear(size);
    if (area == NULL) {
        area = pool_new_linear_area(prev, size);
    } else {
        area->prev = prev;
        area->used = 0;
    }
    return area;
}

pool_t attr_malloc
pool_new_linear(pool_t parent, const char *name, size_t initial_size)
{
    pool_t pool = pool_new(parent, name);
    pool->type = POOL_LINEAR;

    pool->current_area.linear = pool_get_linear_area(NULL, initial_size);

    assert(parent != NULL);

    return pool;
}

static void
pool_destroy(pool_t pool)
{
    assert(pool->ref == 0);
    assert(pool->parent == NULL);

#ifndef NDEBUG
    if (pool->trashed)
        list_remove(&pool->siblings);

    while (!list_empty(&pool->children)) {
        pool_t child = (pool_t)pool->children.next;
        pool_remove_child(pool, child);
        assert(child->ref > 0);
        list_add(&child->siblings, &trash);
        child->trashed = 1;
    }
#endif

    switch (pool->type) {
    case POOL_LIBC:
        while (pool->current_area.libc != NULL) {
            struct libc_pool_chunk *chunk = pool->current_area.libc;
            pool->current_area.libc = chunk->next;
            free(chunk);
        }
        break;

    case POOL_LINEAR:
        while (pool->current_area.linear != NULL) {
            struct linear_pool_area *area = pool->current_area.linear;
            pool->current_area.linear = area->prev;
            pool_recycler_put_linear(area);
        }
        break;
    }

    if (recycler.num_pools < RECYCLER_MAX_POOLS) {
        pool->current_area.recycler = recycler.pools;
        recycler.pools = pool;
        ++recycler.num_pools;
    } else
        free(pool);
}

void
pool_ref(pool_t pool)
{
    assert(pool->ref > 0);
    ++pool->ref;

#ifdef POOL_TRACE_REF
    fprintf(stderr, "pool_ref('%s')=%u\n", pool->name, pool->ref);
#endif
}

unsigned
pool_unref(pool_t pool)
{
    assert(pool->ref > 0);
    --pool->ref;

#ifdef POOL_TRACE_REF
    fprintf(stderr, "pool_unref('%s')=%u\n", pool->name, pool->ref);
#endif

    if (pool->ref == 0) {
        if (pool->parent != NULL)
            pool_remove_child(pool->parent, pool);
        pool_destroy(pool);
        return 0;
    }

    return pool->ref;
}

#ifndef NDEBUG
void
pool_commit(void)
{
    assert(list_empty(&trash));
}
#endif

static void *
p_malloc_libc(pool_t pool, size_t size)
{
    struct libc_pool_chunk *chunk = xmalloc(sizeof(*chunk) - sizeof(chunk->data) + size);
    chunk->next = pool->current_area.libc;
    pool->current_area.libc = chunk;
    return chunk->data;
}

static void *
p_malloc_linear(pool_t pool, size_t size)
{
    struct linear_pool_area *area = pool->current_area.linear;
    void *p;

    if (area->used + size > area->size) {
        size_t new_area_size = area->size;
        fprintf(stderr, "growing linear pool '%s'\n", pool->name);
        if (size > new_area_size)
            new_area_size = ((size + new_area_size - 1) / new_area_size) * new_area_size;
        area = pool_get_linear_area(area, new_area_size);
        pool->current_area.linear = area;
    }

    p = area->data + area->used;
    area->used += size;

    return p;
}

static void *
internal_malloc(pool_t pool, size_t size)
{
    switch (pool->type) {
    case POOL_LIBC:
        return p_malloc_libc(pool, size);

    case POOL_LINEAR:
        return p_malloc_linear(pool, size);
    }

    assert(0);
    return NULL;
}

void *
p_malloc(pool_t pool, size_t size)
{
    return internal_malloc(pool, align_size(size));
}

static inline void
clear_memory(void *p, size_t size)
{
#if defined(__x86_64__)
    size_t n = (size + 7) / 8;
    size_t dummy0, dummy1;
    asm volatile("cld\n\t"
                 "rep stosq\n\t"
                 : "=&c"(dummy0), "=&D"(dummy1)
                 : "a"(0), "0"(n), "1"(p)
                 : "memory"
                 );
#else
    memset(p, 0, size);
#endif
}

void *
p_calloc(pool_t pool, size_t size)
{
    void *p = internal_malloc(pool, align_size(size));
    clear_memory(p, size);
    return p;
}

char *
p_strdup(pool_t pool, const char *src)
{
    size_t length = strlen(src) + 1;
    char *dest = p_malloc(pool, length);
    memcpy(dest, src, length);
    return dest;
}

char *
p_strndup(pool_t pool, const char *src, size_t length)
{
    char *dest = p_malloc(pool, length + 1);
    memcpy(dest, src, length);
    dest[length] = 0;
    return dest;
}
