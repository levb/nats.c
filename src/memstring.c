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

void
nats_strlow(uint8_t *dst, uint8_t *src, size_t n)
{
    while (n) {
        *dst = ngx_tolower(*src);
        dst++;
        src++;
        n--;
    }
}


size_t
nats_strnlen(uint8_t *p, size_t n)
{
    size_t  i;

    for (i = 0; i < n; i++) {

        if (p[i] == '\0') {
            return i;
        }
    }

    return n;
}


uint8_t *
nats_cpystrn(uint8_t *dst, uint8_t *src, size_t n)
{
    if (n == 0) {
        return dst;
    }

    while (--n) {
        *dst = *src;

        if (*dst == '\0') {
            return dst;
        }

        dst++;
        src++;
    }

    *dst = '\0';

    return dst;
}


uint8_t *
natsString_DupPool(natsPool *pool, natsString *src)
{
    uint8_t  *dst;

    dst = natsPool_Alloc(pool, src->len);
    if (dst == NULL) {
        return NULL;
    }

    memcpy(dst, src->data, src->len);

    return dst;
}

