// Copyright 2015-2022 The NATS Authors
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

#ifndef NATSP_H_
#define NATSP_H_

#if defined(_WIN32)
#include "include/n-win.h"
#else
#include "include/n-unix.h"
#endif

#include "nats.h"
#include "dev_mode.h"

#define SSL void *
#define SSL_free(c) \
    {               \
        (c) = NULL; \
    }
#define SSL_CTX void *
#define SSL_CTX_free(c) \
    {                   \
        (c) = NULL;     \
    }
#define NO_SSL_ERR "The library was built without SSL support!"

#define LIB_NATS_VERSION_STRING NATS_VERSION_STRING
#define LIB_NATS_VERSION_NUMBER NATS_VERSION_NUMBER
#define LIB_NATS_VERSION_REQUIRED_NUMBER NATS_VERSION_REQUIRED_NUMBER

#define CString "C"

#define IFOK(s, c)    \
    if (s == NATS_OK) \
    {                 \
        s = (c);      \
    }

#define NATS_MILLIS_TO_NANOS(d) (((int64_t)d) * (int64_t)1E6)
#define NATS_SECONDS_TO_NANOS(d) (((int64_t)d) * (int64_t)1E9)

#define _CRLF_ "\r\n"
#define _SPC_ " "

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

//-----------------------------------------------------------------------------
// Library

void natsSys_Init(void);
void natsLib_Retain(void);
void natsLib_Release(void);
int64_t nats_setTargetTime(int64_t timeout);

//-----------------------------------------------------------------------------
// Types

typedef struct __natsBuffer_s natsBuffer;
typedef struct __natsHash natsHash;
typedef struct __natsHashIter natsHashIter;
typedef struct __natsLarge_s natsLarge;
typedef struct __natsParser natsParser;
typedef struct __natsPong natsPong;
typedef struct __natsPongList natsPongList;
typedef struct __natsPool_s natsPool;
typedef struct __natsChain_s natsChain;
typedef struct __natsServer_s natsServer;
typedef struct __natsServerInfo natsServerInfo;
typedef struct __natsServers_s natsServers;
typedef struct __natsSmall_s natsSmall;
typedef struct __natsSockCtx natsSockCtx;
typedef struct __natsStrHash natsStrHash;
typedef struct __natsStrHashIter natsStrHashIter;
typedef struct __natsString_s natsString;

#endif /* NATSP_H_ */
