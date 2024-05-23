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

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize)
{
    if (chunkSize == 0)
        chunkSize = NATS_DEFAULT_NEW_CHUNK_SIZE;

    size_t headSize = sizeof(natsChain) + sizeof(natsChunk);
    if (chunkSize < headSize)
        return nats_setDefaultError(NATS_INVALID_ARG);

    uint8_t *mem = NATS_CALLOC(1, chunkSize);
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

    // The first chunk is part of the chain's initial allocation and has its 'prev' set to NULL, so do not free it
    while (chunk != NULL && chunk->prev != NULL)
    {
        natsChunk *prev = chunk->prev;
        NATS_FREE(chunk);
        chunk = prev;
    }
    NATS_FREE(chain);
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

    printf("<>/<> found %p\n", (void*)use);
    uint8_t *mem = _chunk_mem_ptr(use);
    if (use == NULL)
    {
        mem = NATS_CALLOC(1, chain->chunkSize);
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

// natsStatus
// natsChain_AddChunkRef(natsChain *chain, uint8_t *data, size_t len)
// {
//     natsChunk *chunk = NATS_CALLOC(1, sizeof(natsChunk));
//     if (chunk == NULL)
//         return nats_setDefaultError(NATS_NO_MEMORY);

//     chunk->data = data;
//     chunk->len = len;
//     chunk->next = chain->head;
//     chain->head = chunk;

//     return NATS_OK;
// }
