// Copyright 2015-2024 The NATS Authors
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

#include "natsp.h"

#include "mem.h"
#include "err.h"

static size_t nats_memPageSize_v = NATS_DEFAULT_MEM_PAGE_SIZE;

// for testing
void nats_setMemPageSize(size_t size) { nats_memPageSize_v = size; }
size_t nats_memPageSize(void) { return nats_memPageSize_v; }

#define _numPages(c) \
    (((c) / nats_memPageSize() + 1) * nats_memPageSize())
#define _roundUpCapacity(c) \
    (_numPages(c) * nats_memPageSize())
    
natsStatus
natsPool_Create(natsPool **newPool)
{
    natsStatus s = NATS_OK;
    natsPool *pool = NULL;
    natsChain *chain = NULL;
    natsChunk *chunk = NULL;

    if (newPool == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = natsChain_Create(&chain, nats_memPageSize());
    IFOK(s, natsChain_AllocChunk(&chunk, chain, sizeof(natsPool)));

    if (s != NATS_OK)
        return s;

    pool = (natsPool *)_chunk_mem_ptr(chunk);
    chunk->len += sizeof(natsPool);
    pool->small = chain;
    *newPool = pool;

    return NATS_OK;
}

static inline void *
_allocLarge(natsPool *pool, size_t size, natsLarge **newLarge)
{
    natsLarge *large = NULL;
    if (newLarge != NULL)
        *newLarge = NULL;

    large = natsPool_Alloc(pool, sizeof(natsLarge));
    if (large == NULL)
        return NULL;

    large->mem = natsHeap_Alloc(size);
    if (large->mem == NULL)
        return NULL;
    large->prev = pool->large;
    pool->large = large;

    if (newLarge != NULL)
        *newLarge = large;
    return large->mem;
}

static inline void *
_allocSmall(natsPool *pool, size_t size)
{
    natsChunk *chunk = NULL;
    void *mem = NULL;

    natsChain_AllocChunk(&chunk, pool->small, size);
    if (chunk == NULL)
        return NULL;

    mem = _chunk_mem_ptr(chunk);
    chunk->len += size;
    return mem;
}

void *natsPool_Alloc(natsPool *pool, size_t size)
{
    if (size > _chunk_cap(pool->small))
        return _allocLarge(pool, size, NULL);
    else
        return _allocSmall(pool, size);
}

void natsPool_Destroy(natsPool *pool)
{
    natsLarge *l;
    if (pool == NULL)
        return;

    for (l = pool->large; l != NULL; l = l->prev)
        natsHeap_Free(l->mem);

    natsChain_Destroy(pool->small);

    // The pool itself is allocated in the chain, so no need to free it.
}

static natsStatus
_expandBufInPool(natsBuffer *buf, size_t capacity)
{
    uint8_t *mem = NULL;
    size_t prevCap = buf->cap;
    size_t newCap;
    natsChunk *prevSmall = buf->small;

    // Double the previous capacity.
    for (newCap = prevCap; newCap < capacity; newCap *= 2)
        ;

    // If the buffer was already allocated in a "large" chunk, use realloc(),
    // it's most efficient.
    if (buf->large != NULL)
    {
        mem = natsHeap_Realloc(buf->large->mem, newCap);
        if (mem == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);
        buf->data = mem;
        buf->cap = newCap;
        return NATS_OK;
    }

    if (newCap > _chunk_cap(buf->pool->small))
    {
        // We don't fit in a chunk, allocate a large.
        natsLarge *newLarge = NULL;
        mem = _allocLarge(buf->pool, newCap, &newLarge);
        if (mem == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);
        buf->small = NULL;
        buf->large = newLarge;
    }
    else
    {
        // We fit in a chunk, and since we already exhausted any previous chunk,
        // try a new one.
        natsChunk *chunk = NULL;
        natsStatus s = natsChain_AllocChunk(&chunk, buf->pool->small, newCap);
        if (s != NATS_OK)
            return nats_setDefaultError(s);

        // take up whatever capacity remains in the chunk so we can expand in
        // place later.
        buf->cap = _chunk_remaining_cap(buf->pool->small, chunk);
        buf->small = chunk;
        mem = _chunk_mem_ptr(chunk);
    }

    memcpy(mem, buf->data, buf->len);
    buf->data = mem;

    // If we were previously in a small chunk, return the space to the chunk.
    if (prevSmall != NULL)
    {
        prevSmall->len -= prevCap;
    }

    return NATS_OK;
}

static inline natsStatus
_expandBuf(natsBuffer *buf, size_t capacity)
{
    if ((capacity < buf->len) || (buf->pool == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);
    if (capacity >= 0x7FFFFFFF)
        return nats_setDefaultError(NATS_NO_MEMORY);
    if (capacity <= buf->cap)
        return NATS_OK;

    return _expandBufInPool(buf, capacity);
}

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize)
{
    if (chunkSize == 0)
        chunkSize = nats_memPageSize();

    size_t headSize = sizeof(natsChain) + sizeof(natsChunk);
    if (chunkSize < headSize)
        return nats_setDefaultError(NATS_INVALID_ARG);

    uint8_t *mem = natsHeap_Alloc(chunkSize);
    if (mem == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    natsChain *chain = (natsChain *)mem;
    natsChunk *chunk = (natsChunk *)(mem + sizeof(natsChain));
    chunk->len = sizeof(natsChunk);

    chain->chunkSize = chunkSize;
    chain->current = chunk;

    *newChain = chain;
    return NATS_OK;
}

natsStatus natsChain_Destroy(natsChain *chain)
{
    if (chain == NULL)
        return NATS_OK;

    natsChunk *chunk = chain->current;

    // The first chunk is part of the chain's initial allocation and has its
    // 'prev' set to NULL, so do not free it
    while (chunk != NULL && chunk->prev != NULL)
    {
        natsChunk *prev = chunk->prev;
        natsHeap_Free(chunk);
        chunk = prev;
    }
    natsHeap_Free(chain);
    return NATS_OK;
}

natsStatus
natsChain_AllocChunk(natsChunk **newChunk, natsChain *chain, size_t size)
{
    natsChunk *use = NULL;
    natsChunk *chunk = _first_chunk(chain);

    if (size > (chain->chunkSize - sizeof(natsChunk)))
        return NATS_INVALID_ARG;

    for (; chunk != NULL; chunk = chunk->prev)
    {
        if (chunk->len + size <= chain->chunkSize)
        {
            use = chunk;
            break;
        }
    }

    printf("<>/<> found %p\n", (void *)use);
    uint8_t *mem = _chunk_mem_ptr(use);
    if (use == NULL)
    {
        mem = natsHeap_Alloc(chain->chunkSize);
        if (mem == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        use = (natsChunk *)mem;
        use->len = sizeof(natsChunk);
        mem += sizeof(natsChunk);

        use->prev = chain->current;
        chain->current = use;
    }

    if (newChunk != NULL)
        *newChunk = use;
    return NATS_OK;
}

natsStatus
natsBuf_Reset(natsBuffer *buf)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    buf->len = 0;
    return NATS_OK;
}

// Creates a new Heap-based pool and allocates the natsBuffer there.
natsStatus
natsBuf_Create(natsBuffer **newBuf, size_t capacity)
{
    natsPool *pool = NULL;
    natsBuffer *buf = NULL;
    natsStatus s = natsPool_Create(&pool);
    IFOK(s, natsBuf_CreateInPool(newBuf, pool, capacity));
    if (s == NATS_OK)
        buf->poolToDestroy = pool;

    return nats_setDefaultError(s);
}

natsStatus
natsBuf_CreateInPool(natsBuffer **newBuf, natsPool *pool, size_t capacity)
{
    natsBuffer *buf = natsPool_Alloc(pool, sizeof(natsBuffer));
    if (buf == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    buf->pool = pool;

    natsStatus s = _expandBuf(buf, capacity);
    if (s != NATS_OK)
        return s;

    printf("<>/<> natsBuf_CreatePool: created buffer in pool %p, size %zu\n", (void *)pool, capacity);
    *newBuf = buf;
    return NATS_OK;
}

natsStatus
natsBuf_AppendBytes(natsBuffer *buf, const uint8_t *data, size_t dataLen)
{
    natsStatus s = NATS_OK;
    size_t n = buf->len + (size_t)dataLen;

    if (dataLen == (size_t)-1 || dataLen == 0)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (n > buf->cap)
        s = _expandBuf(buf, n);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    memcpy(buf->data + buf->len, data, dataLen);
    buf->len += dataLen;

    return NATS_OK;
}

natsStatus
natsBuf_AppendByte(natsBuffer *buf, uint8_t b)
{
    natsStatus s = NATS_OK;
    size_t n;

    if ((n = buf->len + 1) > buf->cap)
        s = _expandBuf(buf, n);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    if (s == NATS_OK)
    {
        buf->data[buf->len] = b;
        buf->len++;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

void natsBuf_Destroy(natsBuffer *buf)
{
    natsPool_Destroy(buf->poolToDestroy);
}
