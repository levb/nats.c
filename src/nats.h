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

#ifndef NATS_H_
#define NATS_H_

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>

#include "status.h"
#include "version.h"

/** \def NATS_EXTERN
 *  \brief Needed for shared library.
 *
 *  Based on the platform this is compiled on, it will resolve to
 *  the appropriate instruction so that objects are properly exported
 *  when building the shared library.
 */
#if defined(_WIN32)
#include <winsock2.h>
#if defined(nats_EXPORTS)
#define NATS_EXTERN __declspec(dllexport)
#elif defined(nats_IMPORTS)
#define NATS_EXTERN __declspec(dllimport)
#else
#define NATS_EXTERN
#endif

typedef SOCKET natsSock;
#else
#define NATS_EXTERN
typedef int natsSock;
#endif

#define NATS_DEFAULT_URL "nats://localhost:4222"

/** \brief A type to represent user-provided metadata, a list of k=v pairs.
 *
 * Used in JetStream, microservice configuration.
 */

typedef struct natsMetadata
{
    // User-provided metadata for the stream, encoded as an array of {"key",
    // "value",...} C strings
    const char **List;

    // Number of key/value pairs in Metadata, 1/2 of the length of the array.
    int Count;
} natsMetadata;

typedef struct __natsConnection natsConnection;
typedef struct __natsOptions natsOptions;

/** \brief Attach this connection to the external event loop.
 *
 * After a connection has (re)connected, this callback is invoked. It should
 * perform the necessary work to start polling the given socket for READ events.
 *
 * @param userData location where the adapter implementation will store the
 * object it created and that will later be passed to all other callbacks. If
 * `*userData` is not `NULL`, this means that this is a reconnect event.
 * @param loop the event loop (as a generic void*) this connection is being
 * attached to.
 * @param nc the connection being attached to the event loop.
 * @param socket the socket to poll for read/write events.
 */
typedef natsStatus (*natsEvLoop_Attach)(
    void **userData,
    void *loop,
    natsConnection *nc,
    natsSock socket);

/** \brief Read event needs to be added or removed.
 *
 * The `NATS` library will invoke this callback to indicate if the event
 * loop should start (`add is `true`) or stop (`add` is `false`) polling
 * for read events on the socket.
 *
 * @param userData the pointer to an user object created in #natsEvLoop_Attach.
 * @param add `true` if the event library should start polling, `false` otherwise.
 */
typedef natsStatus (*natsEvLoop_ReadAddRemove)(
    void *userData,
    bool add);

/** \brief Write event needs to be added or removed.
 *
 * The `NATS` library will invoke this callback to indicate if the event
 * loop should start (`add is `true`) or stop (`add` is `false`) polling
 * for write events on the socket.
 *
 * @param userData the pointer to an user object created in #natsEvLoop_Attach.
 * @param add `true` if the event library should start polling, `false` otherwise.
 */
typedef natsStatus (*natsEvLoop_WriteAddRemove)(
    void *userData,
    bool add);

/** \brief Detach from the event loop.
 *
 * The `NATS` library will invoke this callback to indicate that the connection
 * no longer needs to be attached to the event loop. User can cleanup some state.
 *
 * @param userData the pointer to an user object created in #natsEvLoop_Attach.
 */
typedef natsStatus (*natsEvLoop_Detach)(
    void *userData);

NATS_EXTERN natsStatus nats_Open(void);
NATS_EXTERN const char *nats_GetVersion(void);
NATS_EXTERN uint32_t nats_GetVersionNumber(void);

NATS_EXTERN int64_t nats_Now(void);
NATS_EXTERN int64_t nats_NowInNanoSeconds(void);
NATS_EXTERN void nats_Sleep(int64_t sleepTime);

NATS_EXTERN void nats_PrintLastErrorStack(FILE *file);
NATS_EXTERN const char *natsStatus_GetText(natsStatus s);

NATS_EXTERN natsStatus natsOptions_Create(natsOptions **newOpts);
NATS_EXTERN natsStatus natsOptions_IPResolutionOrder(natsOptions *opts, int order);
NATS_EXTERN natsStatus natsOptions_SetEventLoop(natsOptions *opts, void *loop, natsEvLoop_Attach attachCb, natsEvLoop_ReadAddRemove readCb, natsEvLoop_WriteAddRemove writeCb, natsEvLoop_Detach detachCb);
NATS_EXTERN natsStatus natsOptions_SetName(natsOptions *opts, const char *name);
NATS_EXTERN natsStatus natsOptions_SetNoRandomize(natsOptions *opts, bool noRandomize);
NATS_EXTERN natsStatus natsOptions_SetServers(natsOptions *opts, const char **servers, int serversCount);
NATS_EXTERN natsStatus natsOptions_SetURL(natsOptions *opts, const char *url);
NATS_EXTERN natsStatus natsOptions_SetVerbose(natsOptions *opts, bool verbose);
NATS_EXTERN void natsOptions_Destroy(natsOptions *opts);

NATS_EXTERN natsStatus natsConnection_Connect(natsConnection **nc, natsOptions *options);
NATS_EXTERN natsStatus natsConnection_ConnectTo(natsConnection **nc, const char *urls);
NATS_EXTERN natsStatus natsConnection_Publish(natsConnection *nc, const char *subj, const void *data, int dataLen);
NATS_EXTERN void natsConnection_Close(natsConnection *nc);
NATS_EXTERN void natsConnection_Destroy(natsConnection *nc);
NATS_EXTERN void natsConnection_ProcessReadEvent(natsConnection *nc);
NATS_EXTERN void natsConnection_ProcessWriteEvent(natsConnection *nc);

NATS_EXTERN const char *nats_GetLastError(natsStatus *status);
NATS_EXTERN natsStatus nats_GetLastErrorStack(char *buffer, size_t bufLen);
NATS_EXTERN natsStatus natsOptions_SetSecure(natsOptions *opts, bool secure);

#endif /* NATS_H_ */
