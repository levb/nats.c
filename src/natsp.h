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
# include "include/n-win.h"
#else
# include "include/n-unix.h"
#endif

#define SSL             void*
#define SSL_free(c)     { (c) = NULL; }
#define SSL_CTX         void*
#define SSL_CTX_free(c) { (c) = NULL; }
#define NO_SSL_ERR  "The library was built without SSL support!"

#include "err.h"
#include "nats.h"
#include "parser.h"
#include "timer.h"
#include "url.h"
#include "srvpool.h"
#include "hash.h"
#include "natstime.h"
#include "nuid.h"
#include "mem.h"
#include "string.h"

// Comment/uncomment to replace some function calls with direct structure
// access
//#define DEV_MODE    (1)

#define LIB_NATS_VERSION_STRING             NATS_VERSION_STRING
#define LIB_NATS_VERSION_NUMBER             NATS_VERSION_NUMBER
#define LIB_NATS_VERSION_REQUIRED_NUMBER    NATS_VERSION_REQUIRED_NUMBER

#define CString     "C"

#define _OK_OP_     "+OK"
#define _ERR_OP_    "-ERR"
#define _MSG_OP_    "MSG"
#define _PING_OP_   "PING"
#define _PONG_OP_   "PONG"
#define _INFO_OP_   "INFO"

#define _CRLF_      "\r\n"
#define _SPC_       " "
#define _HPUB_P_    "HPUB "

#define _PING_PROTO_         "PING\r\n"
#define _PONG_PROTO_         "PONG\r\n"
#define _SUB_PROTO_          "SUB %s %s %" PRId64 "\r\n"
#define _UNSUB_PROTO_        "UNSUB %" PRId64 " %d\r\n"
#define _UNSUB_NO_MAX_PROTO_ "UNSUB %" PRId64 " \r\n"

#define STALE_CONNECTION            "Stale Connection"
#define PERMISSIONS_ERR             "Permissions Violation"
#define AUTHORIZATION_ERR           "Authorization Violation"
#define AUTHENTICATION_EXPIRED_ERR  "User Authentication Expired"

#define _CRLF_LEN_          (2)
#define _SPC_LEN_           (1)
#define _HPUB_P_LEN_        (5)
#define _PING_OP_LEN_       (4)
#define _PONG_OP_LEN_       (4)
#define _PING_PROTO_LEN_    (6)
#define _PONG_PROTO_LEN_    (6)
#define _OK_OP_LEN_         (3)
#define _ERR_OP_LEN_        (4)

#define NATS_DEFAULT_INBOX_PRE      "_INBOX."
#define NATS_DEFAULT_INBOX_PRE_LEN  (7)

#define NATS_MAX_REQ_ID_LEN (19) // to display 2^63-1 number

#define WAIT_FOR_READ       (0)
#define WAIT_FOR_WRITE      (1)
#define WAIT_FOR_CONNECT    (2)

#define MAX_FRAMES (50)

#define ERR_CODE_AUTH_EXPIRED   (1)
#define ERR_CODE_AUTH_VIOLATION (2)

// This is temporary until we remove original connection status enum
// values without NATS_CONN_STATUS_ prefix
#if defined(NATS_CONN_STATUS_NO_PREFIX)
#define NATS_CONN_STATUS_DISCONNECTED   DISCONNECTED
#define NATS_CONN_STATUS_CONNECTING     CONNECTING
#define NATS_CONN_STATUS_CONNECTED      CONNECTED
#define NATS_CONN_STATUS_CLOSED         CLOSED
#define NATS_CONN_STATUS_RECONNECTING   RECONNECTING
#define NATS_CONN_STATUS_DRAINING_SUBS  DRAINING_SUBS
#define NATS_CONN_STATUS_DRAINING_PUBS  DRAINING_PUBS
#endif

#define IFOK(s, c)      if (s == NATS_OK) { s = (c); }

#define NATS_MILLIS_TO_NANOS(d)     (((int64_t)d)*(int64_t)1E6)
#define NATS_SECONDS_TO_NANOS(d)    (((int64_t)d)*(int64_t)1E9)

typedef struct __natsControl
{
    char    *op;
    char    *args;

} natsControl;

typedef struct __natsServerInfo
{
    char        *id;
    char        *host;
    int         port;
    char        *version;
    bool        authRequired;
    bool        tlsRequired;
    bool        tlsAvailable;
    int64_t     maxPayload;
    char        **connectURLs;
    int         connectURLsCount;
    int         proto;
    uint64_t    CID;
    char        *nonce;
    char        *clientIP;
    bool        lameDuckMode;
    bool        headers;

} natsServerInfo;

typedef struct
{
    natsEvLoop_Attach           attach;
    natsEvLoop_ReadAddRemove    read;
    natsEvLoop_WriteAddRemove   write;
    natsEvLoop_Detach           detach;

} natsEvLoopCallbacks;

struct __natsOptions
{
    // This field must be the first (see natsOptions_clone, same if you add
    // allocated fields such as strings).
    char                    *url;
    char                    **servers;
    int                     serversCount;
    bool                    noRandomize;
    int64_t                 timeout;
    char                    *name;
    bool                    verbose;
    bool                    pedantic;
    bool                    allowReconnect;
    bool                    secure;
    int                     ioBufSize;
    int                     maxReconnect;
    int64_t                 reconnectWait;
    int                     reconnectBufSize;
    int64_t                 writeDeadline;

    char                    *user;
    char                    *password;
    char                    *token;

    bool                    ignoreDiscoveredServers;

    int64_t                 pingInterval;
    int                     maxPingsOut;
    int                     maxPendingMsgs;
    int64_t                 maxPendingBytes;

    void                    *evLoop;
    natsEvLoopCallbacks     evCbs;

    bool                    libMsgDelivery;

    int                     orderIP; // possible values: 0,4,6,46,64

    // forces the old method of Requests that utilize
    // a new Inbox and a new Subscription for each request
    bool                    useOldRequestStyle;

    // If set to true, the Publish call will flush in place and
    // not rely on the flusher.
    bool                    sendAsap;

    // If set to true, pending requests will fail with NATS_CONNECTION_DISCONNECTED
    // when the library detects a disconnection.
    bool                    failRequestsOnDisconnect;

    // NoEcho configures whether the server will echo back messages
    // that are sent on this connection if we also have matching subscriptions.
    // Note this is supported on servers >= version 1.2. Proto 1 or greater.
    bool                    noEcho;

    // If set to true, in case of failed connect, tries again using
    // reconnect options values.
    bool                    retryOnFailedConnect;

    // Reconnect jitter added to reconnect wait
    int64_t                 reconnectJitter;
    int64_t                 reconnectJitterTLS;

    // Disable the "no responders" feature.
    bool disableNoResponders;

    // Custom message payload padding size
    int payloadPaddingSize;
};

typedef struct __natsPong
{
    int64_t             id;

    struct __natsPong   *prev;
    struct __natsPong   *next;

} natsPong;

typedef struct __natsPongList
{
    natsPong            *head;
    natsPong            *tail;

    int64_t             incoming;
    int64_t             outgoingPings;

    natsPong            cached;

    // natsCondition       *cond;

} natsPongList;

typedef struct __natsSockCtx
{
    natsSock        fd;
    bool            fdActive;

    int             orderIP; // possible values: 0,4,6,46,64

    // By default, the list of IPs returned by the hostname resolution will
    // be shuffled. This option, if `true`, will disable the shuffling.
    bool            noRandomize;

} natsSockCtx;

struct __natsConnection
{
    natsOptions         *opts;
    natsSrv             *cur;

    int                 refs;

    natsSockCtx         sockCtx;

    natsSrvPool         *srvPool;

    natsPool            *pool;
    natsBuffer          *scratch;
    natsChain *out;
    natsChain *in;

    natsServerInfo      info;

    int64_t             ssid;

    natsConnStatus      status;
    natsStatus          err;
    char                errStr[256];

    natsParser          *ps;

    natsPongList        pongs;

    struct
    {
        bool            attached;
        bool            writeAdded;
        void            *buffer;
        void            *data;
    } el;

    // Server version
    struct
    {
        int             ma;
        int             mi;
        int             up;
    } srvVersion;
};

//
// Library
//

void
natsSys_Init(void);

void
natsLib_Retain(void);

void
natsLib_Release(void);

int64_t
nats_setTargetTime(int64_t timeout);

#endif /* NATSP_H_ */
