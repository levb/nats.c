// Copyright 2024 The NATS Authors
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

#include <ctype.h>

void nats_strlow(uint8_t *dst, uint8_t *src, size_t n)
{
    while (n)
    {
        *dst = nats_toLower(*src);
        dst++;
        src++;
        n--;
    }
}

size_t
nats_strnlen(uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {

        if (p[i] == '\0')
        {
            return i;
        }
    }

    return n;
}

uint8_t *
nats_cpystrn(uint8_t *dst, uint8_t *src, size_t n)
{
    if (n == 0)
    {
        return dst;
    }

    while (--n)
    {
        *dst = *src;

        if (*dst == '\0')
        {
            return dst;
        }

        dst++;
        src++;
    }

    *dst = '\0';

    return dst;
}

natsStatus nats_strtoUint64(uint64_t *result, const uint8_t *d, size_t len)
{
    uint64_t v = 0;

    for (size_t i = 0; i < len; i++)
    {
        if ((d[i] < '0') || (d[i] > '9'))
        {
            return nats_setErrorf(NATS_ERR, "invalid number: %.*s", (int)len, d);
        }

        v = v * 10 + (d[i] - '0');
    }

    if (result != NULL)
        *result = v;
    return NATS_OK;
}

#ifdef DEV_MODE

#define PRINTBUF_SIZE 128
#define NUM_PRINT_BUFFERS 10
static char _printbuf[NUM_PRINT_BUFFERS][PRINTBUF_SIZE];
static int _printbufIndex = 0;

const char *nats_printableU(const uint8_t *data, size_t len, size_t limit)
{
    if (data == NULL)
        return "<null>";
    if (limit == 0)
        limit = len;
    if (len > limit)
        len = limit;
    const uint8_t *end = data + len;

    char *out = _printbuf[_printbufIndex++ % NUM_PRINT_BUFFERS];
    const size_t maxbuf = PRINTBUF_SIZE - 1;
    size_t i = 0;
    for (const uint8_t *p = data; (p < end) && (i < maxbuf); p++)
    {
        if (isprint(*p))
        {
            out[i++] = *p;
        }
        else if (*p == '\n')
        {
            out[i++] = '\\';
            out[i++] = 'n';
        }
        else if (*p == '\r')
        {
            out[i++] = '\\';
            out[i++] = 'r';
        }
        else
        {
            out[i++] = '?';
        }

        if (i >= maxbuf - 3)
        {
            out[i++] = '.';
            out[i++] = '.';
            out[i++] = '.';
        }
    }
    out[i] = '\0';
    return out;
}



#endif // DEV_MODE
