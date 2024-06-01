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

#define NATS_DEFAULT_NEW_CHUNK_SIZE 4096

struct __natsString_s
{
    size_t len;
    uint8_t *data;
};

//----------------------------------------------------------------
//  Chain structure:
//
//   1st chunk              2nd chunk
// +---------------+  +-->+---------------+
// | natsChain_s   |  |   | natsChunk_s   |--> ...
// +---------------+  |   +---------------+
// | natsChunk_s   |--|   | used          |
// +---------------+      | memory        |
// | used          |      | (len)         |
// | memory        |      +---------------+
// | (len)         |      | free          |
// +---------------+      | memory        |
// | free          |      |               |
// | memory        |      |               |
// |               |      |               |
// +---------------+      +---------------+
struct __natsChunk_s
{
    struct __natsChunk_s *prev;
    size_t len;
    uint8_t *mem;

    uint8_t data[];
};

struct __natsChain_s
{
    natsChunk *current;
    // When new chunks are added
    size_t chunkSize;

    uint8_t data[];
};

struct __natsLarge_s
{
    struct __natsLarge_s *prev;
    uint8_t *mem;
};

struct __natsPool_s
{
    natsChain *small;
    natsLarge *large;
};

// A natsBuffer is an expandable, continous memory area used mostly to build
// strings. It can be backed by:
// - a chunk of memory owned by the caller, i.e. the caller is responsible for
//   freeing it.
// - a chunk of memory allocated by the buffer itself, will be freed if/when the
//   buffer is destroyed.
// - a chunk of memory associated with a pool, will be freed when the pool is
//   destroyed.
//
// The natsBuffer itself can be owned by the caller, allocated with calloc, or
// allocated from a pool.
struct __natsBuffer_s
{
    uint8_t *data;

    size_t cap;
    size_t len;

    bool freeData;
    bool freeSelf;

    natsPool *pool;
    natsChunk *small;
    natsLarge *large;
};

// Heap-based functions

#define natsHeap_RawAlloc(_s) malloc((_s))
#define natsHeap_Alloc(_s) calloc(1, (_s))
#define natsHeap_Realloc(_p, _s) realloc((_p), (_s))
#define natsHeap_Free(_p) free((_p))

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

static inline size_t nats_strlen(const uint8_t *s) { return strlen((const char *)s); }
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

static inline bool natsString_EqualZ(natsString *str1, const char *lit)
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
// natsChain functions.

#define _first_chunk(_chain) ((natsChunk *)((uint8_t *)(_chain) + sizeof(natsChain)));
#define _chunk_cap(_chain) ((_chain)->chunkSize - sizeof(natsChunk))
#define _chunk_remaining_cap(_chain) ((_chain)->chunkSize - sizeof(natsChunk) - chunk->len)
#define _chunk_mem_ptr(_chunk) ((uint8_t *)(_chunk) + sizeof(natsChunk) + chunk->len)

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize);
natsStatus natsChain_Destroy(natsChain *chain);
natsStatus natsChain_AllocChunk(natsChunk **newChunk, natsChain *chain, size_t size);

//----------------------------------------------------------------
// natsPool functions.

natsStatus natsPool_Create(natsPool **newPool);
void *natsPool_Alloc(natsPool *pool, size_t size);
static inline natsStatus natsPool_AllocS(void **newMem, natsPool *pool, size_t size)
{
    if (newMem == NULL)
        return NATS_INVALID_ARG;
    *newMem = natsPool_Alloc(pool, size);
    return (*newMem == NULL ? NATS_NO_MEMORY : NATS_OK);
}

void natsPool_Destroy(natsPool *pool);

char *natsPool_Strdup(natsPool *pool, const uint8_t *str);

#define DUP_STRING_POOL(s, pool, s1, s2)                \
    {                                                   \
        (s1) = natsPool_Strdup((pool), (s2));           \
        if ((s1) == NULL)                               \
            (s) = nats_setDefaultError(NATS_NO_MEMORY); \
    }

#define IF_OK_DUP_STRING_POOL(s, pool, s1, s2)       \
    if (((s) == NATS_OK) && !nats_isStringEmpty(s2)) \
    DUP_STRING_POOL((s), (pool), (s1), (s2))

//----------------------------------------------------------------
// natsBuffer functions.
//

#define NATS_DEFAULT_BUFFER_SIZE 256

#define natsBuf_Data(b) ((b)->data)
#define natsBuf_Capacity(b) ((b)->cap)
#define natsBuf_Len(b) ((b)->len)
#define natsBuf_Available(b) ((b)->cap - (b)->len)
#define natsBuf_MoveTo(b, pos) (b)->len = pos;

// Initializes an caller-allocated natsBuffer using 'data' as the back-end byte
// array. natsBuf_Destroy will not free 'data', it is the responsibility of the
// caller. natsBuf_Expand() may calloc a new underlying array if needed and that
// array will be freed in natsBuf_Destroy.
//
// One would use this call to initialize a natsBuffer without the added cost of
// allocating memory for the natsBuffer structure itself, for instance
// initializing an natsBuffer on the stack.
natsStatus
natsBuf_InitWith(natsBuffer *buf, uint8_t *data, size_t len, size_t cap);

// Initializes an caller-allocated natsBuffer with a new backend using calloc.
natsStatus
natsBuf_InitCalloc(natsBuffer *buf, size_t cap);

// Allocates a new natsBuffer using calloc.
natsStatus
natsBuf_CreateCalloc(natsBuffer **newBuf, size_t cap);

// Allocates a new natsBuffer using palloc.
natsStatus
natsBuf_CreatePool(natsBuffer **newBuf, natsPool *pool, size_t cap);

// Resets the buffer length to 0.
natsStatus natsBuf_Reset(natsBuffer *buf);

// Expands 'buf' underlying buffer to the given new size 'newSize'.
//
// If 'buf' did not own the underlying buffer, a new buffer is
// created and data copied over. The original data is now detached.
// The underlying buffer is now owned by 'buf' and will be freed when
// the natsBuffer is destroyed, if needed.
//
// When 'buf' owns the underlying buffer and it is expanded, a memory
// reallocation of the buffer occurs to satisfy the new size requirement.
//
// Note that one should not save the returned value of natsBuf_Data() and
// use it after any call to natsBuf_Expand/Append/AppendByte() since
// the memory address for the underlying byte buffer may have changed due
// to the buffer expansion.
natsStatus
natsBuf_Expand(natsBuffer *buf, size_t newSize);

// Appends 'dataLen' bytes from the 'data' byte array to the buffer,
// potentially expanding the buffer.
// See natsBuf_Expand for details about natsBuffer not owning the data.
natsStatus
natsBuf_Append(natsBuffer *buf, const uint8_t *data, size_t dataLen);

// Appends a byte to the buffer, potentially expanding the buffer.
// See natsBuf_Expand for details about natsBuffer not owning the data.
natsStatus
natsBuf_AppendByte(natsBuffer *buf, uint8_t b);

natsStatus
natsBuf_Destroy(natsBuffer *buf);

static inline natsStatus
natsBuf_AppendString(natsBuffer *buf, const char *str)
{
    return natsBuf_Append(buf, (const uint8_t *)str, -1);
}

natsStatus
natsPool_ExpandBuffer(natsBuffer *buf, size_t capacity);

natsStatus
natsBuf_Reset(natsBuffer *buf);

#endif /* MEM_H_ */
