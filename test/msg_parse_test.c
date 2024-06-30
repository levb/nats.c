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
#include "test.h"
#include "msg.h"

static inline natsStatus _createPoolWithReadChain(natsPool **pool, const char *buf)
{
    natsStatus s = NATS_OK;
    natsReadBuffer *rbuf = NULL;

    IFOK(s, nats_createPool(pool, &nats_defaultMemOptions, "conn-op"));
    IFOK(s, nats_getReadBuffer(&rbuf, *pool));
    if (STILL_OK(s))
    {
        memcpy(nats_readBufferData(rbuf), buf, strlen(buf));
        rbuf->readFrom = nats_readBufferData(rbuf);
        rbuf->buf.len = strlen(buf);
    }
    return s;
}

void Test_MessageParse(void)
{
    natsStatus s = NATS_OK;
    natsPool *gpool = NULL;
    natsPool *oppool = NULL;
    natsMessageParser parser = {0};

    test("initialize message parser");
    IFOK(s, nats_createPool(&gpool, &nats_defaultMemOptions, "global"));
    IFOK(s, nats_initMessageParser(&parser, gpool));
    testCond(STILL_OK(s));

    // test("parse hello MSG");
    // const char *data = "MSG foo 1234 5\r\nhello\r\n";
    // natsMessage *msg1 = NULL;
    // size_t consumed1 = 0;
    // IFOK(s, _createPoolWithReadChain(&oppool, data));
    // IFOK(s, nats_prepareToParseMessage(&parser, oppool, false, 3));
    // IFOK(s, nats_parseMessage(&msg1, &parser,
    //                           oppool->readChain->tail->readFrom + 3,
    //                           oppool->readChain->tail->readFrom + unsafe_strlen(data),
    //                           &consumed1));
    // testCond(STILL_OK(s) &&
    //          (consumed1 == unsafe_strlen(data) - 3) &&
    //          (msg1 != NULL) &&
    //          (nats_equalsCString(&msg1->subject, "foo")) &&
    //          (msg1->x.in.ssid == 1234) &&
    //          (nats_IsStringEmpty(&msg1->reply)) &&
    //          (msg1->x.in.totalLen == 5) &&
    //          (msg1->x.in.headerLen == 0) &&
    //          (msg1->x.in.payloadStartPos == 16) &&
    //          (msg1->x.in.startPos == 16));

    test("parse status-only");
    const char *data = "HMSG XXX 0 16\t16\r\nNATS/1.0 503\r\n\r\n\r\n";
    natsMessage *msg2 = NULL;
    size_t consumed2 = 0;
    IFOK(s, _createPoolWithReadChain(&oppool, data));
    IFOK(s, nats_prepareToParseMessage(&parser, oppool, true, 4));
    IFOK(s, nats_parseMessage(&msg2, &parser,
                              oppool->readChain->tail->readFrom + 4,
                              oppool->readChain->tail->readFrom + unsafe_strlen(data),
                              &consumed2));
    testCond(STILL_OK(s) &&
                (consumed2 == unsafe_strlen(data) - 4) &&
                (msg2 != NULL) &&
                (nats_equalsCString(&msg2->subject, "XXX")) &&
                (msg2->x.in.ssid == 0) &&
                (nats_IsStringEmpty(&msg2->reply) &&
                (msg2->x.in.totalLen == 16) &&
                (msg2->x.in.headerLen == 16) &&
                (msg2->x.in.payloadStartPos == 34) &&
                (msg2->x.in.startPos == 18)));
}
