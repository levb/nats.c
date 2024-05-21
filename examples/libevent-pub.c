// Copyright 2016-2018 The NATS Authors
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

#include "adapters/libevent.h"
#include "examples.h"

static const char *usage = ""
                           "-txt           text to send (default is 'hello')\n"
                           "-count         number of messages to send\n";

void onPublishOK(natsConnection *nc, void *closure)
{
}

void onConnectOK(natsConnection *nc, void *closure)
{
    printf("Published test message!!!!\n");
    // natsConnection_Close(nc);
}

void onConnectError(natsConnection *nc, natsStatus err, void *closure)
{
    printf("Error: %d - %s\n", err, natsStatus_GetText(err));
}

void onPublishError(natsConnection *nc, natsStatus err, void *closure)
{
    printf("Error: %d - %s\n", err, natsStatus_GetText(err));
}

int main(int argc, char **argv)
{
    natsConnection *conn = NULL;
    natsOptions *opts = NULL;
    natsStatus s = NATS_OK;
    struct event_base *evLoop = NULL;

    nats_Open();

    opts = parseArgs(argc, argv, usage);

    printf("Sending %" PRId64 " messages to subject '%s'\n", total, subj);

    // One time initialization of things that we need.
    natsLibevent_Init();

    // Create a loop.
    evLoop = event_base_new();
    if (evLoop == NULL)
        s = NATS_ERR;

    // Indicate which loop and callbacks to use once connected.
    if (s == NATS_OK)
        s = natsOptions_SetEventLoop(opts, (void *)evLoop,
                                     natsLibevent_Attach,
                                     natsLibevent_Read,
                                     natsLibevent_Write,
                                     natsLibevent_Detach);

    if (s == NATS_OK)
        s = natsConnection_Connect(&conn, opts);

    if (s != NATS_OK)
    {
        printf("Error: %d - %s\n", s, natsStatus_GetText(s));
        nats_PrintLastErrorStack(stderr);
        return 1;
    }

    event_base_dispatch(evLoop);

    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);

    if (evLoop != NULL)
        event_base_free(evLoop);

    // To silence reports of memory still in used with valgrind
    nats_Close();
    libevent_global_shutdown();

    return 0;
}
