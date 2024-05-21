// Copyright 2015-2023 The NATS Authors
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
#include "conn.h"
#include "parser.h"
#include "err.h"
#include "url.h"
#include "srvpool.h"
#include "opts.h"
#include "util.h"

// CLIENT_PROTO_ZERO is the original client protocol from 2009.
// http://nats.io/documentation/internals/nats-protocol/
#define CLIENT_PROTO_ZERO (0)

// CLIENT_PROTO_INFO signals a client can receive more then the original INFO block.
// This can be used to update clients on other cluster members, etc.
#define CLIENT_PROTO_INFO (1)

#define DEFAULT_SCRATCH_SIZE (512)
#define DEFAULT_SCRATCH_SIZE (512)

#define MAX_INFO_MESSAGE_SIZE (32768)
#define DEFAULT_FLUSH_TIMEOUT (10000)

#define NATS_EVENT_ACTION_ADD (true)
#define NATS_EVENT_ACTION_REMOVE (false)

#ifdef DEV_MODE

#define _retain(c) natsConn_retain(c)
#define _release(c) natsConn_release(c)

#else
// We know what we are doing :-)

#define _retain(c) ((c)->refs++)
#define _release(c) ((c)->refs--)

#endif // DEV_MODE

static natsStatus _createConn(natsConnection *nc);
static natsStatus _processConnInit(natsConnection *nc);
static void _close(natsConnection *nc, natsConnStatus status, bool fromPublicClose, bool doCBs);
static natsStatus _processExpectedInfo(natsConnection *nc);
static natsStatus _sendConnect(natsConnection *nc);
static void _clearSSL(natsConnection *nc);
static natsStatus _evStopPolling(natsConnection *nc);
static natsStatus _readOp(natsConnection *nc, natsControl *control);
static natsStatus _processInfo(natsConnection *nc, char *info, int len);
static natsStatus _checkForSecure(natsConnection *nc);
static natsStatus _connectProto(natsConnection *nc, char **proto);
static natsStatus _readProto(natsConnection *nc, natsBuffer **proto);
static bool _processOpError(natsConnection *nc, natsStatus s, bool initialConnect);

    static void _clearServerInfo(natsServerInfo *si);
static void _freeConn(natsConnection *nc);

#define natsConn_bufferWriteStr(_nc, _s) natsConn_bufferWrite((_nc), (_s), strlen(_s));

void natsConnection_ProcessReadEvent(natsConnection *nc)
{
    natsStatus s = NATS_OK;
    int n = 0;
    char *buffer;
    int size;

    if (!(nc->el.attached) || (nc->sockCtx.fd == NATS_SOCK_INVALID))
    {
        return;
    }

    if (nc->ps == NULL)
    {
        s = natsParser_Create(&(nc->ps));
        if (s != NATS_OK)
        {
            (void)NATS_UPDATE_ERR_STACK(s);
            return;
        }
    }

    _retain(nc);

    buffer = nc->el.buffer;
    size = nc->opts->ioBufSize;

    // Do not try to read again here on success. If more than one connection
    // is attached to the same loop, and there is a constant stream of data
    // coming for the first connection, this would starve the second connection.
    // So return and we will be called back later by the event loop.
    s = natsSock_Read(&(nc->sockCtx), buffer, size, &n);
    // if (s == NATS_OK)
    //     s = natsParser_Parse(nc->ps, nc, buffer, n);

    if (s != NATS_OK)
        _processOpError(nc, s, false);

    // natsConn_release(nc);
}

void natsConnection_ProcessWriteEvent(natsConnection *nc)
{
    natsStatus s = NATS_OK;
    int n = 0;
    // int len;

    if (nc->sockCtx.fd == NATS_SOCK_INVALID)
        return;

    // natsChain *out = nc->out;

    // buf = natsBuf_Data(nc->bw);
    // len = natsBuf_Len(nc->bw);

    s = natsSock_Write(&(nc->sockCtx), "<>/<> TEST", 10, &n);
    if (s == NATS_OK)
    {
        // if (n == len)
        // {
        //     // We sent all the data, reset buffer and remove WRITE event.
        //     natsBuf_Reset(nc->bw);

        //     s = nc->opts->evCbs.write(nc->el.data, NATS_EVENT_ACTION_REMOVE);
        //     if (s == NATS_OK)
        //         nc->el.writeAdded = false;
        //     else
        //         nats_setError(s, "Error processing write request: %d - %s",
        //                       s, natsStatus_GetText(s));
        // }
        // else
        // {
        //     // We sent some part of the buffer. Move the remaining at the beginning.
        //     natsBuf_Consume(nc->bw, n);
        // }
    }

    if (s != NATS_OK)
        _processOpError(nc, s, false);

    (void)NATS_UPDATE_ERR_STACK(s);
}

// Main connect function. Will connect to the server
static natsStatus
_connect(natsConnection *nc)
{
    natsStatus s = NATS_OK;
    natsStatus retSts = NATS_OK;
    natsSrvPool *srvPool = nc->srvPool;
    int i = 0;
    int l = 0;
    int max = 0;
    int64_t wtime = 0;
    bool retry = false;

    if (nc->opts->retryOnFailedConnect)
    {
        retry = true;
        max = nc->opts->maxReconnect;
        wtime = nc->opts->reconnectWait;
    }

    for (;;)
    {
        // The pool may change inside the loop iteration due to INFO protocol.
        for (i = 0; i < natsSrvPool_GetSize(srvPool); i++)
        {
            nc->cur = natsSrvPool_GetSrv(srvPool, i);
            printf("<>/<> connecting to %s\n", nc->cur->url->fullUrl);

            s = _createConn(nc);
            if (s == NATS_OK)
            {
                printf("<>/<> proceeding to process connection init\n");
                s = _processConnInit(nc);

                if (s == NATS_OK)
                {
                    nc->cur->lastAuthErrCode = 0;
                    natsSrvPool_SetSrvDidConnect(srvPool, i, true);
                    natsSrvPool_SetSrvReconnects(srvPool, i, 0);
                    retSts = NATS_OK;
                    retry = false;
                    break;
                }
                else
                {
                    retSts = s;

                    _close(nc, NATS_CONN_STATUS_DISCONNECTED, false, false);

                    nc->cur = NULL;
                }
            }
            else
            {
                if (natsConn_isClosed(nc))
                {
                    s = NATS_CONNECTION_CLOSED;
                    break;
                }

                if (s == NATS_IO_ERROR)
                    retSts = NATS_OK;
            }
        }

        if (!retry)
            break;

        l++;
        if ((max > 0) && (l > max))
            break;

        if (wtime > 0)
            nats_Sleep(wtime);
    }

    if ((retSts == NATS_OK) && (nc->status != NATS_CONN_STATUS_CONNECTED))
        s = nats_setDefaultError(NATS_NO_SERVER);

    return NATS_UPDATE_ERR_STACK(s);
}

// _createConn will connect to the server and do the right thing when an
// existing connection is in place.
static natsStatus
_createConn(natsConnection *nc)
{
    natsStatus s = NATS_OK;

    // Set the IP resolution order
    nc->sockCtx.orderIP = nc->opts->orderIP;

    // Set ctx.noRandomize based on public NoRandomize option.
    nc->sockCtx.noRandomize = nc->opts->noRandomize;

    s = natsSock_ConnectTcp(&(nc->sockCtx), nc->pool, nc->cur->url->host, nc->cur->url->port);
    if (s == NATS_OK)
        nc->sockCtx.fdActive = true;

    // Need to create or reset the buffer even on failure in case we allow
    // retry on failed connect
    if ((s == NATS_OK) || nc->opts->retryOnFailedConnect)
    {
        natsStatus ls = NATS_OK;
        printf("<>/<> TCP connected to %s\n", nc->cur->url->fullUrl);

        // natsChain_Destroy(nc->out); <>/<>
        ls = natsChain_Create(&(nc->out), 0);
        if (s == NATS_OK)
            s = ls;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_processConnInit(natsConnection *nc)
{
    natsStatus s = NATS_OK;

    nc->status = NATS_CONN_STATUS_CONNECTING;

    // Process the INFO protocol that we should be receiving
    s = _processExpectedInfo(nc);

    // Send the CONNECT and PING protocol, and wait for the PONG.
    if (s == NATS_OK)
        s = _sendConnect(nc);

    // If there is no write deadline option, switch to blocking socket here...
    if ((s == NATS_OK) && (nc->opts->writeDeadline <= 0))
        s = natsSock_SetBlocking(nc->sockCtx.fd, true);

        s = natsSock_SetBlocking(nc->sockCtx.fd, false);

        // If we are reconnecting, buffer will have already been allocated
        if ((s == NATS_OK) && (nc->el.buffer == NULL))
        {
            nc->el.buffer = (char *)malloc(nc->opts->ioBufSize);
            if (nc->el.buffer == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);
        }
        if (s == NATS_OK)
        {
            s = nc->opts->evCbs.attach(&(nc->el.data),
                                       nc->opts->evLoop,
                                       nc,
                                       (int)nc->sockCtx.fd);
            if (s == NATS_OK)
            {
                nc->el.attached = true;
            }
            else
            {
                nats_setError(s,
                              "Error attaching to the event loop: %d - %s",
                              s, natsStatus_GetText(s));
            }
        }

    return NATS_UPDATE_ERR_STACK(s);
}

// Low level close call that will do correct cleanup and set
// desired status. Also controls whether user defined callbacks
// will be triggered. The lock should not be held entering this
// function. This function will handle the locking manually.
static void
_close(natsConnection *nc, natsConnStatus status, bool fromPublicClose, bool doCBs)
{
    natsConn_retain(nc);

    if (natsConn_isClosed(nc))
    {
        nc->status = status;

        natsConn_release(nc);
        return;
    }

    nc->status = NATS_CONN_STATUS_CLOSED;

    _evStopPolling(nc);

    natsSock_Close(nc->sockCtx.fd);
    nc->sockCtx.fd = NATS_SOCK_INVALID;

    // We need to cleanup some things if the connection was SSL.
    _clearSSL(nc);
    nc->sockCtx.fdActive = false;

    // natsAsyncCb_PostConnHandler(nc, ASYNC_DISCONNECTED);
    // natsAsyncCb_PostConnHandler(nc, ASYNC_CLOSED);

    nc->status = status;

    nc->opts->evCbs.detach(nc->el.data);
    natsConn_release(nc);
}

static natsStatus
_processExpectedInfo(natsConnection *nc)
{
    natsControl control;
    natsStatus s;

    // _initControlContent(&control);

    s = _readOp(nc, &control);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    if ((s == NATS_OK) && ((control.op == NULL) || (strcmp(control.op, _INFO_OP_) != 0)))
    {
        s = nats_setError(NATS_PROTOCOL_ERROR,
                          "Unexpected protocol: got '%s' instead of '%s'",
                          (control.op == NULL ? "<null>" : control.op),
                          _INFO_OP_);
    }
    if (s == NATS_OK)
        s = _processInfo(nc, control.args, -1);
    if (s == NATS_OK)
        s = _checkForSecure(nc);

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_sendConnect(natsConnection *nc)
{
    natsStatus s = NATS_OK;
    // char *cProto = NULL;
    // natsBuffer *proto = NULL;

    // // Create the CONNECT protocol
    // s = _connectProto(nc, &cProto);

    // // Add it to the buffer
    // if (s == NATS_OK)
    //     s = natsConn_bufferWriteStr(nc, cProto);

    // // Add the PING protocol to the buffer
    // if (s == NATS_OK)
    //     s = natsConn_bufferWrite(nc, _PING_OP_, _PING_OP_LEN_);
    // if (s == NATS_OK)
    //     s = natsConn_bufferWrite(nc, _CRLF_, _CRLF_LEN_);

    // // Flush the buffer
    // if (s == NATS_OK)
    //     s = natsConn_bufferFlush(nc);

    // // Now read the response from the server.
    // if (s == NATS_OK)
    //     s = _readProto(nc, &proto);

    // // If Verbose is set, we expect +OK first.
    // if ((s == NATS_OK) && nc->opts->verbose)
    // {
    //     // Check protocol is as expected
    //     if (strncmp(natsBuf_Data(proto), _OK_OP_, _OK_OP_LEN_) != 0)
    //     {
    //         s = nats_setError(NATS_PROTOCOL_ERROR,
    //                           "Expected '%s', got '%s'",
    //                           _OK_OP_, natsBuf_Data(proto));
    //     }
    //     natsBuf_Destroy(proto);
    //     proto = NULL;

    //     // Read the rest now...
    //     if (s == NATS_OK)
    //         s = _readProto(nc, &proto);
    // }

    // // We except the PONG protocol
    // if ((s == NATS_OK) && (strncmp(natsBuf_Data(proto), _PONG_OP_, _PONG_OP_LEN_) != 0))
    // {
    //     // But it could be something else, like -ERR

    //     if (strncmp(natsBuf_Data(proto), _ERR_OP_, _ERR_OP_LEN_) == 0)
    //     {
    //         char buffer[256];
    //         int authErrCode = 0;

    //         buffer[0] = '\0';
    //         snprintf_truncate(buffer, sizeof(buffer), "%s", natsBuf_Data(proto));

    //         // Remove -ERR, trim spaces and quotes.
    //         nats_NormalizeErr(buffer);

    //         // Look for auth errors.
    //         if ((authErrCode = _checkAuthError(buffer)) != 0)
    //         {
    //             // This sets nc->err to NATS_CONNECTION_AUTH_FAILED
    //             // copy content of buffer into nc->errStr.
    //             _processAuthError(nc, authErrCode, buffer);
    //             s = nc->err;
    //         }
    //         else
    //             s = NATS_ERR;

    //         // Update stack
    //         s = nats_setError(s, "%s", buffer);
    //     }
    //     else
    //     {
    //         s = nats_setError(NATS_PROTOCOL_ERROR,
    //                           "Expected '%s', got '%s'",
    //                           _PONG_OP_, natsBuf_Data(proto));
    //     }
    // }
    // // Destroy proto (ok if proto is NULL).
    // natsBuf_Destroy(proto);

    // if (s == NATS_OK)
    //     nc->status = NATS_CONN_STATUS_CONNECTED;

    // NATS_FREE(cProto);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsConn_bufferFlush(natsConnection *nc)
{
    natsStatus s = NATS_OK;
    if (nc->el.writeAdded)
        return NATS_OK;

    nc->el.writeAdded = true;
    s = nc->opts->evCbs.write(nc->el.data, NATS_EVENT_ACTION_ADD);
    if (s != NATS_OK)
        nats_setError(s, "Error processing write request: %d - %s",
                      s, natsStatus_GetText(s));

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsConn_bufferWrite(natsConnection *nc, const char *buffer, int len)
{
    return natsConn_bufferFlush(nc);

    // natsStatus s = NATS_OK;
    // int offset = 0;
    // int avail = 0;

    // if (len <= 0)
    //     return NATS_OK;

    //     s = natsBuf_Append(nc->bw, buffer, len);
    //     if ((s == NATS_OK) && (natsBuf_Len(nc->bw) >= nc->opts->ioBufSize) && !(nc->el.writeAdded))
    //     {
    //         nc->el.writeAdded = true;
    //         s = nc->opts->evCbs.write(nc->el.data, NATS_EVENT_ACTION_ADD);
    //         if (s != NATS_OK)
    //             nats_setError(s, "Error processing write request: %d - %s",
    //                           s, natsStatus_GetText(s));
    //     }

    //     return NATS_UPDATE_ERR_STACK(s);
}

static bool
_isConnecting(natsConnection *nc)
{
    return nc->status == NATS_CONN_STATUS_CONNECTING;
}

static bool
_isConnected(natsConnection *nc)
{
    return ((nc->status == NATS_CONN_STATUS_CONNECTED)); //  || natsConn_isDraining(nc));
}

bool natsConn_isClosed(natsConnection *nc)
{
    return nc->status == NATS_CONN_STATUS_CLOSED;
}

static natsStatus
_readOp(natsConnection *nc, natsControl *control)
{
    natsStatus s = NATS_OK;
    char buffer[MAX_INFO_MESSAGE_SIZE];

    buffer[0] = '\0';

    printf("<>/<> !!!!!!! %d\n", s);
    s = natsSock_ReadLine(&(nc->sockCtx), buffer, sizeof(buffer));
    printf("<>/<> !!!!!!! %d\n", s);
    if (s == NATS_OK)
        s = nats_ParseControl(control, buffer);

    return NATS_UPDATE_ERR_STACK(s);
}

static void
_unpackSrvVersion(natsConnection *nc)
{
    nc->srvVersion.ma = 0;
    nc->srvVersion.mi = 0;
    nc->srvVersion.up = 0;

    if (nats_IsStringEmpty(nc->info.version))
        return;

    sscanf(nc->info.version, "%d.%d.%d", &(nc->srvVersion.ma), &(nc->srvVersion.mi), &(nc->srvVersion.up));
}

bool natsConn_srvVersionAtLeast(natsConnection *nc, int major, int minor, int update)
{
    bool ok;
    ok = (((nc->srvVersion.ma > major) || ((nc->srvVersion.ma == major) && (nc->srvVersion.mi > minor)) || ((nc->srvVersion.ma == major) && (nc->srvVersion.mi == minor) && (nc->srvVersion.up >= update))) ? true : false);
    return ok;
}

// _processInfo is used to parse the info messages sent
// from the server.
// This function may update the server pool.
static natsStatus
_processInfo(natsConnection *nc, char *info, int len)
{
    natsStatus s = NATS_OK;
    nats_JSON *json = NULL;
    // bool        postDiscoveredServersCb = false;
    // bool        postLameDuckCb = false;

    if (info == NULL)
        return NATS_OK;

    // postDiscoveredServersCb = (nc->opts->discoveredServersCb != NULL);
    // postLameDuckCb = (nc->opts->lameDuckCb != NULL);

    _clearServerInfo(&(nc->info));

    s = nats_JSONParse(&json, info, len);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    IFOK(s, nats_JSONGetStr(json, "server_id", &(nc->info.id)));
    IFOK(s, nats_JSONGetStr(json, "version", &(nc->info.version)));
    IFOK(s, nats_JSONGetStr(json, "host", &(nc->info.host)));
    IFOK(s, nats_JSONGetInt(json, "port", &(nc->info.port)));
    IFOK(s, nats_JSONGetBool(json, "auth_required", &(nc->info.authRequired)));
    IFOK(s, nats_JSONGetBool(json, "tls_required", &(nc->info.tlsRequired)));
    IFOK(s, nats_JSONGetBool(json, "tls_available", &(nc->info.tlsAvailable)));
    IFOK(s, nats_JSONGetLong(json, "max_payload", &(nc->info.maxPayload)));
    IFOK(s, nats_JSONGetArrayStr(json, "connect_urls",
                                 &(nc->info.connectURLs),
                                 &(nc->info.connectURLsCount)));
    IFOK(s, nats_JSONGetInt(json, "proto", &(nc->info.proto)));
    IFOK(s, nats_JSONGetULong(json, "client_id", &(nc->info.CID)));
    IFOK(s, nats_JSONGetStr(json, "nonce", &(nc->info.nonce)));
    IFOK(s, nats_JSONGetStr(json, "client_ip", &(nc->info.clientIP)));
    IFOK(s, nats_JSONGetBool(json, "ldm", &(nc->info.lameDuckMode)));
    IFOK(s, nats_JSONGetBool(json, "headers", &(nc->info.headers)));

    if (s == NATS_OK)
        _unpackSrvVersion(nc);

    // The array could be empty/not present on initial connect,
    // if advertise is disabled on that server, or servers that
    // did not include themselves in the async INFO protocol.
    // If empty, do not remove the implicit servers from the pool.
    if ((s == NATS_OK) && !nc->opts->ignoreDiscoveredServers && (nc->info.connectURLsCount > 0))
    {
        bool added = false;
        const char *tlsName = NULL;

        if ((nc->cur != NULL) && (nc->cur->url != NULL) && !nats_HostIsIP(nc->cur->url->host))
            tlsName = (const char *)nc->cur->url->host;

        s = natsSrvPool_addNewURLs(nc->srvPool,
                                   nc->cur->url,
                                   nc->info.connectURLs,
                                   nc->info.connectURLsCount,
                                   tlsName,
                                   &added);
        // if ((s == NATS_OK) && added && !nc->initc && postDiscoveredServersCb) <>//<>
        //     natsAsyncCb_PostConnHandler(nc, ASYNC_DISCOVERED_SERVERS);
    }
    // Process the LDM callback after the above. It will cover cases where
    // we have connect URLs and invoke discovered server callback, and case
    // where we don't.
    // if ((s == NATS_OK) && nc->info.lameDuckMode && postLameDuckCb) <>//<>
    //     natsAsyncCb_PostConnHandler(nc, ASYNC_LAME_DUCK_MODE);

    if (s != NATS_OK)
        s = nats_setError(NATS_PROTOCOL_ERROR,
                          "Invalid protocol: %s", nats_GetLastError(NULL));

    nats_JSONDestroy(json);

    return NATS_UPDATE_ERR_STACK(s);
}

// makeTLSConn will wrap an existing Conn using TLS
static natsStatus
_makeTLSConn(natsConnection *nc)
{
    return NATS_OK;
}

// This will check to see if the connection should be
// secure. This can be dictated from either end and should
// only be called after the INIT protocol has been received.
static natsStatus
_checkForSecure(natsConnection *nc)
{
    natsStatus s = NATS_OK;

    // Check for mismatch in setups
    if (nc->opts->secure && !nc->info.tlsRequired && !nc->info.tlsAvailable)
        s = nats_setDefaultError(NATS_SECURE_CONNECTION_WANTED);
    else if (nc->info.tlsRequired && !nc->opts->secure)
    {
        // Switch to Secure since server needs TLS.
        s = natsOptions_SetSecure(nc->opts, true);
    }

    if ((s == NATS_OK) && nc->opts->secure)
        s = _makeTLSConn(nc);

    return NATS_UPDATE_ERR_STACK(s);
}

// static char *
// _escape(char *origin)
// {
//     char escChar[] = {'\a', '\b', '\f', '\n', '\r', '\t', '\v', '\\'};
//     char escRepl[] = {'a', 'b', 'f', 'n', 'r', 't', 'v', '\\'};
//     int l = (int)strlen(origin);
//     int ec = 0;
//     char *dest = NULL;
//     char *ptr = NULL;
//     int i;
//     int j;

//     for (i = 0; i < l; i++)
//     {
//         for (j = 0; j < 8; j++)
//         {
//             if (origin[i] == escChar[j])
//             {
//                 ec++;
//                 break;
//             }
//         }
//     }
//     if (ec == 0)
//         return origin;

//     dest = NATS_MALLOC(l + ec + 1);
//     if (dest == NULL)
//         return NULL;

//     ptr = dest;
//     for (i = 0; i < l; i++)
//     {
//         for (j = 0; j < 8; j++)
//         {
//             if (origin[i] == escChar[j])
//             {
//                 *ptr++ = '\\';
//                 *ptr++ = escRepl[j];
//                 break;
//             }
//         }
//         if (j == 8)
//             *ptr++ = origin[i];
//     }
//     *ptr = '\0';
//     return dest;
// }

static natsStatus
_connectProto(natsConnection *nc, char **proto)
{
    natsStatus s = NATS_OK;
    natsOptions *opts = nc->opts;
    const char *token = NULL;
    const char *user = NULL;
    const char *pwd = NULL;
    const char *name = NULL;
    char *sig = NULL;
    char *ujwt = NULL;
    char *nkey = NULL;
    int res;
    // unsigned char *sigRaw = NULL;
    // int sigRawLen = 0;

    // Check if NoEcho is set and we have a server that supports it.
    if (opts->noEcho && (nc->info.proto < 1))
        return NATS_NO_SERVER_SUPPORT;

    if (nc->cur->url->username != NULL)
        user = nc->cur->url->username;
    if (nc->cur->url->password != NULL)
        pwd = nc->cur->url->password;
    if ((user != NULL) && (pwd == NULL))
    {
        token = user;
        user = NULL;
    }
    if ((user == NULL) && (token == NULL))
    {
        // Take from options (possibly all NULL)
        user = opts->user;
        pwd = opts->password;
        token = opts->token;
        // nkey = opts->nkey;

        // Options take precedence for an implicit URL. If above is still
        // empty, we will check if we have saved a user from an explicit
        // URL in the server pool.
        if (nats_IsStringEmpty(user) && nats_IsStringEmpty(token) && (nc->srvPool->user != NULL))
        {
            user = nc->srvPool->user;
            pwd = nc->srvPool->pwd;
            // Again, if there is no password, assume username is token.
            if (pwd == NULL)
            {
                token = user;
                user = NULL;
            }
        }
    }

    // if (opts->userJWTHandler != NULL)
    // {
    //     char *errTxt = NULL;
    //     bool userCb = opts->userJWTHandler != natsConn_userCreds;

    //     // If callback is not the internal one, we need to release connection lock.
    //     if (userCb)
    //         natsConn_Unlock(nc);

    //     s = opts->userJWTHandler(&ujwt, &errTxt, (void *)opts->userJWTClosure);

    //     if (userCb)
    //     {
    //         // natsConn_Lock(nc);
    //         if (natsConn_isClosed(nc) && (s == NATS_OK))
    //             s = NATS_CONNECTION_CLOSED;
    //     }
    //     if ((s != NATS_OK) && (errTxt != NULL))
    //     {
    //         s = nats_setError(s, "%s", errTxt);
    //         NATS_FREE(errTxt);
    //     }
    //     if ((s == NATS_OK) && !nats_IsStringEmpty(nkey))
    //         s = nats_setError(NATS_ILLEGAL_STATE, "%s", "user JWT callback and NKey cannot be both specified");

    //     if ((s == NATS_OK) && (ujwt != NULL))
    //     {
    //         char *tmp = _escape(ujwt);
    //         if (tmp == NULL)
    //         {
    //             s = nats_setDefaultError(NATS_NO_MEMORY);
    //         }
    //         else if (tmp != ujwt)
    //         {
    //             NATS_FREE(ujwt);
    //             ujwt = tmp;
    //         }
    //     }
    // }

    // if ((s == NATS_OK) && (!nats_IsStringEmpty(ujwt) || !nats_IsStringEmpty(nkey)))
    // {
    //     char *errTxt = NULL;
    //     bool userCb = opts->sigHandler != natsConn_signatureHandler;

    //     if (userCb)
    //         // natsConn_Unlock(nc);

    //         s = opts->sigHandler(&errTxt, &sigRaw, &sigRawLen, nc->info.nonce, opts->sigClosure);

    //     if (userCb)
    //     {
    //         // natsConn_Lock(nc);
    //         if (natsConn_isClosed(nc) && (s == NATS_OK))
    //             s = NATS_CONNECTION_CLOSED;
    //     }
    //     if ((s != NATS_OK) && (errTxt != NULL))
    //     {
    //         s = nats_setError(s, "%s", errTxt);
    //         NATS_FREE(errTxt);
    //     }
    //     if (s == NATS_OK)
    //         s = nats_Base64RawURL_EncodeString((const unsigned char *)sigRaw, sigRawLen, &sig);
    // }

    // if ((s == NATS_OK) && (opts->tokenCb != NULL))
    // {
    //     if (token != NULL)
    //         s = nats_setError(NATS_ILLEGAL_STATE, "%s", "Token and token handler options cannot be both set");

    //     if (s == NATS_OK)
    //         token = opts->tokenCb(opts->tokenCbClosure);
    // }

    if ((s == NATS_OK) && (opts->name != NULL))
        name = opts->name;

    if (s == NATS_OK)
    {
        // If our server does not support headers then we can't do them or no responders.
        const char *hdrs = nats_GetBoolStr(nc->info.headers);
        const char *noResponders = nats_GetBoolStr(nc->info.headers && !nc->opts->disableNoResponders);

        res = nats_asprintf(proto,
                            "CONNECT {\"verbose\":%s,\"pedantic\":%s,%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\"tls_required\":%s,"
                            "\"name\":\"%s\",\"lang\":\"%s\",\"version\":\"%s\",\"protocol\":%d,\"echo\":%s,"
                            "\"headers\":%s,\"no_responders\":%s}%s",
                            nats_GetBoolStr(opts->verbose),
                            nats_GetBoolStr(opts->pedantic),
                            (nkey != NULL ? "\"nkey\":\"" : ""),
                            (nkey != NULL ? nkey : ""),
                            (nkey != NULL ? "\"," : ""),
                            (ujwt != NULL ? "\"jwt\":\"" : ""),
                            (ujwt != NULL ? ujwt : ""),
                            (ujwt != NULL ? "\"," : ""),
                            (sig != NULL ? "\"sig\":\"" : ""),
                            (sig != NULL ? sig : ""),
                            (sig != NULL ? "\"," : ""),
                            (user != NULL ? "\"user\":\"" : ""),
                            (user != NULL ? user : ""),
                            (user != NULL ? "\"," : ""),
                            (pwd != NULL ? "\"pass\":\"" : ""),
                            (pwd != NULL ? pwd : ""),
                            (pwd != NULL ? "\"," : ""),
                            (token != NULL ? "\"auth_token\":\"" : ""),
                            (token != NULL ? token : ""),
                            (token != NULL ? "\"," : ""),
                            nats_GetBoolStr(opts->secure),
                            (name != NULL ? name : ""),
                            CString, NATS_VERSION_STRING,
                            CLIENT_PROTO_INFO,
                            nats_GetBoolStr(!opts->noEcho),
                            hdrs,
                            noResponders,
                            _CRLF_);
        if (res < 0)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }

    // NATS_FREE(ujwt);
    // NATS_FREE(sigRaw);
    // NATS_FREE(sig);

    return s;
}

natsStatus
natsConn_sendUnsubProto(natsConnection *nc, int64_t subId, int max)
{
    natsStatus s = NATS_OK;
    // char *proto = NULL;
    // int res = 0;

    // if (max > 0)
    //     res = nats_asprintf(&proto, _UNSUB_PROTO_, subId, max);
    // else
    //     res = nats_asprintf(&proto, _UNSUB_NO_MAX_PROTO_, subId);

    // if (res < 0)
    //     s = nats_setDefaultError(NATS_NO_MEMORY);
    // else
    // {
    //     s = natsConn_bufferWriteStr(nc, proto);
    //     NATS_FREE(proto);
    // }

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsConn_sendSubProto(natsConnection *nc, const char *subject, const char *queue, int64_t sid)
{
    natsStatus s = NATS_OK;
    // char *proto = NULL;
    // int res = 0;

    // res = nats_asprintf(&proto, _SUB_PROTO_, subject, (queue == NULL ? "" : queue), sid);
    // if (res < 0)
    //     s = nats_setDefaultError(NATS_NO_MEMORY);
    // else
    // {
    //     s = natsConn_bufferWriteStr(nc, proto);
    //     NATS_FREE(proto);
    //     proto = NULL;
    // }
    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_flushReconnectPendingItems(natsConnection *nc)
{
    natsStatus s = NATS_OK;

    // if (nc->pending == NULL)
    //     return NATS_OK;

    // if (natsBuf_Len(nc->pending) > 0)
    // {
    //     // Flush pending buffer
    //     s = natsConn_bufferWrite(nc, natsBuf_Data(nc->pending),
    //                              natsBuf_Len(nc->pending));

    //     // Regardless of outcome, we must clear the pending buffer
    //     // here to avoid duplicates (if the flush were to fail
    //     // with some messages/partial messages being sent).
    //     natsBuf_Reset(nc->pending);
    // }

    return s;
}

static void
_removePongFromList(natsConnection *nc, natsPong *pong)
{
    if (pong->prev != NULL)
        pong->prev->next = pong->next;

    if (pong->next != NULL)
        pong->next->prev = pong->prev;

    if (nc->pongs.head == pong)
        nc->pongs.head = pong->next;

    if (nc->pongs.tail == pong)
        nc->pongs.tail = pong->prev;

    pong->prev = pong->next = NULL;
}

natsStatus
natsConn_initInbox(natsConnection *nc, char *buf, int bufSize, char **newInbox, bool *allocated)
{
    // int needed = nc->inboxPfxLen + NUID_BUFFER_LEN + 1;
    // char *inbox = buf;
    // bool created = false;
    // natsStatus s;

    // if (needed > bufSize)
    // {
    //     inbox = NATS_MALLOC(needed);
    //     if (inbox == NULL)
    //         return nats_setDefaultError(NATS_NO_MEMORY);
    //     created = true;
    // }
    // memcpy(inbox, nc->inboxPfx, nc->inboxPfxLen);
    // // This will add the terminal '\0';
    // s = natsNUID_Next(inbox + nc->inboxPfxLen, NUID_BUFFER_LEN + 1);
    // if (s == NATS_OK)
    // {
    //     *newInbox = inbox;
    //     if (allocated != NULL)
    //         *allocated = created;
    // }
    // else if (created)
    //     NATS_FREE(inbox);

    // return s;

    return NATS_OK;
}

// natsStatus
// natsConn_newInbox(natsConnection *nc, natsInbox **newInbox)
// {
//     natsStatus s;
//     int inboxLen = nc->inboxPfxLen + NUID_BUFFER_LEN + 1;
//     char *inbox = NATS_MALLOC(inboxLen);

//     if (inbox == NULL)
//         return nats_setDefaultError(NATS_NO_MEMORY);

//     s = natsConn_initInbox(nc, inbox, inboxLen, (char **)newInbox, NULL);
//     if (s != NATS_OK)
//         NATS_FREE(inbox);
//     return s;
// }

static void
_clearSSL(natsConnection *nc)
{
    // if (nc->sockCtx.ssl == NULL)
    //     return;

    // SSL_free(nc->sockCtx.ssl);
    // nc->sockCtx.ssl = NULL;
}

// reads a protocol one byte at a time.
static natsStatus
_readProto(natsConnection *nc, natsBuffer **proto)
{
    // natsStatus s = NATS_OK;
    // char protoEnd = '\n';
    // natsBuffer *buf = NULL;
    // char oneChar[1] = {'\0'};

    // s = natsBuf_Create(&buf, 10);
    // if (s != NATS_OK)
    //     return NATS_UPDATE_ERR_STACK(s);

    // for (;;)
    // {
    //     s = natsSock_Read(&(nc->sockCtx), oneChar, 1, NULL);
    //     if (s != NATS_OK)
    //     {
    //         natsBuf_Destroy(buf);
    //         return NATS_UPDATE_ERR_STACK(s);
    //     }
    //     s = natsBuf_AppendByte(buf, oneChar[0]);
    //     if (s != NATS_OK)
    //     {
    //         natsBuf_Destroy(buf);
    //         return NATS_UPDATE_ERR_STACK(s);
    //     }
    //     if (oneChar[0] == protoEnd)
    //         break;
    // }
    // s = natsBuf_AppendByte(buf, '\0');
    // if (s != NATS_OK)
    // {
    //     natsBuf_Destroy(buf);
    //     return NATS_UPDATE_ERR_STACK(s);
    // }
    // *proto = buf;
    return NATS_OK;
}

static natsStatus
_evStopPolling(natsConnection *nc)
{
    natsStatus s;

    nc->el.writeAdded = false;
    s = nc->opts->evCbs.read(nc->el.data, NATS_EVENT_ACTION_REMOVE);
    if (s == NATS_OK)
        s = nc->opts->evCbs.write(nc->el.data, NATS_EVENT_ACTION_REMOVE);

    return s;
}

// _processOpError handles errors from reading or parsing the protocol.
// The lock should not be held entering this function.
static bool
_processOpError(natsConnection *nc, natsStatus s, bool initialConnect)
{
    // natsConn_Lock(nc);

    // if (!initialConnect)
    // {
    //     if (_isConnecting(nc) || natsConn_isClosed(nc) || (nc->inReconnect > 0))
    //     {
    //         natsConn_Unlock(nc);

    //         return false;
    //     }
    // }

    // // Do reconnect only if allowed and we were actually connected
    // // or if we are retrying on initial failed connect.
    // if (initialConnect || (nc->opts->allowReconnect && (nc->status == NATS_CONN_STATUS_CONNECTED)))
    // {
    //     natsStatus ls = NATS_OK;

    //     // Set our new status
    //     nc->status = NATS_CONN_STATUS_RECONNECTING;

    //     if (nc->ptmr != NULL)
    //         natsTimer_Stop(nc->ptmr);

    //     if (nc->sockCtx.fdActive)
    //     {
    //         SET_WRITE_DEADLINE(nc);
    //         natsConn_bufferFlush(nc);

    //         natsSock_Shutdown(nc->sockCtx.fd);
    //         nc->sockCtx.fdActive = false;
    //     }

    //     // If we use an external event loop, we need to stop polling
    //     // on the socket since we are going to reconnect.
    //     if (nc->el.attached)
    //     {
    //         ls = _evStopPolling(nc);
    //         natsSock_Close(nc->sockCtx.fd);
    //         nc->sockCtx.fd = NATS_SOCK_INVALID;

    //         // We need to cleanup some things if the connection was SSL.
    //         _clearSSL(nc);
    //     }

    //     // Fail pending flush requests.
    //     if (ls == NATS_OK)
    //         _clearPendingFlushRequests(nc);
    //     // If option set, also fail pending requests.
    //     if ((ls == NATS_OK) && nc->opts->failRequestsOnDisconnect)
    //         _clearPendingRequestCalls(nc, NATS_CONNECTION_DISCONNECTED);

    //     // Create the pending buffer to hold all write requests while we try
    //     // to reconnect.
    //     if (ls == NATS_OK)
    //         ls = natsBuf_Create(&(nc->pending), nc->opts->reconnectBufSize);
    //     if (ls == NATS_OK)
    //     {
    //         nc->usePending = true;

    //         // Start the reconnect thread
    //         ls = natsThread_Create(&(nc->reconnectThread),
    //                               _doReconnect, (void*) nc);
    //     }
    //     if (ls == NATS_OK)
    //     {
    //         // We created the reconnect thread successfully, so retain
    //         // the connection.
    //         _retain(nc);
    //         nc->inReconnect++;
    //         natsConn_Unlock(nc);

    //         return true;
    //     }
    // }

    // // reconnect not allowed or we failed to setup the reconnect code.

    // nc->status = NATS_CONN_STATUS_DISCONNECTED;
    // nc->err = s;

    // natsConn_Unlock(nc);

    _close(nc, NATS_CONN_STATUS_CLOSED, false, true);

    return false;
}

// static void
// _readLoop(void *arg)
// {
//     natsStatus s = NATS_OK;
//     char *buffer;
//     int n;
//     int bufSize;

//     natsConnection *nc = (natsConnection *)arg;

//     bufSize = nc->opts->ioBufSize;
//     buffer = NATS_MALLOC(bufSize);
//     if (buffer == NULL)
//         s = nats_setDefaultError(NATS_NO_MEMORY);

//     if (nc->sockCtx.ssl != NULL)
//         nats_sslRegisterThreadForCleanup();

//     natsDeadline_Clear(&(nc->sockCtx.readDeadline));

//     if (nc->ps == NULL)
//         s = natsParser_Create(&(nc->ps));

//     while ((s == NATS_OK) && !natsConn_isClosed(nc) && !natsConn_isReconnecting(nc))
//     {
//         n = 0;

//         s = natsSock_Read(&(nc->sockCtx), buffer, bufSize, &n);
//         if ((s == NATS_IO_ERROR) && (NATS_SOCK_GET_ERROR == NATS_SOCK_WOULD_BLOCK))
//             s = NATS_OK;
//         if ((s == NATS_OK) && (n > 0))
//             s = natsParser_Parse(nc->ps, nc, buffer, n);

//         if (s != NATS_OK)
//             _processOpError(nc, s, false);
//     }

//     NATS_FREE(buffer);

//     natsSock_Close(nc->sockCtx.fd);
//     nc->sockCtx.fd = NATS_SOCK_INVALID;
//     nc->sockCtx.fdActive = false;

//     // We need to cleanup some things if the connection was SSL.
//     _clearSSL(nc);

//     natsParser_Destroy(nc->ps);
//     nc->ps = NULL;

//     // This unlocks and releases the connection to compensate for the retain
//     // when this thread was created.
//     natsConn_release(nc);
// }

static void
_sendPing(natsConnection *nc, natsPong *pong)
{
    // natsStatus s = NATS_OK;

    // SET_WRITE_DEADLINE(nc);
    // s = natsConn_bufferWrite(nc, _PING_PROTO_, _PING_PROTO_LEN_);
    // if (s == NATS_OK)
    // {
    //     // Flush the buffer in place.
    //     s = natsConn_bufferFlush(nc);
    // }
    // if (s == NATS_OK)
    // {
    //     // Now that we know the PING was sent properly, update
    //     // the number of PING sent.
    //     nc->pongs.outgoingPings++;

    //     if (pong != NULL)
    //     {
    //         pong->id = nc->pongs.outgoingPings;

    //         // Add this pong to the list.
    //         pong->next = NULL;
    //         pong->prev = nc->pongs.tail;

    //         if (nc->pongs.tail != NULL)
    //             nc->pongs.tail->next = pong;

    //         nc->pongs.tail = pong;

    //         if (nc->pongs.head == NULL)
    //             nc->pongs.head = pong;
    //     }
    // }
}

// static void
// _processPingTimer(natsTimer *timer, void *arg)
// {
//     natsConnection *nc = (natsConnection *)arg;

//     if (nc->status != NATS_CONN_STATUS_CONNECTED)
//     {
//         return;
//     }

//     // If we have more PINGs out than PONGs in, consider
//     // the connection stale.
//     if (++(nc->pout) > nc->opts->maxPingsOut)
//     {
//         _processOpError(nc, NATS_STALE_CONNECTION, false);
//         return;
//     }

//     _sendPing(nc, NULL);
// }

// static void
// _pingStopppedCb(natsTimer *timer, void *closure)
// {
//     natsConnection *nc = (natsConnection *)closure;

//     natsConn_release(nc);
// }

static natsStatus
_spinUpSocketWatchers(natsConnection *nc)
{
    natsStatus s = NATS_OK;

    // nc->pout        = 0;
    // nc->flusherStop = false;

    // if (nc->opts->evLoop == NULL)
    // {
    //     // Let's not rely on the created threads acquiring lock that would make it
    //     // safe to retain only on success.
    //     _retain(nc);

    //     s = natsThread_Create(&(nc->readLoopThread), _readLoop, (void*) nc);
    //     if (s != NATS_OK)
    //         _release(nc);
    // }

    // // Don't start flusher thread if connection was created with SendAsap option.
    // if ((s == NATS_OK) && !(nc->opts->sendAsap))
    // {
    //     _retain(nc);

    //     s = natsThread_Create(&(nc->flusherThread), _flusher, (void*) nc);
    //     if (s != NATS_OK)
    //         _release(nc);
    // }

    // if ((s == NATS_OK) && (nc->opts->pingInterval > 0))
    // {
    //     _retain(nc);

    //     if (nc->ptmr == NULL)
    //     {
    //         s = natsTimer_Create(&(nc->ptmr),
    //                              _processPingTimer,
    //                              _pingStopppedCb,
    //                              nc->opts->pingInterval,
    //                              (void*) nc);
    //         if (s != NATS_OK)
    //             _release(nc);
    //     }
    //     else
    //     {
    //         natsTimer_Reset(nc->ptmr, nc->opts->pingInterval);
    //     }
    // }

    return s;
}

natsStatus
natsConn_processMsg(natsConnection *nc, char *buf, int bufLen)
{
    return NATS_OK;
}

void natsConn_processOK(natsConnection *nc)
{
    // Do nothing for now.
}

void natsConn_processErr(natsConnection *nc, char *buf, int bufLen)
{
    // char error[256];
    // bool close       = false;
    // int  authErrCode = 0;

    // // Copy the error in this local buffer.
    // snprintf(error, sizeof(error), "%.*s", bufLen, buf);

    // // Trim spaces and remove quotes.
    // nats_NormalizeErr(error);

    // if (strcasecmp(error, STALE_CONNECTION) == 0)
    // {
    //     _processOpError(nc, NATS_STALE_CONNECTION, false);
    // }
    // else if (nats_strcasestr(error, PERMISSIONS_ERR) != NULL)
    // {
    //     _processPermissionViolation(nc, error);
    // }
    // else if ((authErrCode = _checkAuthError(error)) != 0)
    // {
    //     natsConn_Lock(nc);
    //     close = _processAuthError(nc, authErrCode, error);
    //     natsConn_Unlock(nc);
    // }
    // else
    // {
    //     close = true;
    //     natsConn_Lock(nc);
    //     nc->err = NATS_ERR;
    //     snprintf(nc->errStr, sizeof(nc->errStr), "%s", error);
    //     natsConn_Unlock(nc);
    // }
    // if (close)
    _close(nc, NATS_CONN_STATUS_CLOSED, false, true);
}

void natsConn_processPing(natsConnection *nc)
{
    // SET_WRITE_DEADLINE(nc);
    // if (natsConn_bufferWrite(nc, _PONG_PROTO_, _PONG_PROTO_LEN_) == NATS_OK)
    //     natsConn_flushOrKickFlusher(nc);
}

void natsConn_processPong(natsConnection *nc)
{
    // natsPong *pong = NULL;

    // nc->pongs.incoming++;

    // // Check if the first pong's id in the list matches the incoming Id.
    // if (((pong = nc->pongs.head) != NULL) && (pong->id == nc->pongs.incoming))
    // {
    //     // Remove the pong from the list
    //     _removePongFromList(nc, pong);

    //     // Release the Flush[Timeout] call
    //     pong->id = 0;
    // }

    // nc->pout = 0;
}

static natsStatus
_setupServerPool(natsConnection *nc)
{
    natsStatus s;

    s = natsSrvPool_Create(&(nc->srvPool), nc->opts);
    if (s == NATS_OK)
        nc->cur = natsSrvPool_GetSrv(nc->srvPool, 0);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsConn_create(natsConnection **newConn, natsOptions *options)
{
    natsStatus s = NATS_OK;
    natsConnection *nc = NULL;

    s = nats_Open();
    if (s == NATS_OK)
    {
        nc = NATS_CALLOC(1, sizeof(natsConnection));
        if (nc == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }
    if (s != NATS_OK)
    {
        // options have been cloned or created for the connection,
        // which was supposed to take ownership, so destroy it now.
        natsOptions_Destroy(options);
        return NATS_UPDATE_ERR_STACK(s);
    }

    natsLib_Retain();

    nc->refs = 1;
    nc->sockCtx.fd = NATS_SOCK_INVALID;
    nc->opts = options;

    IFOK(s, natsPool_Create(&(nc->pool), 0, false));

    nc->errStr[0] = '\0';

    IFOK(s, _setupServerPool(nc));
    // IFOK(s, natsHash_Create(&(nc->subs), 8));
    IFOK(s, natsSock_Init(&nc->sockCtx));
    IFOK(s, natsBuf_CreatePool(&(nc->scratch), nc->pool, DEFAULT_SCRATCH_SIZE));
    IFOK(s, natsBuf_Append(nc->scratch, (const uint8_t *)_HPUB_P_, _HPUB_P_LEN_));

    if (s == NATS_OK)
    {
        printf("<>/<> natsConn_create: created natsConnection: %p\n", nc);
        *newConn = nc;
    }
    else
        natsConn_release(nc);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
natsConnection_Connect(natsConnection **newConn, natsOptions *options)
{
    natsStatus s = NATS_OK;
    natsConnection *nc = NULL;
    natsOptions *opts = NULL;

    if (options == NULL)
    {
        s = natsConnection_ConnectTo(newConn, NATS_DEFAULT_URL);
        return NATS_UPDATE_ERR_STACK(s);
    }

    opts = natsOptions_clone(options);
    if (opts == NULL)
        s = NATS_NO_MEMORY;

    if (s == NATS_OK)
        s = natsConn_create(&nc, opts);
    if (s == NATS_OK)
        s = _connect(nc);

    if ((s == NATS_OK) || (s == NATS_NOT_YET_CONNECTED))
        *newConn = nc;
    else
        natsConn_release(nc);

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_processUrlString(natsOptions *opts, const char *urls)
{
    // int count = 0;
    natsStatus s = NATS_OK;
    // char **serverUrls = NULL;
    // char *urlsCopy = NULL;
    // char *commaPos = NULL;
    // char *ptr = NULL;

    // if (urls != NULL)
    // {
    //     ptr = (char *)urls;
    //     while ((ptr = strchr(ptr, ',')) != NULL)
    //     {
    //         ptr++;
    //         count++;
    //     }
    // }
    // if (count == 0)
    //     return natsOptions_SetURL(opts, urls);

    // serverUrls = (char **)NATS_CALLOC(count + 1, sizeof(char *));
    // if (serverUrls == NULL)
    //     s = NATS_NO_MEMORY;
    // if (s == NATS_OK)
    // {
    //     urlsCopy = NATS_STRDUP(urls);
    //     if (urlsCopy == NULL)
    //     {
    //         NATS_FREE(serverUrls);
    //         return NATS_NO_MEMORY;
    //     }
    // }

    // count = 0;
    // ptr = urlsCopy;

    // do
    // {
    //     serverUrls[count++] = ptr;

    //     commaPos = strchr(ptr, ',');
    //     if (commaPos != NULL)
    //     {
    //         ptr = (char *)(commaPos + 1);
    //         *(commaPos) = '\0';
    //     }

    // } while (commaPos != NULL);

    // if (s == NATS_OK)
    //     s = natsOptions_SetServers(opts, (const char **)serverUrls, count);

    // NATS_FREE(urlsCopy);
    // NATS_FREE(serverUrls);

    return s;
}

natsStatus
natsConnection_ConnectTo(natsConnection **newConn, const char *url)
{
    natsStatus s = NATS_OK;
    natsConnection *nc = NULL;
    natsOptions *opts = NULL;

    s = natsOptions_Create(&opts);
    if (s == NATS_OK)
    {
        s = _processUrlString(opts, url);
        // We still own the options at this point (until the call to natsConn_create())
        // so if there was an error, we need to destroy the options now.
        if (s != NATS_OK)
            natsOptions_Destroy(opts);
    }
    if (s == NATS_OK)
        s = natsConn_create(&nc, opts);
    if (s == NATS_OK)
        s = _connect(nc);

    if (s == NATS_OK)
        *newConn = nc;
    else
        natsConn_release(nc);

    return NATS_UPDATE_ERR_STACK(s);
}

static void
_destroyPong(natsConnection *nc, natsPong *pong)
{
    // // If this pong is the cached one, do not free
    // if (pong == &(nc->pongs.cached))
    //     memset(pong, 0, sizeof(natsPong));
    // else
    //     NATS_FREE(pong);
}

void natsConnection_Close(natsConnection *nc)
{
    if (nc == NULL)
        return;

    nats_doNotUpdateErrStack(true);

    _close(nc, NATS_CONN_STATUS_CLOSED, true, true);

    nats_doNotUpdateErrStack(false);
}

void natsConnection_Destroy(natsConnection *nc)
{
    if (nc == NULL)
        return;

    nats_doNotUpdateErrStack(true);

    _close(nc, NATS_CONN_STATUS_CLOSED, true, true);

    nats_doNotUpdateErrStack(false);

    natsConn_release(nc);
}

static void
_clearServerInfo(natsServerInfo *si)
{
    int i;

    NATS_FREE(si->id);
    NATS_FREE(si->host);
    NATS_FREE(si->version);

    for (i = 0; i < si->connectURLsCount; i++)
        NATS_FREE(si->connectURLs[i]);
    NATS_FREE(si->connectURLs);

    NATS_FREE(si->nonce);
    NATS_FREE(si->clientIP);

    memset(si, 0, sizeof(natsServerInfo));
}

static void
_freeConn(natsConnection *nc)
{
    if (nc == NULL)
        return;

    natsPool_Destroy(nc->pool);
    natsSrvPool_Destroy(nc->srvPool);
    _clearServerInfo(&(nc->info));
    natsParser_Destroy(nc->ps);
    natsOptions_Destroy(nc->opts);
    NATS_FREE(nc->el.buffer);

    NATS_FREE(nc);

    natsLib_Release();
}

void natsConn_retain(natsConnection *nc)
{
    if (nc == NULL)
        return;
    nc->refs++;
}

void natsConn_release(natsConnection *nc)
{
    if (nc == NULL)
        return;
    if (--(nc->refs) == 0)
        _freeConn(nc);
}
