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

#ifndef MEMCHAIN_H_
#define MEMCHAIN_H_

#include "mem.h"
#include "status.h"
#include "err.h"

// Declarations.
#ifdef NATS_NO_INLINE_CHAIN

void natsChunk_Destroy(natsChunk *c);

natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize);
void natsChain_Destroy(natsChain *chain);

#endif // NATS_NO_INLINE_CHAINS

// Definitions.
#if defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_CHAIN)

#ifdef NATS_MEM_C_
#define NATS_INLINE_CHAIN
#else
#define NATS_INLINE_CHAIN static inline
#endif

NATS_INLINE_CHAIN void natsChunk_Destroy(natsChunk *c)
{
    if (c == NULL)
        return;

    if (c->data != NULL)
        NATS_FREE(c->data);

    NATS_FREE(c);
}

NATS_INLINE_CHAIN natsStatus natsChain_Create(natsChain **newChain, size_t chunkSize)
{
    natsChain *chain = NATS_CALLOC(1, sizeof(natsChain));
    if (chain == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    chain->chunkSize = (chunkSize == 0 ? NATS_DEFAULT_CHUNK_SIZE : chunkSize);

    *newChain = chain;

    return NATS_OK;
}

NATS_INLINE_CHAIN void natsChain_Destroy(natsChain *chain)
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

NATS_INLINE_CHAIN natsChunk*
natsChain_AllocateChunk(natsChain *chain, size_t size)
{
    natsChunk *found = NULL;
    natsChunk *chunk = chain->head;

    if (size > chain->chunkSize)
        return NULL;

    for(; chunk != NULL; chunk = chunk->next)
    {
        if (chunk->len + size <= chain->chunkSize)
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

        found->data = NATS_CALLOC(1, chain->chunkSize);
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

#endif /* defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_CHAIN) */

#endif /* MEMCHAIN_H_ */
