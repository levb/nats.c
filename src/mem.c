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

size_t nats_memPageSize = NATS_MEM_PAGE_SIZE;
size_t nats_memChainSize = NATS_MEM_CHAIN_SIZE;

#define _numPages(c) \
    (((c) / nats_memPageSize() + 1) * nats_memPageSize())
#define _roundUpCapacity(c) \
    (_numPages(c) * nats_memPageSize())

static inline size_t _smallMax(natsPool *pool) { return nats_memPageSize - sizeof(natsSmall); }
static inline size_t _smallCap(natsPool *pool, natsSmall *small) { return nats_memPageSize - small->len; }
static inline void *_smallGrab(natsSmall *small, size_t size)
{
    void *mem = (uint8_t *)small + small->len;
    small->len += size;
    return mem;
}

static inline natsPool *
_initPoolMemory(void *mempage)
{
    natsSmall *small = mempage;
    _smallGrab(small, sizeof(natsSmall)); // mark itself allocated
    natsPool *pool = _smallGrab(small, sizeof(natsPool));
    pool->small = small;
    return pool;
}

static void *_allocSmall(natsSmall **newOrFound, natsPool *pool, size_t size DEV_MODE_ARGS)
{
    natsSmall *last = pool->small;
    natsSmall *small = pool->small;
    int i = 0;
    void *mem = NULL;

    for (small = pool->small; small != NULL; small = small->next, i++)
    {
        if (size > _smallCap(pool, small))
        {
            last = small;
            continue;
        }

        mem = _smallGrab(small, size);

        if (newOrFound != NULL)
            *newOrFound = small;
        MEMLOGx(file, line, func, "Alloc in POOL '%s': small:%d, bytes:%zu, remaining: %zu, ptr:%p", pool->name, i, size, _smallCap(pool, small), mem);
        return mem;
    }

    small = natsHeap_Alloc(nats_memPageSize); // greater than sizeof(natsSmall)
    if (small == NULL)
    {
        MEMLOGx(file, line, func, "FAILED to alloc in POOL '%s': NEW small page %zu bytes", pool->name, nats_memPageSize);
        return NULL;
    }
    _smallGrab(small, sizeof(natsSmall)); // mark itself allocated
    mem = _smallGrab(small, size);

    // Link it to the end of the chain.
    last->next = small;

    if (newOrFound != NULL)
        *newOrFound = small;
    MEMLOGx(file, line, func, "Alloc in POOL '%s': small:%d (NEW), bytes:%zu, remaining: %zu, ptr:%p", pool->name, i, size, _smallCap(pool, small), mem);
    return mem;
}

natsStatus natsPool_recycle(natsChain **newChain, natsPool *pool)
{
    natsChain clone = {.mem = NULL};

    if (pool == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    // Free all large allocations.
    for (natsLarge *l = pool->large; l != NULL; l = l->prev)
        natsHeap_Free(l->mem);
    pool->large = NULL;

    // Free the chain, except the last link if it has remaining data. If so, make it the new chain.
    for (natsChain *chain = pool->chain; (chain != NULL) && (chain != pool->currentChain); chain = pool->chain)
    {
        natsHeap_Free(chain->mem);
    }
    if (pool->currentChain != NULL && natsChain_Len(pool->currentChain) > 0)
    {
        // Need to stash away the values since we may free the chain struct.
        clone.mem = pool->currentChain->mem;
        clone.readFrom = pool->currentChain->readFrom;
        clone.appendTo = pool->currentChain->appendTo;
    }
    else
    {
        natsHeap_Free(pool->currentChain->mem);
    }

    // Free all Smalls, except the first one. Must do it last since other entities
    natsSmall *next = NULL;
    for (natsSmall *s = pool->small->next; s != NULL; s = next)
    {
        next = s->next;
        natsHeap_Free(s);
    }
    pool = _initPoolMemory(pool->small);

    // Now, if there was remaining data in the last chain, add it back.
    if (clone.mem != NULL)
    {
        pool->chain = _allocSmall(NULL, pool, sizeof(natsChain) DEV_MODE_CTX);
        if (pool->chain == NULL)
            return NATS_NO_MEMORY;

        pool->chain->mem = clone.mem;
        pool->chain->readFrom = clone.readFrom;
        pool->chain->appendTo = clone.appendTo;
        pool->currentChain = pool->chain;
    }

    if (newChain != NULL)
        *newChain = pool->chain;
    return NATS_OK;
}

static void *
_allocLarge(natsPool *pool, size_t size, natsLarge **newLarge DEV_MODE_ARGS)
{
    natsLarge *large = NULL;
    if (newLarge != NULL)
        *newLarge = NULL;

    large = _allocSmall(NULL, pool, sizeof(natsLarge) DEV_MODE_CTX);
    if (large == NULL)
        return NULL;

    large->mem = natsHeap_Alloc(size);
    if (large->mem == NULL)
    {
        MEMLOGx(file, line, func, "FAILED to alloc in POOL '%s': large %zu bytes", pool->name, nats_memPageSize);
        return NULL;
    }
    large->prev = pool->large;
    pool->large = large;

    if (newLarge != NULL)
        *newLarge = large;

    MEMLOGx(file, line, func, "Alloc in POOL '%s': large, bytes:%zu ptr:%p ", pool->name, size, (void *)large->mem);
    return large->mem;
}

natsStatus _addChain(natsChain **newChain, natsPool *pool DEV_MODE_ARGS)
{
    // If we have a current chain and it has enough space, return it.
    if (pool->currentChain != NULL && natsChain_Available(pool->currentChain) >= NATS_MEM_CHAIN_MIN)
    {
        *newChain = pool->currentChain;
        return NATS_OK;
    }

    // Don't have a chain or the current one is full, allocate a new one.
    natsChain *chain = _allocSmall(NULL, pool, sizeof(natsChain) DEV_MODE_CTX);
    if (chain == NULL)
        return NATS_NO_MEMORY;

    chain->mem = natsHeap_Alloc(nats_memChainSize);
    if (chain->mem == NULL)
        return NATS_NO_MEMORY;
    chain->readFrom = chain->mem;
    chain->appendTo = chain->mem;

    if (pool->currentChain == NULL)
    {
        // First chain
        pool->chain = chain;
        pool->currentChain = chain;
    }
    else
    {
        // Link it to the end of the chain.
        pool->currentChain->next = chain;
        pool->currentChain = chain;
    }

    *newChain = chain;
    return NATS_OK;
}

#ifdef DEV_MODE

void *natsPool_log_alloc(natsPool *pool, size_t size DEV_MODE_ARGS)
{
    if (size > _smallMax(pool))
        return _allocLarge(pool, size, NULL, file, line, func);
    else
        return _allocSmall(NULL, pool, size, file, line, func);
}

natsStatus natsPool_log_allocS(void **newMem, natsPool *pool, size_t size DEV_MODE_ARGS)
{
    void *mem = natsPool_log_alloc(pool, size, file, line, func);
    if (mem == NULL)
        return NATS_NO_MEMORY;
    *newMem = mem;
    return NATS_OK;
}

natsStatus natsPool_log_create(natsPool **newPool, const char *name DEV_MODE_ARGS)
{
    natsStatus s = natsPool_create(newPool, name);
    if (s != NATS_OK)
    {
        MEMLOGx(file, line, func, "FAILED to create POOL '%s':  %d", name, s);
        return s;
    }
    MEMLOGx(file, line, func, "Created POOL '%s', pageSize:%zu, ptr: %p", name, nats_memPageSize, (void *)*newPool);
    (*newPool)->name = name;
    (*newPool)->file = file;
    (*newPool)->line = line;
    (*newPool)->func = func;

    return s;
}

natsStatus natsPool_log_addChain(natsChain **chain, natsPool *pool DEV_MODE_ARGS)
{
    return _addChain(chain, pool, file, line, func);
}

#endif

natsStatus
natsPool_create(natsPool **newPool, const char *name)
{
    void *mempage = NULL;

    const size_t required = sizeof(natsPool) + sizeof(natsSmall);
    if (required > nats_memPageSize)
        return nats_setError(NATS_INVALID_ARG, "page size %zu too small, need at least %zu", nats_memPageSize, required);

    mempage = natsHeap_Alloc(nats_memPageSize);
    if (mempage == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    *newPool = _initPoolMemory(mempage);
    (*newPool)->name = name;
    return NATS_OK;
}

void *natsPool_alloc(natsPool *pool, size_t size)
{
    if (size > _smallMax(pool))
        return _allocLarge(pool, size, NULL DEV_MODE_CTX);
    else
        return _allocSmall(NULL, pool, size DEV_MODE_CTX);
}

natsStatus natsPool_addChain(natsChain **chain, natsPool *pool)
{
    return _addChain(chain, pool DEV_MODE_CTX);
}

void natsPool_Destroy(natsPool *pool)
{
    if (pool == NULL)
        return;

    MEMLOGf("Destroying POOL '%s'", pool->name);

    for (natsLarge *l = pool->large; l != NULL; l = l->prev)
        natsHeap_Free(l->mem);

    natsSmall *next = NULL;
    for (natsSmall *s = pool->small; s != NULL; s = next)
    {
        next = s->next;
        natsHeap_Free(s);
    }

    // The pool itself is allocated in the first natsSmall, so no need to free
    // it.
}

static natsStatus
_expandBufInPool(natsBuffer *buf, size_t capacity)
{
    uint8_t *mem = NULL;
    size_t prevCap = buf->cap > 0 ? buf->cap : 1;
    size_t newCap;
    natsSmall *prevSmall = buf->small;

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

    if (newCap > _smallMax(buf->pool))
    {
        // We don't fit in a small, allocate a large.
        natsLarge *newLarge = NULL;
        mem = _allocLarge(buf->pool, newCap, &newLarge DEV_MODE_CTX);
        if (mem == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);
        buf->small = NULL;
        buf->large = newLarge;
    }
    else
    {
        // take up an entire natsSmall, will return it to the pool when done.
        newCap = _smallMax(buf->pool);
        mem = _allocSmall(&buf->small, buf->pool, newCap DEV_MODE_CTX);
        if (mem == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        buf->large = NULL;
    }

    memcpy(mem, buf->data, buf->len);
    buf->data = mem;
    buf->cap = newCap;

    // If we were previously in a small, return the space to the pool.
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

natsStatus
natsBuf_Reset(natsBuffer *buf)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    buf->len = 0;
    return NATS_OK;
}

// Creates a new Heap-based pool and allocates the natsBuffer there.
// TODO: <>/<> do this with direct, page-aligned memory allocation.
natsStatus
natsBuf_Create(natsBuffer **newBuf, size_t capacity)
{
    natsPool *pool = NULL;
    natsBuffer *buf = NULL;
    natsStatus s = natsPool_Create(&pool,  "natsBuffer");
    IFOK(s, natsBuf_CreateInPool(&buf, pool, capacity));
    if (s == NATS_OK)
        buf->poolToDestroy = pool;

    *newBuf = buf;
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

    *newBuf = buf;
    MEMLOGf("created new buffer in pool '%s', cap %zu", pool->name, buf->cap);
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
