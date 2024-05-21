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

void natsChunk_Destroy(natsChunk *c)
{
    if (c == NULL)
        return;

    if (c->data != NULL)
        NATS_FREE(c->data);

    NATS_FREE(c);
}

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize)
{
    natsChain *chain = NATS_CALLOC(1, sizeof(natsChain));
    if (chain == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    chain->newChunkSize = (chunkSize == 0 ? NATS_DEFAULT_NEW_CHUNK_SIZE : chunkSize);

    *newChain = chain;

    return NATS_OK;
}

void natsChain_Destroy(natsChain *chain)
{
    natsChunk *chunk = chain->head;
    natsChunk *next;

    while (chunk != NULL)
    {
        next = chunk->next;
        natsChunk_Destroy(chunk);
        chunk = next;
    }
    NATS_FREE(chain);
}

natsChunk *
natsChain_Alloc(natsChain *chain, size_t size)
{
    natsChunk *found = NULL;
    natsChunk *chunk = chain->head;

    if (size > chain->newChunkSize)
        return NULL;

    for (; chunk != NULL; chunk = chunk->next)
    {
        if (chunk->len + size <= chain->newChunkSize)
        {
            found = chunk;
            break;
        }
    }

    if (found == NULL)
    {
        found = NATS_CALLOC(1, sizeof(natsChunk));
        if (found == NULL)
            return NULL;

        found->data = NATS_CALLOC(1, chain->newChunkSize);
        if (found->data == NULL)
        {
            NATS_FREE(found);
            return NULL;
        }

        found->len = 0;
        found->next = chain->head;
        chain->head = found;
    }

    return found;
}

natsStatus
natsChain_AddChunkRef(natsChain *chain, uint8_t *data, size_t len)
{
    natsChunk *chunk = NATS_CALLOC(1, sizeof(natsChunk));
    if (chunk == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    chunk->data = data;
    chunk->len  = len;
    chunk->next = chain->head;
    chain->head = chunk;

    return NATS_OK;
}
