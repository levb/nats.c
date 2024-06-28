// Copyright 2015-2022 The NATS Authors
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

#ifndef MSG_H_
#define MSG_H_

static const natsString nats_NATS10 = NATS_STR("NATS/1.0");

#define STATUS_HDR "Status"
#define DESCRIPTION_HDR "Description"
#define NO_RESP_STATUS "503"
#define NOT_FOUND_STATUS "404"
#define REQ_TIMEOUT "408"
#define CTRL_STATUS "100"
#define HDR_STATUS_LEN (3)

struct __natsMessage
{
    natsString subject;
    natsPool *pool;
    natsString reply;
    natsStrHash *headers;

    struct
    {
        unsigned needsLift : 1;
        unsigned acked : 1;
        unsigned timeout : 1;
        unsigned outgoing : 1;
    } flags;
    int64_t time;

    union
    {
        struct
        {
            // stores the entire message that has been read in, including the protocol line and the headers
            natsReadBuffer *buf;
            // points at the "NATS/1.0" part of the protocol line
            size_t headerStart;
            // points at the last byte of the header (after the last LF of CRLF CRLF)
            size_t headerEnd;

            uint64_t ssid;
        } in;
        struct
        {
            natsBytes buf;
            natsOnMessagePublishedF donef;
            void *doneClosure;
            void (*freef)(void *);
            void *freeClosure;
        } out;
    } x;
};

struct __natsHeaderValue;

typedef struct __natsHeaderValue
{
    natsString value;
    struct __natsHeaderValue *next;

} natsHeaderValue;

size_t natsMessageHeader_encodedLen(natsMessage *msg);
natsStatus natsMessageHeader_encode(natsBuf *buf, natsMessage *msg);

natsStatus nats_createMessage(natsMessage **newm, natsPool *pool, const char *subj);
natsStatus nats_createMessageParser(natsMessageParser **newParser, natsPool *pool, bool expectHeaders);
natsStatus nats_parseMessage(natsMessage **newMsg, natsMessageParser *parser, const uint8_t *data, const uint8_t *end, size_t *consumed);
natsStatus nats_setMessageHeader(natsMessage *m, natsString *key, natsString *value);

#endif /* MSG_H_ */
