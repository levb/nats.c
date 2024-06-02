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

#ifndef OPTS_H_
#define OPTS_H_

#define CHECK_OPTIONS(o, c)                   \
    if (((o) == NULL) || ((c)))                        \
        return nats_setDefaultError(NATS_INVALID_ARG);

struct __natsOptions
{
    // All options values are allocated in this pool.
    natsPool *pool;

    // All fields that are not "simple" types must be declared before
    // __memcpy_from_here, and should be manually added to natsOptions_Clone()
    // and duplicated into the target opts' pool.
    char *url;
    char **servers;
    int serversCount;
    char *name;
    char *user;
    char *password;
    char *token;

    uint8_t __memcpy_from_here;
    bool noRandomize;
    int64_t timeout;
    bool verbose;
    bool pedantic;
    bool allowReconnect;
    bool secure;
    int ioBufSize;
    int maxReconnect;
    int64_t reconnectWait;
    int reconnectBufSize;
    int64_t writeDeadline;

    bool ignoreDiscoveredServers;

    int64_t pingInterval;
    int maxPingsOut;
    int maxPendingMsgs;
    int64_t maxPendingBytes;

    void *evLoop;
    natsEvLoopCallbacks evCbs;

    int orderIP; // possible values: 0,4,6,46,64

    // forces the old method of Requests that utilize
    // a new Inbox and a new Subscription for each request
    bool useOldRequestStyle;

    // If set to true, the Publish call will flush in place and
    // not rely on the flusher.
    bool sendAsap;

    // If set to true, pending requests will fail with NATS_CONNECTION_DISCONNECTED
    // when the library detects a disconnection.
    bool failRequestsOnDisconnect;

    // NoEcho configures whether the server will echo back messages
    // that are sent on this connection if we also have matching subscriptions.
    // Note this is supported on servers >= version 1.2. Proto 1 or greater.
    bool noEcho;

    // If set to true, in case of failed connect, tries again using
    // reconnect options values.
    bool retryOnFailedConnect;

    // Reconnect jitter added to reconnect wait
    int64_t reconnectJitter;
    int64_t reconnectJitterTLS;

    // Disable the "no responders" feature.
    bool disableNoResponders;

    // Custom message payload padding size
    int payloadPaddingSize;
};

#define NATS_OPTS_DEFAULT_MAX_RECONNECT         (60)
#define NATS_OPTS_DEFAULT_TIMEOUT               (2 * 1000)          // 2 seconds
#define NATS_OPTS_DEFAULT_RECONNECT_WAIT        (2 * 1000)          // 2 seconds
#define NATS_OPTS_DEFAULT_PING_INTERVAL         (2 * 60 * 1000)     // 2 minutes
#define NATS_OPTS_DEFAULT_MAX_PING_OUT          (2)
#define NATS_OPTS_DEFAULT_IO_BUF_SIZE           (32 * 1024)         // 32 KB
#define NATS_OPTS_DEFAULT_MAX_PENDING_MSGS      (65536)             // 65536 messages
#define NATS_OPTS_DEFAULT_MAX_PENDING_BYTES     (64 * 1024 * 1024)  // 64 MB
#define NATS_OPTS_DEFAULT_RECONNECT_BUF_SIZE    (8 * 1024 * 1024)   // 8 MB
#define NATS_OPTS_DEFAULT_RECONNECT_JITTER      (100)               // 100 ms
#define NATS_OPTS_DEFAULT_RECONNECT_JITTER_TLS  (1000)              // 1 second

natsOptions*
natsOptions_clone(natsOptions *opts);

#endif /* OPTS_H_ */
