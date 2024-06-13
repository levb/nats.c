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

static size_t _pageSize = NATS_DEFAULT_PAGE_SIZE;
static size_t _chainSize = NATS_DEFAULT_PAGE_SIZE;

// for testing
void natsPool_setPageSize(size_t size) { _pageSize = size; }
void natsPool_setChainSize(size_t size) { _chainSize = size; }

#define _numPages(c) \
    (((c) / nats_memPageSize() + 1) * nats_memPageSize())
#define _roundUpCapacity(c) \
    (_numPages(c) * nats_memPageSize())

static inline size_t _smallMax(natsPool *pool) { return _pageSize - sizeof(natsSmall); }
static inline size_t _smallCap(natsPool *pool, natsSmall *small) { return _pageSize - small->len; }
static inline void *_smallGrab(natsSmall *small, size_t size)
{
    void *mem = (uint8_t *)small + small->len;
    small->len += size;
    return mem;
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

    small = natsHeap_Alloc(pool->pageSize); // greater than sizeof(natsSmall)
    if (small == NULL)
    {
        MEMLOGx(file, line, func, "FAILED to alloc in POOL '%s': NEW small page %zu bytes", pool->name, pool->pageSize);
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
        MEMLOGx(file, line, func, "FAILED to alloc in POOL '%s': large %zu bytes", pool->name, pool->pageSize);
        return NULL;
    }
    large->prev = pool->large;
    pool->large = large;

    if (newLarge != NULL)
        *newLarge = large;

    MEMLOGx(file, line, func, "Alloc in POOL '%s': large, bytes:%zu ptr:%p ", pool->name, size, (void *)large->mem);
    return large->mem;
}

natsChain *_addChain(natsPool *pool DEV_MODE_ARGS)
{
    if (pool -> readLast == NULL)
    {
        natsReadBuffer *buf = _allocSmall(NULL, pool, sizeof(natsReadBuffer) DEV_MODE_CTX);
        if (buf == NULL)
            return NULL;
        pool->readLast = buf;
        pool->readFirst = buf;
    }


    natsReadBuffer *buf = _allocSmall(NULL, pool, sizeof(natsReadBuffer) DEV_MODE_CTX);
    if (buf == NULL)
        return NULL;

    buf->mem = natsHeap_Alloc(_chainSize);
    if (buf->mem == NULL)
    {
        MEMLOGx(file, line, func, "FAILED to alloc in POOL '%s': read buffer %zu bytes", pool->name, _chainSize);
        return NULL;
    }

    buf->data = buf->mem;
    buf->len = 0;
    MEMLOGx(file, line, func, "Alloc in POOL '%s': ReadBuffer, ptr:%p", pool->name, (void *)buf->mem);
    return buf;
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

natsStatus natsPool_log_create(natsPool **newPool, size_t pageSize, const char *name DEV_MODE_ARGS)
{
    natsStatus s = natsPool_create(newPool, pageSize, name);
    if (s != NATS_OK)
    {
        MEMLOGx(file, line, func, "FAILED to create POOL '%s':  %d", name, s);
        return s;
    }
    MEMLOGx(file, line, func, "Created POOL '%s', pageSize:%zu, ptr: %p", name, (*newPool)->pageSize, (void *)*newPool);
    (*newPool)->name = name;
    (*newPool)->file = file;
    (*newPool)->line = line;
    (*newPool)->func = func;

    return s;
}

natsReadBuffer *natsPool_log_getReadBuffer(natsPool *pool DEV_MODE_ARGS)
{
    return _getReadBuffer(pool, file, line, func);
}

#endif

natsStatus
natsPool_create(natsPool **newPool, size_t pageSize, const char *name)
{
    natsPool *pool = NULL;
    natsSmall *small = NULL;

    if (pageSize == 0)
        pageSize = _pageSize;

    const size_t required = sizeof(natsPool) + sizeof(natsSmall);
    if (required > pageSize)
        return nats_setError(NATS_INVALID_ARG, "page size %zu too small, need at least %zu", pageSize, required);

    small = natsHeap_Alloc(pageSize);
    if (small == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    _smallGrab(small, sizeof(natsSmall)); // mark itself allocated

    pool = _smallGrab(small, sizeof(natsPool));
    pool->small = small;
    pool->pageSize = pageSize;

    *newPool = pool;
    return NATS_OK;
}

void *natsPool_alloc(natsPool *pool, size_t size)
{
    if (size > _smallMax(pool))
        return _allocLarge(pool, size, NULL DEV_MODE_CTX);
    else
        return _allocSmall(NULL, pool, size DEV_MODE_CTX);
}

natsReadBuffer *natsPool_getReadBuffer(natsPool *pool)
{
    return _getReadBuffer(pool DEV_MODE_CTX);
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
    natsStatus s = natsPool_Create(&pool, 2 * NATS_DEFAULT_BUFFER_SIZE, "natsBuffer");
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
