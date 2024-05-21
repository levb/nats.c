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

struct __natsChunk_s
{
    struct __natsChunk_s *next;
    uint8_t *data;
    size_t len;
};

struct __natsChain_s
{
    natsChunk *head;

    // When new chunks are added
    size_t newChunkSize;
};

struct __natsLarge_s
{
    struct __natsLarge_s *next;
    uint8_t *data;
};

struct __natsPool_s
{
    natsChain small;
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

#define NATS_MALLOC(s) malloc((s))
#define NATS_CALLOC(c, s) calloc((c), (s))
#define NATS_REALLOC(p, s) realloc((p), (s))
#define NATS_FREE(p) free((p))

#ifdef _WIN32
#define NATS_STRDUP(s) _strdup((s))
#else
#define NATS_STRDUP(s) strdup((s))
#endif

//----------------------------------------------------------------
// string functions.
//

#define NATS_STR(str)                   \
    {                                   \
        sizeof(str) - 1, (uint8_t *)str \
    }
#define NATS_EMPTY_STR \
    {                  \
        0, NULL        \
    }

#define natsString_Set(str, text) ((str)->len = sizeof(text) - 1, (str)->data = (uint8_t *)(text), str)
#define natsString_SetStr(str, stringlit) ((str)->len = strlen(stringlit), (str)->data = (uint8_t *)(stringlit), str)

static inline const uint8_t *nats_strstr(const uint8_t *s, const char *find)
{
    return (const uint8_t *)strstr((const char *)s, find);
}

static inline size_t nats_strlen(const uint8_t *s) { return strlen((const char *)s); }
static inline uint8_t *nats_strchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strchr((const char *)s, (int)(find)); }
static inline uint8_t *nats_strrchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strrchr((const char *)s, (int)(find)); }

#define nats_ToLower(c) (uint8_t)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c)
#define nats_ToUpper(c) (uint8_t)((c >= 'a' && c <= 'z') ? (c & ~0x20) : c)

natsString *natsString_DupPool(natsPool *pool, const natsString *src);
natsString *natsString_DupPoolStr(natsPool *pool, const char *src);
#define natsString_DupStr(_s) strdup(_s)

#define nats_IsStringEmpty(_s) (((_s) == NULL) || (strlen(_s) == 0))

//----------------------------------------------------------------
// natsChain functions.

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize);
void natsChain_Destroy(natsChain *chain);
natsChunk *natsChain_Alloc(natsChain *chain, size_t size);
natsStatus natsChain_AddChunkRef(natsChain *chain, uint8_t *data, size_t len);

void natsChunk_Destroy(natsChunk *c);

//----------------------------------------------------------------
// natsPool functions.

natsStatus natsPool_Create(natsPool **newPool, size_t chunkSize, bool init);
void *natsPool_Alloc(natsPool *pool, size_t size);
void natsPool_UndoLast(natsPool *pool, void *mem);
void natsPool_Destroy(natsPool *pool);

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

#endif /* MEM_H_ */
