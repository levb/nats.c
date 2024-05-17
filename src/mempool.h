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

#ifndef MEMPOOL_H_
#define MEMPOOL_H_

#include "mem.h"
#include "status.h"
#include "err.h"

// Declarations only
#ifdef NATS_NO_INLINE_POOL

natsStatus natsPool_Create(natsPool **newPool, size_t chunkSize);

#endif /* NATS_NO_INLINE_POOL */

// Definitions
#if defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_POOL)

#ifdef NATS_MEM_C_
#define NATS_INLINE_POOL
#else
#define NATS_INLINE_POOL static inline
#endif

NATS_INLINE_POOL natsStatus
natsPool_Create(natsPool **newPool, size_t chunkSize)
{
    natsPool *pool = NULL;

    pool = NATS_CALLOC(1, sizeof(natsPool));
    if (pool == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    pool->small.chunkSize = (chunkSize == 0 ? NATS_DEFAULT_CHUNK_SIZE : chunkSize);
    *newPool = pool;
    return NATS_OK;
}

NATS_INLINE_POOL void *
nats_palloc(natsPool *pool, size_t size, bool takeAll)
{
    natsChunk *chunk = NULL;
    natsLarge *large = NULL;
    void *mem = NULL;

    if (size > pool->small.chunkSize)
    {
        large = nats_palloc(pool, sizeof(natsLarge), false);
        if (large == NULL)
            return NULL;

        large->data = NATS_CALLOC(1, size);
        if (large->data == NULL)
            return NULL;
        large->next = pool->large;
        pool->large = large;

        return large->data;
    }
    else
    {
        chunk = natsChain_AllocateChunk(&(pool->small), size);
        if (chunk == NULL)
            return NULL;

        mem = chunk->data + chunk->len;
        if (takeAll)
            chunk->len = pool->small.chunkSize;
        else
            chunk->len += size;

        return mem;
    }
}

NATS_INLINE_POOL void *
natsPool_Alloc(natsPool *pool, size_t size)
{
    return nats_palloc(pool, size, false);
}

NATS_INLINE_POOL natsStatus
natsPool_ExpandBuffer(natsBuffer *buf, size_t capacity)
{
    natsStatus s = NATS_OK;

    if ((buf == NULL) || (capacity < buf->len) || (buf->pool == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (capacity <= buf->cap)
    {
        // Expand in place
        buf->cap = capacity;
        return NATS_OK;
    }

   // Can not expand in place, need to move.
   if (buf->small != NULL)
   {
        // The space of this buffer is always the entire "end of the chunk", return it to the pool.
        buf->small->len -= buf->cap;
   } 

    if (capacity > buf->pool->small.chunkSize)
    {
        natsLarge *large = nats_palloc(buf->pool, sizeof(natsLarge), false);
        if (large == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        large->data = NATS_CALLOC(1, capacity);
        if (large->data == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);
        large->next = buf->large;
        buf->large = large;
        buf->data = large->data;
    }
    else
    {
        natsChunk *chunk = natsChain_AllocateChunk(&(buf->pool->small), capacity);
        if (chunk == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        buf->data = chunk->data;
        buf->small = chunk;
    }
    return s;
}

#endif /* defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_POOL) */

#endif /* MEMPOOL_H_ */
