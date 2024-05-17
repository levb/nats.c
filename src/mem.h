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

// static inline uint8_t *nats_strstr(const uint8_t *s, const char *find)
// {
//     return strstr((const char *)s, find);
// }
// static inline size_t nats_strlen(const uint8_t *s) { return strlen((const char *)s); }
// static inline uint8_t *nats_strchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strchr((const char *)s, (int)find); }
// static inline uint8_t *nats_strrchr(const uint8_t *s, uint8_t find) { return (uint8_t *)strrchr((const char *)s, (int)find); }

#define NATS_MALLOC(s) malloc((s))
#define NATS_CALLOC(c, s) calloc((c), (s))
#define NATS_REALLOC(p, s) realloc((p), (s))

#ifdef _WIN32
#define NATS_STRDUP(s) _strdup((s))
#else
#define NATS_STRDUP(s) strdup((s))
#endif
#define NATS_FREE(p) free((p))

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

#define NATS_DEFAULT_CHUNK_SIZE 4096
#define NATS_DEFAULT_BUFFER_SIZE 256

typedef struct __natsChunk_s
{
    struct __natsChunk_s *next;
    uint8_t *data;
    size_t len;

} natsChunk;

typedef struct __natsChain_s
{
    natsChunk *head;
    size_t chunkSize;

} natsChain;

typedef struct __natsLarge_s
{
    struct __natsLarge_s *next;
    uint8_t *data;

} natsLarge;

typedef struct __natsPool_s
{
    natsChain small;
    natsLarge *large;

} natsPool;

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
typedef struct __natsBuffer_s
{
    uint8_t *data;

    size_t cap;
    size_t len;

    bool freeData;
    bool freeSelf;

    natsPool *pool;
    natsChunk *small;
    natsLarge *large;
} natsBuffer;

// Include function prototypes/inline definitions.
#include "memchain.h"
#include "mempool.h"
#include "membuf.h"
#include "memstring.h"

#endif /* MEM_H_ */
