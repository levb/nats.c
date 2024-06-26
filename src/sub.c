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

#include "natsp.h"

#include <string.h>

#include "conn.h"
#include "msg.h"

natsStatus nats_Subscribe(natsConnection *nc, uint64_t *sid, const char *subject, const char *queueGroup)
{
    natsStatus s = NATS_OK;

    s = nats_sendSubscribe(nc, sid, subject, queueGroup);

    return NATS_UPDATE_ERR_STACK(s);
}
