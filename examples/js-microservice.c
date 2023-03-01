// Copyright 2021 The NATS Authors
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

#include "examples.h"

static void
onMsg(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
    if (print)
        printf("Received msg: %s - %.*s\n",
               natsMsg_GetSubject(msg),
               natsMsg_GetDataLength(msg),
               natsMsg_GetData(msg));

    if (start == 0)
        start = nats_Now();

    // We should be using a mutex to protect those variables since
    // they are used from the subscription's delivery and the main
    // threads. For demo purposes, this is fine.
    if (++count == total)
        elapsed = nats_Now() - start;

    // Since this is auto-ack callback, we don't need to ack here.
    natsMsg_Destroy(msg);
}

static void
asyncCb(natsConnection *nc, natsSubscription *sub, natsStatus err, void *closure)
{
    printf("Async error: %u - %s\n", err, natsStatus_GetText(err));

    natsSubscription_GetDropped(sub, (int64_t *)&dropped);
}

int main(int argc, char **argv)
{
    natsConnection *conn = NULL;
    natsOptions *opts = NULL;
    jsMicroservice *m = NULL;
    jsCtx *js = NULL;
    jsErrCode jerr = 0;
    jsOptions jsOpts;
    jsSubOptions so;
    natsStatus s;
    bool delStream = false;
    jsMicroserviceConfig cfg = {
        .description = "Example JetStream microservice",
        .name = "example",
        .version = "1.0.0",
    };

    opts = parseArgs(argc, argv, "");

    s = natsOptions_SetErrorHandler(opts, asyncCb, NULL);
    if (s == NATS_OK)
    {
        s = natsConnection_Connect(&conn, opts);
    }
    if (s == NATS_OK)
    {
        s = jsOptions_Init(&jsOpts);
    }
    if (s == NATS_OK)
    {
        s = jsSubOptions_Init(&so);
    }
    if (s == NATS_OK)
    {
        s = natsConnection_JetStream(&js, conn, &jsOpts);
    }
    if (s == NATS_OK)
    {
        s = js_AddMicroservice(&m, js, &cfg, &jerr);
    }
    if (s == NATS_OK)
    {
        s = js_RunMicroservice(m, &jerr);
    }
    if (s == NATS_OK)
    {
        // Destroy all our objects to avoid report of memory leak
        jsMicroservice_Destroy(m);
        jsCtx_Destroy(js);
        natsConnection_Destroy(conn);
        natsOptions_Destroy(opts);

        // To silence reports of memory still in used with valgrind
        nats_Close();

        return 0;
    }

    printf("Error: %u - %s - jerr=%u\n", s, natsStatus_GetText(s), jerr);
    nats_PrintLastErrorStack(stderr);
    return 1;
}
