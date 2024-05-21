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
natsPool_Create(natsPool **newPool, size_t chunkSize, bool init)
{
    natsPool *pool = NULL;

    pool = NATS_CALLOC(1, sizeof(natsPool));
    if (pool == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    pool->small.newChunkSize = (chunkSize == 0 ? NATS_DEFAULT_NEW_CHUNK_SIZE : chunkSize);
    *newPool = pool;

    if (init)
    {
        // Ensure allocation of the first chunk, but ignore the result.
        natsChain_Alloc(&(pool->small), 0);
    }

    return NATS_OK;
}

void natsPool_UndoLast(natsPool *pool, void *mem)
{
    if (pool == NULL)
        return;

    if ((pool->large != NULL) && (pool->large->data == mem))
    {
        pool->large = pool->large->next;
        NATS_FREE(mem);
    }
    else if ((pool->small.head != NULL) && ((uint8_t *)mem >= pool->small.head->data) && ((uint8_t *)mem < (pool->small.head->data + pool->small.newChunkSize)))
    {
        pool->small.head->len = (const uint8_t *)mem - (const uint8_t *)pool->small.head->data;
    }
}

void *
nats_palloc(natsPool *pool, size_t size, bool takeAll)
{
    natsChunk *chunk = NULL;
    natsLarge *large = NULL;
    void *mem = NULL;

    if (size > pool->small.newChunkSize)
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
        chunk = natsChain_Alloc(&(pool->small), size);
        if (chunk == NULL)
            return NULL;

        mem = chunk->data + chunk->len;
        if (takeAll)
            chunk->len = pool->small.newChunkSize;
        else
            chunk->len += size;

        return mem;
    }
}

natsStatus
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

    if (capacity > buf->pool->small.newChunkSize)
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
        natsChunk *chunk = natsChain_Alloc(&(buf->pool->small), capacity);
        if (chunk == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        buf->data = chunk->data;
        buf->small = chunk;
    }
    return s;
}

void natsPool_Destroy(natsPool *pool)
{
    natsLarge *l;
    if (pool == NULL)
        return;

    for (l = pool->large; l != NULL; l = l->next)
    {
        NATS_FREE(l->data);
    }

    natsChain_Destroy(&(pool->small));

    NATS_FREE(pool);
}
