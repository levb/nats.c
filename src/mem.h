// Copyright 2015-2018 The NATS Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MEM_H_
#define MEM_H_

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// GNU C Library version 2.25 or later.
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
#define HAVE_EXPLICIT_BZERO 1
#endif

// Newlib
#if defined(__NEWLIB__)
#define HAVE_EXPLICIT_BZERO 1
#endif

// FreeBSD version 11.0 or later.
#if defined(__FreeBSD__) && __FreeBSD_version >= 1100037
#define HAVE_EXPLICIT_BZERO 1
#endif

// OpenBSD version 5.5 or later.
#if defined(__OpenBSD__) && OpenBSD >= 201405
#define HAVE_EXPLICIT_BZERO 1
#endif

// NetBSD version 7.2 or later.
#if defined(__NetBSD__) && __NetBSD_Version__ >= 702000000
#define HAVE_EXPLICIT_MEMSET 1
#endif

//----------------------------------------------------------------------------
//  natsPool - memory pool.
//
//  - Uses a linked lists of natsSmall for small memory allocations. Each
//    heap-allocated chunk is 1-page NATS_DEFAULT_MEM_PAGE_SIZE) sized.
//  - Maximum small allocation size is NATS_DEFAULT_MEM_PAGE_SIZE -
//    sizeof(natsSmall).
//  - Uses a linked list of natsLarge for large HEAP memory allocations. The
//    list elements are allocated in the (small) pool.
//  - natsPool_Destroy() will free all memory allocated in the pool.
//
//  +->+---------------+ 0       +------------->+---------------+
//  |  | natsSmall     |         |              | natsSmall     |
//  |  |  - next       |---------+              |  - next       |
//  |  |---------------+                        |---------------|
//  |  | natsPool      |                        |               |
//  +--|  - small      |                        |               |
//     |               |                        | used          |
//     |  - large      |---                     | memory        |
//     |               |                        |               |
//     | (most recent) |                        |               |
//     |---------------|                        |---------------| len
//     | used          |                        | free          |
//     | memory        |                        | memory        |
//     |               |                        |               |
//     |               |                        |               |
//  +->+---------------|                        |               |
//  |  | natsLarge #1  |                        |               |
//  |  |  - prev       |                        |               |
//  |  |  - mem  ======|=> HEAP                 |               |
//  |  |---------------|                        |               |
//  |  | natsLarge #2  |                        |               |
//  +--|  - prev       |                        |               |
//     |  - mem  ======|=> HEAP                 |               |
//     |---------------|                        |               |
//     | more          |                        |               |
//     | used          |                        |               |
//     | memory        |                        |               |
//     | ...           |                        |               |
//     |---------------| len                    |               |
//     | free          |                        |               |
//     | memory        |                        |               |
//     |               |                        |               |
//     +---------------+ page size              +---------------+ page size

void natsPool_setPageSize(size_t size); // for testing

#define NATS_DEFAULT_POOL_PAGE_SIZE 155
#define NATS_DEFAULT_BUFFER_SIZE 256

struct __natsSmall_s
{
    struct __natsSmall_s *next;
    size_t len;
};

struct __natsLarge_s
{
    struct __natsLarge_s *prev;
    uint8_t *mem;
};

struct __natsPool_s
{
    // Each small is allocated of pageSize.
    size_t pageSize;

    // small head is the first chunk allocated since that is where we attempt to
    // allocate first.
    natsSmall *small;
    // large head is the most recent large allocation, for simplicity.
    natsLarge *large;

#ifdef DEV_MODE
    size_t totalAllocs;
    size_t totalFrees;
    size_t totalSmallAllocs;
    size_t totalLargeAllocs;
    size_t totalSmallFrees;
    size_t totalLargeFrees;
    int totalSmallChunks;
    const char *name;
    const char *file;
    int line;
    const char *func;
#endif
};

natsStatus
natsPool_create(natsPool **newPool, size_t pageSize, const char *name);
void *natsPool_alloc(natsPool *pool, size_t size);

static inline natsStatus natsPool_allocS(void **newMem, natsPool *pool, size_t size)
{
    if (newMem == NULL)
        return NATS_INVALID_ARG;
    *newMem = natsPool_alloc(pool, size);
    return (*newMem == NULL ? NATS_NO_MEMORY : NATS_OK);
}

#ifdef DEV_MODE

static inline natsStatus natsPool_log_create(natsPool **newPool, size_t pageSize, const char *name DEV_MODE_ARGS)
{
    natsStatus s = natsPool_create(newPool, pageSize, name);
    if (s != NATS_OK)
    {
        MEMLOGx(file, line, func, "POOL '%s' create failed: %d", name, s);
        return s;
    }
    MEMLOGx(file, line, func, "POOL '%s' created with page size %zu: %p", name, (*newPool)->pageSize, (void *)*newPool);
    (*newPool)->name = name;
    (*newPool)->file = file;
    (*newPool)->line = line;
    (*newPool)->func = func;

    return s;
}

static inline void *natsPool_log_alloc(natsPool *pool, size_t size, const char *file, int line, const char *func)
{
    void *mem = natsPool_alloc(pool, size);
    if (mem != NULL)
        MEMLOGx(file, line, func, "POOL '%s' allocated %zu bytes: %p", pool->name, size, mem);
    else
        MEMLOGx(file, line, func, "POOL '%s' allocating %zu bytes failed", pool->name, size);
    return mem;
}

static inline natsStatus natsPool_log_allocS(void **newMem, natsPool *pool, size_t size, const char *file, int line, const char *func)
{
    natsStatus s = natsPool_allocS(newMem, pool, size);
    if (s == NATS_OK)
        MEMLOGx(file, line, func, "POOL '%s' allocated %zu bytes: %p", pool->name, size, *newMem);
    else
        MEMLOGx(file, line, func, "POOL '%s' allocating %zu bytes failed", pool->name, size);
    return s;
}

#define natsPool_Create(_p, _s, _n) natsPool_log_create((_p), (_s), (_n)DEV_MODE_CTX)
#define natsPool_Alloc(_p, _s) natsPool_log_alloc((_p), (_s)DEV_MODE_CTX)
#define natsPool_AllocS(_n, _p, _s) natsPool_log_allocS((_n), (_p), (_s)DEV_MODE_CTX)

#else

#define natsPool_Create(_p, _s, _n) natsPool_create((_p), (_s), (_n))
#define natsPool_Alloc(_p, _s) natsPool_alloc((_p), (_s))
#define natsPool_AllocS(_n, _p, _s) natsPool_allocS((_n), (_p), (_s))

#endif

void natsPool_Destroy(natsPool *pool);

static inline uint8_t *natsPool_Strdup(natsPool *pool, const uint8_t *str)
{
    size_t len = strlen((const char *)str) + 1;
    uint8_t *dup = natsPool_Alloc(pool, len);
    if (dup != NULL)
        memcpy(dup, str, len);
    return dup;
}

static inline char *natsPool_StrdupC(natsPool *pool, const char *str)
{
    return (char *)natsPool_Strdup(pool, (const uint8_t *)str);
}

#define DUP_STRING_POOL(s, pool, s1, s2)                \
    {                                                   \
        (s1) = natsPool_StrdupC((pool), (s2));          \
        if ((s1) == NULL)                               \
            (s) = nats_setDefaultError(NATS_NO_MEMORY); \
    }

#define IF_OK_DUP_STRING_POOL(s, pool, s1, s2)       \
    if (((s) == NATS_OK) && !nats_isStringEmpty(s2)) \
    DUP_STRING_POOL((s), (pool), (s1), (s2))

struct __natsBuffer_s
{
    uint8_t *data;

    size_t cap;
    size_t len;

    natsPool *pool;
    natsPool *poolToDestroy;
    natsSmall *small;
    natsLarge *large;
};

// Heap-based functions

#ifdef DEV_MODE
static inline void *natsHeap_log_RawAlloc(size_t size, const char *file, int line, const char *func)
{
    void *mem = malloc(size);
    MEMLOGx(file, line, func, "HEAP malloc %zu bytes: %p", size, mem);
    return mem;
}
#define natsHeap_RawAlloc(_s) natsHeap_log_RawAlloc((_s)DEV_MODE_CTX)

static inline void *natsHeap_log_Alloc(size_t nmemb, size_t size, const char *file, int line, const char *func)
{
    void *mem = calloc(nmemb, size);
    MEMLOGx(file, line, func, "HEAP calloc %zu bytes: %p", size, mem);
    return mem;
}
#define natsHeap_Alloc(_s) natsHeap_log_Alloc(1, (_s)DEV_MODE_CTX)

static inline void *natsHeap_log_Realloc(void *ptr, size_t size, const char *file, int line, const char *func)
{
    void *mem = realloc(ptr, size);
    MEMLOGx(file, line, func, "HEAP realloc %zu bytes: %p", size, mem);
    return mem;
}
#define natsHeap_Realloc(_p, _s) natsHeap_log_Realloc((_p), (_s)DEV_MODE_CTX)

static inline void natsHeap_log_Free(void *ptr, const char *file, int line, const char *func)
{
    free(ptr);
    MEMLOGx(file, line, func, "HEAP free: %p", ptr);
}
#define natsHeap_Free(_p) natsHeap_log_Free((_p)DEV_MODE_CTX)

#else

#define natsHeap_RawAlloc(_s) malloc((_s))
#define natsHeap_Alloc(_s) calloc(1, (_s))
#define natsHeap_Realloc(_p, _s) realloc((_p), (_s))
#define natsHeap_Free(_p) free((_p))

#endif

#ifdef _WIN32
#define natsHeap_Strdup(_s) _strdup((_s))
#else
#define natsHeap_Strdup(_s) strdup((_s))
#endif

#define DUP_STRING_HEAP(s, s1, s2)                      \
    {                                                   \
        (s1) = natsHeap_Strdup(s2);                     \
        if ((s1) == NULL)                               \
            (s) = nats_setDefaultError(NATS_NO_MEMORY); \
    }

#define IF_OK_DUP_STRING_HEAP(s, s1, s2)             \
    if (((s) == NATS_OK) && !nats_isStringEmpty(s2)) \
    DUP_STRING_HEAP((s), (s1), (s2))

//----------------------------------------------------------------
// string functions.
//

struct __natsString_s
{
    size_t len;
    uint8_t *data;
};

static inline size_t
nats_strlen(const uint8_t *s) { return strlen((const char *)s); }
static inline uint8_t *nats_strchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strchr((const char *)s, (int)(find)); }
static inline uint8_t *nats_strrchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strrchr((const char *)s, (int)(find)); }
static inline const uint8_t *nats_strstr(const uint8_t *s, const char *find) { return (const uint8_t *)strstr((const char *)s, find); }
static inline int nats_strcmp(const uint8_t *s1, const char *s2) { return strcmp((const char *)s1, s2); }

static inline int nats_strarray_find(const char **array, int count, const char *str)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(array[i], str) == 0)
            return i;
    }
    return -1;
}

static inline size_t nats_strarray_remove(char **array, int count, const char *str)
{
    int i = nats_strarray_find((const char **)array, count, str);
    if (i < 0)
        return count;

    for (int j = i + 1; j < count; j++)
        array[j - 1] = array[j];

    return count - 1;
}

#define NATS_STR(str)                   \
    {                                   \
        sizeof(str) - 1, (uint8_t *)str \
    }
#define NATS_EMPTY_STR \
    {                  \
        0, NULL        \
    }

#define natsString_Set(str, text) ( \
    (str)->len = sizeof(text) - 1,  \
    (str)->data = (uint8_t *)(text), str)

#define natsString_Printable(str)     \
    ((str) == NULL ? 6 : (str)->len), \
        ((str) == NULL ? "<NULL>" : (const char *)(str)->data)

static inline bool natsString_Equal(natsString *str1, natsString *str2)
{
    if (str1 == str2)
        return true;
    return (str1 != NULL) && (str2 != NULL) &&
           (str1->len == str2->len) &&
           (strncmp((const char *)str1->data, (const char *)str2->data, str1->len) == 0);
}

static inline bool natsString_EqualC(natsString *str1, const char *lit)
{
    if ((str1 == NULL) && (lit == NULL))
        return true;
    return (str1 != NULL) && (lit != NULL) &&
           (str1->len == strlen((const char *)lit)) &&
           (strncmp((const char *)str1->data, lit, str1->len) == 0);
}

static inline bool natsString_IsEmpty(natsString *str)
{
    return (str == NULL) || (str->len == 0);
}

#define nats_toLower(c) (uint8_t)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c)
#define nats_toUpper(c) (uint8_t)((c >= 'a' && c <= 'z') ? (c & ~0x20) : c)
#define nats_isStringEmpty(_s) (((_s) == NULL) || (strlen(_s) == 0))

//----------------------------------------------------------------
// natsBuffer functions.
//

#define natsBuf_Data(b) ((b)->data)
#define natsBuf_Capacity(b) ((b)->cap)
#define natsBuf_Len(b) ((b)->len)
#define natsBuf_Available(b) ((b)->cap - (b)->len)

//
// Allocates a new natsBuffer using calloc.
natsStatus
natsBuf_Create(natsBuffer **newBuf, size_t cap);

// Allocates a new natsBuffer using palloc.
natsStatus
natsBuf_CreateInPool(natsBuffer **newBuf, natsPool *pool, size_t cap);

// Resets the buffer length to 0.
natsStatus natsBuf_Reset(natsBuffer *buf);

natsStatus
natsBuf_AppendBytes(natsBuffer *buf, const uint8_t *data, size_t dataLen);

natsStatus
natsBuf_AppendByte(natsBuffer *buf, uint8_t b);

void natsBuf_Destroy(natsBuffer *buf);

static inline natsStatus
natsBuf_AppendString(natsBuffer *buf, const char *str)
{
    if (str == NULL)
        return NATS_OK;
    return natsBuf_AppendBytes(buf, (const uint8_t *)str, strlen(str));
}

natsStatus
natsBuf_Reset(natsBuffer *buf);

#endif /* MEM_H_ */
