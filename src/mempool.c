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

natsStatus
natsPool_Create(natsPool **newPool)
{
    natsStatus s = NATS_OK;
    natsPool *pool = NULL;
    natsChain *chain = NULL;
    natsChunk *chunk = NULL;

    if (newPool == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    s = natsChain_Create(&chain, NATS_DEFAULT_NEW_CHUNK_SIZE);
    IFOK(s, natsChain_AllocChunk(&chunk, chain, sizeof(natsPool)));

    if (s != NATS_OK)
        return s;

    pool = (natsPool *)_chunk_mem_ptr(chunk);
    chunk->len += sizeof(natsPool);
    pool->small = chain;
    *newPool = pool;

    return NATS_OK;
}

// natsStatus natsPool_UndoLast(natsPool *pool, void *mem)
// {
//     if (pool == NULL)
//         return nats_setDefaultError(NATS_INVALID_ARG);

//     if ((pool->large != NULL) && (pool->large->data == mem))
//     {
//         pool->large = pool->large->prev;
//         NATS_FREE(mem);
//     }
//     else if (pool->small->current != NULL)
//     {
//         uint8_t *start = _chunk_mem_ptr(pool->small->current);
//         uint8_t *end = start + pool->small->current->len;
//         if (((uint8_t *)mem >= start) && ((uint8_t *)mem < end))
//         {
//             pool->small->current->len = (uint8_t *)mem - start;
//         }
//     }

//     return NATS_OK;
// }

static inline void *
_allocLarge(natsPool *pool, size_t size)
{
    natsLarge *large = NULL;

    large = natsPool_Alloc(pool, sizeof(natsLarge));
    if (large == NULL)
        return NULL;

    large->data = NATS_CALLOC(1, size);
    if (large->data == NULL)
        return NULL;
    large->prev = pool->large;
    pool->large = large;

    return large->data;
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

void * natsPool_Alloc( natsPool *pool, size_t size)
{
    if (size > _chunk_cap(pool->small))
        return _allocLarge(pool, size);
    else
        return _allocSmall(pool, size);
}

natsStatus
natsPool_ExpandBuffer(natsBuffer *buf, size_t capacity)
{
    uint8_t *mem = NULL;
    if ((buf == NULL) || (capacity < buf->len) || (buf->pool == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    size_t prevCap = buf->cap;

    if (capacity <= prevCap)
    {
        // Expand in place
        buf->cap = capacity;
        return NATS_OK;
    }
    // Can not expand in place, need to move.

    // If the buffer was allocated in a "large" chunk, use realloc(), it's most efficient.
    if (buf->large != NULL)
    {
        void *newData = NATS_REALLOC(buf->large->data, capacity);
        if (newData == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        buf->data = newData;
        buf->cap = capacity;
        return NATS_OK;
    }

    // Allocate new memory, move the contents.
    if (capacity > _chunk_cap(buf->pool->small))
    {
        // We don't fit in a chunk, allocate a large.
        mem = _allocLarge(buf->pool, capacity);
    }
    else
    {
        // We fit in a chunk.
        natsChunk *chunk = NULL;
        natsChain_AllocChunk(&chunk, buf->pool->small, capacity);
        if (chunk == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        // If the buffer was allocated at the end of a "small" chunk like we
        // just did, return the space to the pool/chunk.
        if (buf->small != NULL)
        {
            buf->small->len -= buf->cap;
        }

        // take up whatever capacity remains in the chunk so we can expand in
        // place later.
        buf->cap = _chunk_cap(buf->pool->small);
        buf->small = chunk;
        mem = _chunk_mem_ptr(chunk);
    }
    if (mem == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    memcpy(mem, buf->data, buf->len);
    buf->data = mem;

    return NATS_OK;
}

void natsPool_Destroy(natsPool *pool)
{
    natsLarge *l;
    if (pool == NULL)
        return;

    for (l = pool->large; l != NULL; l = l->prev)
    {
        NATS_FREE(l->data);
    }

    natsChain_Destroy(pool->small);

    // The pool itself is allocated in the chain so no need to free it.
}
