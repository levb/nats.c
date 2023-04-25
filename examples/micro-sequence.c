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

// Sequence NATS microservice example.
//
// This example illustrated multiple NATS microservices communicating with each
// other.
//
// The main service (c-sequence) calculates the sum of 1/f(1) + 1/f(2)... up to
// N (included).  It exposes one (default) endpoint, "sequence". The inputs are
// f (the function name) and N. name, can be "factorial", "fibonacci", or
// "power2").
//
// c-sequence parses the request, then calculates the sequence by calling the
// c-functions microservice to calculate f(1), f(2), etc. The c-functions
// service in turn uses c-arithmetics microservice for all arithmetic
// operations.
//
// Requires NATS server and CLI, and the nats.c examples fully built. See
// https://github.com/nats-io/nats.c#building
//
// RUN:
//   ```sh
//   $NATS_SERVER & # NATS_SERVER points to the NATS server binary
//   nats_pid=$!
//   sleep 2 # wait for server to start
//   ./examples/nats-micro-sequence &
//   sequence_pid=$!
//   ./examples/nats-micro-func &
//   func_pid=$!
//   ./examples/nats-micro-arithmetics &
//   arithmetics_pid=$!
//   sleep 2 # wait for microservice to start
//   nats request -r 'sequence' '"factorial" 10'
//   nats request -r 'sequence' '"power2" 10'
//   nats request -r 'sequence' '"fibonacci" 10'
//   kill $sequence_pid $func_pid $arithmetics_pid $nats_pid
//   ```
//
// OUTPUT:
//   ```
//   2.718282
//   1.999023
//   3.341705
//   ```

static microError *
call_function(long double *result, natsConnection *nc, const char *subject, int n)
{
    microError *err = NULL;
    microClient *client = NULL;
    natsMsg *response = NULL;
    microArgs *args = NULL;
    char buf[256];
    char sbuf[256];
    int len;

    len = snprintf(buf, sizeof(buf), "%d", n);
    snprintf(sbuf, sizeof(sbuf), "f.%s", subject);
    MICRO_CALL(err, micro_NewClient(&client, nc, NULL));
    MICRO_CALL(err, microClient_DoRequest(&response, client, sbuf, buf, len));
    MICRO_CALL(err, micro_ParseArgs(&args, natsMsg_GetData(response), natsMsg_GetDataLength(response)));
    MICRO_CALL(err, microArgs_GetFloat(result, args, 0));

    microClient_Destroy(client);
    natsMsg_Destroy(response);
    return err;
}

// calculates the sum of X/f(1) + X/f(2)... up to N (included). The inputs are X
// (float), f name (string), and N (int). E.g.: '10.0 "power2" 10' will
// calculate 10/2 + 10/4 + 10/8 + 10/16 + 10/32 + 10/64 + 10/128 + 10/256 +
// 10/512 + 10/1024 = 20.998046875
static void handle_sequence(microRequest *req)
{
    microError *err = NULL;
    natsConnection *nc = microRequest_GetConnection(req);
    microArgs *args = NULL;
    int n = 0;
    int i;
    const char *function;
    long double initialValue = 1.0;
    long double value = 1.0;
    long double denominator = 0;
    char result[64];
    int result_len = 0;

    MICRO_CALL(err, micro_ParseArgs(&args, microRequest_GetData(req), microRequest_GetDataLength(req)));
    if ((err == NULL) && (microArgs_Count(args) != 2))
    {
        err = micro_Errorf(400, "Invalid number of arguments, expected 2 got %d", microArgs_Count(args));
    }

    MICRO_CALL(err, microArgs_GetString(&function, args, 0));
    if ((err == NULL) &&
        (strcmp(function, "factorial") != 0) &&
        (strcmp(function, "power2") != 0) &&
        (strcmp(function, "fibonacci") != 0))
    {
        err = micro_Errorf(400, "Invalid function name '%s', must be 'factorial', 'power2', or 'fibonacci'", function);
    }

    MICRO_CALL(err, microArgs_GetInt(&n, args, 1));
    if ((err == NULL) && (n < 1))
    {
        err = micro_Errorf(400, "Invalid number of iterations %d, must be at least 1", n);
    }

    for (i = 1; (err == NULL) && (i <= n); i++)
    {
        MICRO_CALL(err, call_function(&denominator, nc, function, i));
        if ((err == NULL) && (denominator == 0))
        {
            err = micro_Errorf(500, "division by zero at step %d", i);
        }
        MICRO_DO(err, value = value + initialValue / denominator);
    }

    MICRO_DO(err, result_len = snprintf(result, sizeof(result), "%Lf", value));

    microRequest_Respond(req, &err, result, result_len);
    microArgs_Destroy(args);
}

int main(int argc, char **argv)
{
    natsStatus s = NATS_OK;
    microError *err = NULL;
    natsConnection *conn = NULL;
    natsOptions *opts = NULL;
    microService *m = NULL;
    char errorbuf[1024];

    microEndpointConfig sequence_cfg = {
        .subject = "sequence",
        .name = "sequence-service",
        .handler = handle_sequence,
        .schema = NULL,
    };
    microServiceConfig cfg = {
        .description = "Sequence adder - NATS microservice example in C",
        .name = "c-sequence",
        .version = "1.0.0",
        .endpoint = &sequence_cfg,
    };

    opts = parseArgs(argc, argv, "");
    s = natsConnection_Connect(&conn, opts);
    if (s != NATS_OK)
    {
        printf("Error: %u - %s\n", s, natsStatus_GetText(s));
        nats_PrintLastErrorStack(stderr);
        return 1;
    }

    MICRO_CALL(err, micro_AddService(&m, conn, &cfg));
    MICRO_CALL(err, microService_Run(m));

    microService_Destroy(m);
    if (err != NULL)
    {
        printf("Error: %s\n", microError_String(err, errorbuf, sizeof(errorbuf)));
        microError_Destroy(err);
        return 1;
    }
    return 0;
}