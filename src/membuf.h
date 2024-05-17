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

#ifndef MEMBUF_H_
#define MEMBUF_H_

#include "mem.h"
#include "status.h"
#include "err.h"

#define natsBuf_Data(b) ((b)->data)
#define natsBuf_Capacity(b) ((b)->cap)
#define natsBuf_Len(b) ((b)->len)
#define natsBuf_Available(b) ((b)->cap - (b)->len)
#define natsBuf_MoveTo(b, pos) (b)->len = pos;

// Declarations only
#ifdef NATS_NO_INLINE_BUFFER

// Initializes an caller-allocated natsBuffer using 'data' as the back-end byte
// array. natsBuf_Destroy will not free 'data', it is the responsibility of the
// caller. natsBuf_Expand() may calloc a new underlying array if needed and that
// array will be freed in natsBuf_Destroy.
//
// One would use this call to initialize a natsBuffer without the added cost of
// allocating memory for the natsBuffer structure itself, for instance
// initializing an natsBuffer on the stack.
natsStatus
natsBuf_InitWith(natsBuffer *buf, uint8_t *data, size_t len, size_t cap);

// Initializes an caller-allocated natsBuffer with a new backend using calloc.
natsStatus
natsBuf_InitCalloc(natsBuffer *buf, size_t cap);

// Allocates a new natsBuffer using calloc.
natsStatus
natsBuf_CreateCalloc(natsBuffer **newBuf, size_t cap);

// Allocates a new natsBuffer using palloc.
natsStatus
natsBuf_CreatePalloc(natsBuffer **newBuf, natsPool *pool, size_t cap);

// Resets the buffer length to 0.
void natsBuf_Reset(natsBuffer *buf);

// Expands 'buf' underlying buffer to the given new size 'newSize'.
//
// If 'buf' did not own the underlying buffer, a new buffer is
// created and data copied over. The original data is now detached.
// The underlying buffer is now owned by 'buf' and will be freed when
// the natsBuffer is destroyed, if needed.
//
// When 'buf' owns the underlying buffer and it is expanded, a memory
// reallocation of the buffer occurs to satisfy the new size requirement.
//
// Note that one should not save the returned value of natsBuf_Data() and
// use it after any call to natsBuf_Expand/Append/AppendByte() since
// the memory address for the underlying byte buffer may have changed due
// to the buffer expansion.
natsStatus
natsBuf_Expand(natsBuffer *buf, int newSize);

// Appends 'dataLen' bytes from the 'data' byte array to the buffer,
// potentially expanding the buffer.
// See natsBuf_Expand for details about natsBuffer not owning the data.
natsStatus
natsBuf_Append(natsBuffer *buf, const char *data, int dataLen);

// Appends a byte to the buffer, potentially expanding the buffer.
// See natsBuf_Expand for details about natsBuffer not owning the data.
natsStatus
natsBuf_AppendByte(natsBuffer *buf, char b);

natsStatus
natsBuf_Destroy(natsBuffer *buf);

#endif /* NATS_NO_INLINE_BUFFER */

// Definitions
#if defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_BUFFER)

#ifdef NATS_MEM_C_
#define NATS_INLINE_BUFFER
#else
#define NATS_INLINE_BUFFER static inline
#endif

NATS_INLINE_BUFFER natsStatus
natsBuf_Reset(natsBuffer *buf)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    buf->len = 0;
    return NATS_OK;
}

NATS_INLINE_BUFFER natsStatus
natsBuf_InitWith(natsBuffer *buf, uint8_t *data, size_t len, size_t capacity)
{
    if ((buf == NULL) || (data == NULL))
        return nats_setDefaultError(NATS_INVALID_ARG);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;
    memset(buf, 0, sizeof(natsBuffer));
    buf->data = data;
    buf->len = len;
    buf->cap = capacity;
    return NATS_OK;
}

NATS_INLINE_BUFFER natsStatus
natsBuf_Expand(natsBuffer *buf, size_t capacity)
{
    uint8_t *newData = NULL;

    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;

    if (buf->pool != NULL)
        return natsPool_ExpandBuffer(buf, capacity);

    if (!buf->freeData)
    {
        newData = NATS_REALLOC(buf->data, capacity);
        if (newData == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);
    }
    else
    {
        newData = NATS_MALLOC(capacity);
        if (newData == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        memcpy(newData, buf->data, buf->len);
        buf->freeData = true;
    }

    if (buf->data != newData)
        buf->data = newData;
    buf->cap = capacity;
    return NATS_OK;
}

NATS_INLINE_BUFFER natsStatus
natsBuf_InitCalloc(natsBuffer *buf, size_t capacity)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;
    memset(buf, 0, sizeof(natsBuffer));
    return natsBuf_Expand(buf, capacity);
}

NATS_INLINE_BUFFER natsStatus
natsBuf_CreateCalloc(natsBuffer **newBuf, size_t capacity)
{
    natsBuffer *buf = NATS_CALLOC(1, sizeof(natsBuffer));
    if (buf == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;
    buf->freeSelf = true;

    natsStatus s = natsBuf_Expand(buf, capacity);
    if (s != NATS_OK)
    {
        NATS_FREE(buf);
        return s;
    }
    *newBuf = buf;
    return NATS_OK;
}

NATS_INLINE_BUFFER natsStatus
natsBuf_CreatePalloc(natsBuffer **newBuf, natsPool *pool, size_t capacity)
{
    natsBuffer *buf = natsPool_Alloc(pool, sizeof(natsBuffer));
    if (buf == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;
    buf->pool = pool;
    buf->len = 0;
    buf->cap = capacity;

    natsStatus s = natsBuf_Expand(buf, capacity);
    if (s != NATS_OK)
        return s;

    *newBuf = buf;
    return NATS_OK;
}

NATS_INLINE_BUFFER natsStatus
natsBuf_Append(natsBuffer *buf, const uint8_t *data, int dataLen)
{
    natsStatus s = NATS_OK;
    size_t n;

    if (dataLen == -1)
        dataLen = strlen((const char *)data);

    n = buf->len + (size_t)dataLen;

    if ((n < 0) || (n >= 0x7FFFFFFF))
        return nats_setDefaultError(NATS_NO_MEMORY);

    if (n > buf->cap)
    {
        // Increase by 10%
        size_t extra = (size_t)(n * 0.1);

        // Make sure that we have at least some bytes left after adding.
        n += extra < 64 ? 64 : extra;

        // Overrun.
        if (n >= 0x7FFFFFFF)
            return nats_setDefaultError(NATS_NO_MEMORY);

        s = natsBuf_Expand(buf, n);
    }

    if (s == NATS_OK)
    {
        memcpy(buf->data + buf->len, data, dataLen);
        buf->len += dataLen;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

NATS_INLINE_BUFFER natsStatus
natsBuf_AppendString(natsBuffer *buf, const char *str)
{
    return natsBuf_Append(buf, (const uint8_t *)str, -1);
}

NATS_INLINE_BUFFER natsStatus
natsBuf_AppendByte(natsBuffer *buf, uint8_t b)
{
    natsStatus s = NATS_OK;
    size_t n = buf->len + 1;

    if (n > buf->cap)
    {
        // Increase by 10%
        size_t extra = (size_t)(n * 0.1);

        // Make sure that we have at least some bytes left after adding.
        n += extra < 64 ? 64 : extra;

        // Overrun.
        if (n >= 0x7FFFFFFF)
            return nats_setDefaultError(NATS_NO_MEMORY);

        s = natsBuf_Expand(buf, n);
    }

    if (s == NATS_OK)
    {
        buf->data[buf->len] = b;
        buf->len++;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

NATS_INLINE_BUFFER void
natsBuf_Destroy(natsBuffer *buf)
{
    if (buf == NULL)
        return;

    if (buf->freeData)
        NATS_FREE(buf->data);

    if (buf->freeSelf)
        NATS_FREE(buf);
}

#endif /* defined(NATS_MEM_C_) || !defined(NATS_NO_INLINE_BUFFER) */

#endif /* MEMBUF_H_ */
