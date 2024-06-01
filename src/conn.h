// Copyright 2015-2021 The NATS Authors
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

#ifndef CONN_H_
#define CONN_H_

#include "comsock.h"

#define _OK_OP_ "+OK"
#define _ERR_OP_ "-ERR"
#define _MSG_OP_ "MSG"
#define _PING_OP_ "PING"
#define _PONG_OP_ "PONG"
#define _INFO_OP_ "INFO"

#define _HPUB_P_ "HPUB "

#define _PING_PROTO_ "PING\r\n"
#define _PONG_PROTO_ "PONG\r\n"
#define _SUB_PROTO_ "SUB %s %s %" PRId64 "\r\n"
#define _UNSUB_PROTO_ "UNSUB %" PRId64 " %d\r\n"
#define _UNSUB_NO_MAX_PROTO_ "UNSUB %" PRId64 " \r\n"

#define STALE_CONNECTION "Stale Connection"
#define PERMISSIONS_ERR "Permissions Violation"
#define AUTHORIZATION_ERR "Authorization Violation"
#define AUTHENTICATION_EXPIRED_ERR "User Authentication Expired"

#define _CRLF_LEN_ (2)
#define _SPC_LEN_ (1)
#define _HPUB_P_LEN_ (5)
#define _PING_OP_LEN_ (4)
#define _PONG_OP_LEN_ (4)
#define _PING_PROTO_LEN_ (6)
#define _PONG_PROTO_LEN_ (6)
#define _OK_OP_LEN_ (3)
#define _ERR_OP_LEN_ (4)

#define NATS_DEFAULT_INBOX_PRE "_INBOX."
#define NATS_DEFAULT_INBOX_PRE_LEN (7)

#define NATS_MAX_REQ_ID_LEN (19) // to display 2^63-1 number

#define ERR_CODE_AUTH_EXPIRED (1)
#define ERR_CODE_AUTH_VIOLATION (2)

// This is temporary until we remove original connection status enum
// values without NATS_CONN_STATUS_ prefix
#if defined(NATS_CONN_STATUS_NO_PREFIX)
#define NATS_CONN_STATUS_DISCONNECTED DISCONNECTED
#define NATS_CONN_STATUS_CONNECTING CONNECTING
#define NATS_CONN_STATUS_CONNECTED CONNECTED
#define NATS_CONN_STATUS_CLOSED CLOSED
#define NATS_CONN_STATUS_RECONNECTING RECONNECTING
#define NATS_CONN_STATUS_DRAINING_SUBS DRAINING_SUBS
#define NATS_CONN_STATUS_DRAINING_PUBS DRAINING_PUBS
#endif

typedef struct
{
    natsEvLoop_Attach attach;
    natsEvLoop_ReadAddRemove read;
    natsEvLoop_WriteAddRemove write;
    natsEvLoop_Detach detach;

} natsEvLoopCallbacks;

struct __natsPong
{
    int64_t id;

    struct __natsPong *prev;
    struct __natsPong *next;

};

struct __natsPongList
{
    natsPong *head;
    natsPong *tail;

    int64_t incoming;
    int64_t outgoingPings;

    natsPong cached;

    // natsCondition       *cond;

};

struct __natsControl
{
    natsString op;
    natsString args;

};

struct __natsServerInfo
{
    const char *id;
    const char *host;
    int port;
    const char *version;
    bool authRequired;
    bool tlsRequired;
    bool tlsAvailable;
    int64_t maxPayload;
    const char **connectURLs;
    int connectURLsCount;
    int proto;
    uint64_t CID;
    const char *nonce;
    const char *clientIP;
    bool lameDuckMode;
    bool headers;

};

struct __natsConnection
{
    natsOptions *opts;
    natsSrv *cur;

    int refs;

    natsSockCtx sockCtx;

    natsSrvPool *srvPool;

    // pool exists (and grows) for the lifetime of connection.
    natsPool *pool;

    natsBuffer *scratch;

    // This is the buffer used to accumulate data to write to the socket.
    natsChain *out;
    natsChain *in;

    natsPool *infoPool;
    natsServerInfo *info;

    int64_t ssid;

    natsConnStatus status;
    natsStatus err;
    char errStr[256];

    natsParser *ps;

    natsPongList pongs;

    struct
    {
        bool attached;
        bool writeAdded;
        void *buffer;
        void *data;
    } el;

    // Server version
    struct
    {
        int ma;
        int mi;
        int up;
    } srvVersion;
};

#define RESP_INFO_POOL_MAX_SIZE (10)

#define SET_WRITE_DEADLINE(nc) if ((nc)->opts->writeDeadline > 0) natsDeadline_Init(&(nc)->sockCtx.writeDeadline, (nc)->opts->writeDeadline)

natsStatus
natsConn_create(natsConnection **newConn, natsOptions *options);

void
natsConn_retain(natsConnection *nc);

void
natsConn_release(natsConnection *nc);

natsStatus
natsConn_bufferWrite(natsConnection *nc, const char *buffer, int len);

natsStatus
natsConn_bufferFlush(natsConnection *nc);

bool
natsConn_isClosed(natsConnection *nc);

bool
natsConn_isReconnecting(natsConnection *nc);

natsStatus
natsConn_flushOrKickFlusher(natsConnection *nc);

natsStatus
natsConn_processMsg(natsConnection *nc, char *buf, int bufLen);

void
natsConn_processOK(natsConnection *nc);

void
natsConn_processErr(natsConnection *nc, char *buf, int bufLen);

void
natsConn_processPing(natsConnection *nc);

void
natsConn_processPong(natsConnection *nc);

#define natsConn_subscribeNoPool(sub, nc, subj, cb, closure)                            natsConn_subscribeImpl((sub), (nc), true, (subj), NULL, 0, (cb), (closure), true, NULL)
#define natsConn_subscribeNoPoolNoLock(sub, nc, subj, cb, closure)                      natsConn_subscribeImpl((sub), (nc), false, (subj), NULL, 0, (cb), (closure), true, NULL)
#define natsConn_subscribeSyncNoPool(sub, nc, subj)                                     natsConn_subscribeNoPool((sub), (nc), (subj), NULL, NULL)
#define natsConn_subscribeWithTimeout(sub, nc, subj, timeout, cb, closure)              natsConn_subscribeImpl((sub), (nc), true, (subj), NULL, (timeout), (cb), (closure), false, NULL)
#define natsConn_subscribe(sub, nc, subj, cb, closure)                                  natsConn_subscribeWithTimeout((sub), (nc), (subj), 0, (cb), (closure))
#define natsConn_subscribeSync(sub, nc, subj)                                           natsConn_subscribe((sub), (nc), (subj), NULL, NULL)
#define natsConn_queueSubscribeWithTimeout(sub, nc, subj, queue, timeout, cb, closure)  natsConn_subscribeImpl((sub), (nc), true, (subj), (queue), (timeout), (cb), (closure), false, NULL)
#define natsConn_queueSubscribe(sub, nc, subj, queue, cb, closure)                      natsConn_queueSubscribeWithTimeout((sub), (nc), (subj), (queue), 0, (cb), (closure))
#define natsConn_queueSubscribeSync(sub, nc, subj, queue)                               natsConn_queueSubscribe((sub), (nc), (subj), (queue), NULL, NULL)

void
natsConn_processAsyncINFO(natsConnection *nc, char *buf, int len);

// natsStatus
// natsConn_publish(natsConnection *nc, natsMsg *msg, const char *reply, bool directFlush);

bool
natsConn_srvVersionAtLeast(natsConnection *nc, int major, int minor, int update);

void
natsConn_close(natsConnection *nc);

void
natsConn_destroy(natsConnection *nc, bool fromPublicDestroy);

#endif /* CONN_H_ */
