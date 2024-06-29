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

#ifndef MEM_POOL_H_
#define MEM_POOL_H_

#include "opts.h"

//----------------------------------------------------------------------------
//  natsPool - memory pool.
//
//  - Uses a linked lists of natsSmall for small memory allocations. Each
//    heap-allocated chunk is 1-page NATS_DEFAULT_MEM_PAGE_SIZE) sized.
//  - Maximum small allocation size is NATS_DEFAULT_MEM_PAGE_SIZE -
//    sizeof(natsSmall).
//  - Uses a linked list of natsLarge for large HEAP memory allocations. The
//    list elements are allocated in the (small) pool.
//  - natsPool_Destroy() will free all memory allocated in the pool.
//
//  +->+---------------+ 0       +------------->+---------------+
//  |  | natsSmall     |         |              | natsSmall     |
//  |  |  - next       |---------+              |  - next       |
//  |  |---------------+                        |---------------|
//  |  | natsPool      |                        |               |
//  +--|  - small      |                        |               |
//     |               |                        | used          |
//     |  - large      |---                     | memory        |
//     |               |                        |               |
//     | (most recent) |                        |               |
//     |---------------|                        |---------------| len
//     | used          |                        | free          |
//     | memory        |                        | memory        |
//     |               |                        |               |
//     |               |                        |               |
//  +->+---------------|                        |               |
//  |  | natsLarge #1  |                        |               |
//  |  |  - prev       |                        |               |
//  |  |  - mem  ======|=> HEAP                 |               |
//  |  |---------------|                        |               |
//  |  | natsLarge #2  |                        |               |
//  +--|  - prev       |                        |               |
//     |  - mem  ======|=> HEAP                 |               |
//     |---------------|                        |               |
//     | more          |                        |               |
//     | used          |                        |               |
//     | memory        |                        |               |
//     | ...           |                        |               |
//     |---------------| len                    |               |
//     | free          |                        |               |
//     | memory        |                        |               |
//     |               |                        |               |
//     +---------------+ page size              +---------------+ page size

struct __natsSmall_s
{
    struct __natsSmall_s *next;
    size_t len;
};

struct __natsLarge_s
{
    struct __natsLarge_s *prev;
    uint8_t *data;  // <>/<> rename to void *mem
};

struct __natsReadBuffer_s
{
    natsBytes buf; // must be first, so natsReadBuf* is also a natsString*
    struct __natsReadBuffer_s *next;
    uint8_t *readFrom;
};

struct __natsBuf_s
{
    natsBytes buf; // must be first, so natsBuf* is also a natsString* and natsBytes*

    size_t cap;
    natsPool *pool;
    natsSmall *small;
    natsLarge *large;
    bool isFixedSize;
};

// natsReadChain provides read buffer(s) to nats_ProcessReadEvent.
// While reading, we allocate or recycle the opPool every time a new operation
// is detected. The last read buffer from the previous operation may get
// recycled as the first read buffer of the new operation, including any
// leftover data in it.
struct __natsReadChain_s
{
    struct __natsReadBuffer_s *head;
    struct __natsReadBuffer_s *tail;
};

struct __natsPool_s
{
    int refs;
    natsMemOptions *opts;

    // small head is the first chunk allocated since that is where we attempt to
    // allocate first.
    natsSmall *small;

    // large head is the most recent large allocation, for simplicity.
    natsLarge *large;

    natsReadChain *readChain;

    const char *name; // stored as a pointer, not copied.
};

#define POOLTRACEx(module, fmt, ...)
#define POOLDEBUGf(fmt, ...)
#define POOLERRORf(fmt, ...)

#ifdef DEV_MODE_MEM_POOL

#ifdef DEV_MODE_MEM_POOL_TRACE
#undef POOLTRACEx
#define POOLTRACEx(module, fmt, ...) DEVLOGx(DEV_MODE_TRACE, module, file, line, func, fmt, __VA_ARGS__)
#endif // DEV_MODE_MEM_POOL_TRACE

#undef POOLDEBUGf
#define POOLDEBUGf(fmt, ...) DEVDEBUGf("POOL", fmt, __VA_ARGS__)
#undef POOLERRORf
#define POOLERRORf(fmt, ...) DEVERRORx("POOL", file, line, func, fmt, __VA_ARGS__)

#if defined(DEV_MODE) && !defined(MEM_POOL_C_)
// avoid using public functions internally, they don't pass the log context
#define nats_RetailPool(_p) USE_nats_retailPool_INSTEAD
#define nats_ReleasePool(_p) USE_nats_releasePool_INSTEAD
#define nats_CreatePool(_p, _h) USE_nats_createPool_INSTEAD
#define nats_Palloc(_p, _s) USE_nats_palloc_INSTEAD
#endif

#endif // DEV_MODE_MEM_POOL

//----------------------------------------------------------------------------
// Pool functions.

#define nats_createPool(_p, _opts, _n) nats_log_createPool((_p), (_opts), (_n)DEV_MODE_CTX)
natsStatus nats_log_createPool(natsPool **newPool, natsMemOptions *opts, const char *name DEV_MODE_ARGS);

#define nats_retainPool(_p) nats_log_retainPool((_p)DEV_MODE_CTX)
static inline natsPool *nats_log_retainPool(natsPool *pool DEV_MODE_ARGS)
{
    pool->refs++;
    POOLTRACEx("", "%s: retained, now %zu refs", pool->name, pool->refs);
    return pool;
}

#define nats_releasePool(_p) nats_log_releasePool((_p)DEV_MODE_CTX)
void nats_log_releasePool(natsPool *pool DEV_MODE_ARGS);

#define nats_palloc(_p, _s) nats_log_palloc((_p), _s DEV_MODE_CTX)
void *nats_log_palloc(natsPool *pool, size_t size DEV_MODE_ARGS);

#define nats_recyclePool(_pptr, _rbufptr) nats_log_recyclePool((_pptr), (_rbufptr)DEV_MODE_CTX)
natsStatus nats_log_recyclePool(natsPool **pool, natsReadBuffer **rbuf DEV_MODE_ARGS);

#define nats_recycleBuf(_buf) nats_log_recycleBuf((_buf)DEV_MODE_CTX)
void natsPool_log_recycleBuf(natsBuf *buf DEV_MODE_ARGS);

//----------------------------------------------------------------------------
// strdup-like helpers.

#define nats_pstrdup(_p, _s) nats_log_pstrdup((_p), (_s)DEV_MODE_CTX)
static inline char *nats_log_pstrdup(natsPool *pool, const char *str DEV_MODE_ARGS)
{
    if (str == NULL)
        return NULL;
    size_t len = unsafe_strlen(str);
    char *dup = nats_log_palloc(pool, len + 1 DEV_MODE_PASSARGS); // +1 for the terminating 0
    if (dup == NULL)
        return NULL;
    memcpy(dup, str, len);
    return dup;
}

#define nats_pdupn(_to, _pool, _from, _len) nats_log_pdupn((_to), (_pool), (_from), (_len)DEV_MODE_CTX)
static inline natsStatus nats_log_pdupn(natsBytes *to, natsPool *pool, const uint8_t *from, size_t len, bool padding DEV_MODE_ARGS)
{
    if (from == NULL)
    {
        nats_clearBytes(to);
        return NATS_OK;
    }
    to->bytes = nats_log_palloc(pool, len + padding DEV_MODE_PASSARGS); // +1 for the terminating 0 to be safe when converting to C strings
    if (to->bytes == NULL)
        return NATS_NO_MEMORY;
    memcpy(to->bytes, from, len);
    to->len = len;
    return NATS_OK;
}

#define nats_pdupBytes(_to, _pool, _from) nats_log_pdupBytes((_to), (_pool), (_from)DEV_MODE_CTX)
static inline natsStatus nats_log_pdupBytes(natsBytes *to, natsPool *pool, const natsBytes *from DEV_MODE_ARGS)
{
    return (from == NULL) ? ALWAYS_OK(nats_clearBytes(to)) : nats_log_pdupn(to, pool, from->bytes, from->len, false DEV_MODE_PASSARGS);
}

#define nats_pdupString(_to, _pool, _from) nats_log_pdupString((_to), (_pool), (_from)DEV_MODE_CTX)
static inline natsStatus nats_log_pdupString(natsString *to, natsPool *pool, const natsString *from DEV_MODE_ARGS)
{
    return (from == NULL) ? ALWAYS_OK(nats_clearString(to)) : nats_log_pdupn((natsBytes *)to, pool, (const uint8_t *)from->text, from->len, true DEV_MODE_PASSARGS);
}

#define nats_pdupStringFromC(_to, _pool, _from) nats_log_pdupStringFromC((_to), (_pool), (_from)DEV_MODE_CTX)
static inline natsStatus nats_log_pdupStringFromC(natsString *to, natsPool *pool, const char *str DEV_MODE_ARGS)
{
    return (str == NULL) ? ALWAYS_OK(nats_clearString(to)) : nats_log_pdupn((natsBytes *)to, pool, (const uint8_t *)str, safe_strlen(str), true DEV_MODE_PASSARGS);
}

//----------------------------------------------------------------------------
// Read buffer.

#define nats_readBufferData(_rbuf) ((_rbuf)->buf.bytes)
#define nats_readBufferLen(_rbuf) ((_rbuf)->buf.len)
#define nats_readBufferAvailable(_memopts, _rbuf) ((_memopts)->heapPageSize - (_rbuf)->buf.len)
#define nats_readBufferEnd(_rbuf) ((_rbuf)->buf.bytes + (_rbuf)->buf.len)
#define nats_readBufferUnreadLen(_rbuf) (nats_readBufferEnd(_rbuf) - (_rbuf)->readFrom)
#define nats_readBufferAsBytes(_rbuf) (&(_rbuf)->buf)
#define nats_readBufferAsString(_rbuf) ((nats_bytesAsString(&(_rbuf)->buf))

natsStatus nats_getReadBuffer(natsReadBuffer **rbuf, natsPool *pool);

//----------------------------------------------------------------------------
// Growable (pool-based) buffer.

#define nats_bufAvailable(b) ((b)->cap - (b)->buf.len)
#define nats_bufCapacity(b) ((b)->cap)
#define nats_bufData(b) ((b)->buf.bytes)
#define nats_bufLen(b) ((b)->buf.len)
#define nats_bufAsString(b) ((natsString *)&(b)->buf)
#define nats_bufAsBytes(b) (&(b)->buf)

natsStatus nats_getFixedBuf(natsBuf **newBuf, natsPool *pool, size_t cap);
natsStatus nats_getGrowableBuf(natsBuf **newBuf, natsPool *pool, size_t initialCap);
natsStatus nats_expandBuf(natsBuf *buf, size_t capacity);
void nats_log_recycleBuf(natsBuf *buf DEV_MODE_ARGS);
natsStatus nats_resetBuf(natsBuf *buf);
natsStatus nats_append(natsBuf *buf, const uint8_t *data, size_t len);
natsStatus nats_appendB(natsBuf *buf, uint8_t b);

// Does NOT add the terminating 0!
static inline natsStatus nats_appendCString(natsBuf *buf, const char *str)
{
    if (nats_strIsEmpty(str))
        return NATS_OK;
    return nats_append(buf, (const uint8_t *)str, unsafe_strlen(str)); // don't use nats_strlen, no need.
}

static inline natsStatus nats_appendC(natsBuf *buf, const char *str, size_t len)
{
    return nats_append(buf, (const uint8_t *)str, len);
}

static inline natsStatus nats_appendString(natsBuf *buf, const natsString *str)
{
    return nats_append(buf, (const uint8_t *)str->text, str->len);
}

static inline natsStatus nats_appendBytes(natsBuf *buf, const natsBytes *bb)
{
    return nats_append(buf, bb->bytes, bb->len);
}

#endif /* MEM_POOL_H_ */
