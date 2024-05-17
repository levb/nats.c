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

#include "natsp.h"

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
