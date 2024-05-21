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
natsBuf_Reset(natsBuffer *buf)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    buf->len = 0;
    return NATS_OK;
}

natsStatus
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

natsStatus
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

natsStatus
natsBuf_InitCalloc(natsBuffer *buf, size_t capacity)
{
    if (buf == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);
    if (capacity == 0)
        capacity = NATS_DEFAULT_BUFFER_SIZE;
    memset(buf, 0, sizeof(natsBuffer));
    return natsBuf_Expand(buf, capacity);
}

natsStatus
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

natsStatus
natsBuf_CreatePool(natsBuffer **newBuf, natsPool *pool, size_t capacity)
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

natsStatus
natsBuf_Append(natsBuffer *buf, const uint8_t *data, size_t dataLen)
{
    natsStatus s = NATS_OK;
    size_t n;

    if (dataLen == (size_t)-1)
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

natsStatus
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

natsStatus natsBuf_Destroy(natsBuffer *buf)
{
    if (buf == NULL)
        return NATS_OK;

    if (buf->freeData)
        NATS_FREE(buf->data);

    if (buf->freeSelf)
        NATS_FREE(buf);

    return NATS_OK;
}
