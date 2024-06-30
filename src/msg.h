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

#define NATS_MAX_MESSAGE_HEADER_LINE (32 * 1024)
#define NATS_MAX_MESSAGE_HEADER_ARGS 3

#define NO_RESP_STATUS "503"
#define NOT_FOUND_STATUS "404"
#define REQ_TIMEOUT "408"
#define CTRL_STATUS "100"

struct __natsMessage
{
    natsString subject;
    natsPool *pool;
    natsString reply;
    natsStrHash *headers; // <>/<> FIXME defer the parsing of headers until the first access

    int64_t time;

    union
    {
        struct
        {
            // The read chain of an incoming message is in the pool already, no
            // need to store another pointer here.

            // points at the "NATS/1.0" part of the protocol line
            size_t startPos;
            size_t headerLen;
            // points at the first byte of the message payload
            size_t payloadStartPos;
            size_t totalLen;

            uint64_t ssid;

            int status;
            natsString statusDescription;
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

    struct
    {
        unsigned needsLift : 1;
        unsigned acked : 1;
        unsigned timeout : 1;
        unsigned outgoing : 1;
    } flags;
};

struct __natsHeaderValue;

typedef struct __natsHeaderValue
{
    natsString value;
    struct __natsHeaderValue *next;

} natsHeaderValue;

struct __natsMessageParser_s
{
    natsMessage *msg;

    struct
    {
        int current : 8;
        int next : 8;

        // Are we processing an HMSG or a MSG?
        unsigned isHMSG : 1;

        // Re-process the current character again.
        unsigned reprocessChar : 1;

        // Toggles whitespace skipping.
        unsigned skipWhitespace : 1;

        // set when encountered \r or \n, reset on any other character.
        unsigned EOL : 1;

        unsigned breakOnColon : 1;
        unsigned breakOnWhitespace : 1;
        unsigned validateChars : 1;
    } state;

    size_t lineLen;
    size_t pos;   // current position in the read chain
    natsBuf *acc; // Accumulator
    natsValidBitmask accValidChars;

    natsBytes varargs[NATS_MAX_MESSAGE_HEADER_ARGS];
    int numVarargs;

    natsString headerName; // (a copy of) the last accumulated header name
};

natsStatus nats_createMessage(natsMessage **newm, natsPool *pool, const char *subj);
natsStatus nats_encodeMessageHeader(natsBuf *buf, natsMessage *msg);
natsStatus nats_initMessageParser(natsMessageParser *parser, natsPool *pool);
natsStatus nats_parseMessage(natsMessage **newMsg, natsMessageParser *parser, const uint8_t *data, const uint8_t *end, size_t *consumed);
natsStatus nats_prepareToParseMessage(natsMessageParser *parser, natsPool *pool, bool isHMSG, size_t startPosInReadChain);
natsStatus nats_setMessageHeader(natsMessage *m, natsString *key, natsString *value);
size_t nats_encodedMessageHeaderLen(natsMessage *msg);

#endif /* MSG_H_ */
