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

#include "hash.h"
#include "conn.h"

typedef enum
{
    stateStart = 0,
    stateAccumulateArg,
    stateSubject,
    stateSSID,
    stateVarargs,
    stateNATSheader,
    stateNATSheader_N,
    stateNATSheader_NA,
    stateNATSheader_NAT,
    stateNATSheader_NATS,
    stateNATSheader_NATS_SLASH,
    stateNATSheader_NATS_SLASH_1,
    stateNATSheader_NATS_SLASH_1_DOT,
    stateNATSheader_NATS_SLASH_1_DOT_0,
    stateNATSheader_NATS_SLASH_1_DOT_0_CR,
    stateHeaderName,
    stateHeaderValue,
    stateEndHeaderLine,
    stateEndHeader,
    stateEnd
} state;

struct __natsMessageParser_s
{
    state state;
    size_t headerBytes;
    size_t totalBytes;

    natsMessage *msg;

    struct
    {
        // Are we processing an HMSG or a MSG?
        unsigned expectHeaders : 1;

        // Toggles whitespace skipping.
        unsigned skipWhitespace : 1;

        // Set if we are accumulating a header name, must be terminated with a ':'
        unsigned parsingHeaderName : 1;
        // Set if we are accumulating a header value, must be terminated with a CR,
        // we include trailing whitespace in the value.
        unsigned parsingHeaderValue : 1;

        // set if terminated by a CR, indicating end of the line.
        unsigned EOL : 1;
    } flags;

    // Besides the subject and sid which we can immediately store in the
    // message, we need to buffer up to 3 args and parse them depending on the
    // total number of args.
    natsBytes varargs[3];
    int numVarargs;

    natsString headerName; // (a copy of) the last accumulated header name

    // Accumulator state
    natsBuf *argBuf;
    state nextState; // next state to transition to after finishing the arg
};

static natsStatus _startAccumulateArg(natsMessageParser *parser, state nextState, uint8_t ch)
{
    natsStatus s = nats_resetBuf(parser->argBuf);
    IFOK(s, nats_appendB(parser->argBuf, ch));
    if (STILL_OK(s))
    {
        parser->state = stateAccumulateArg;
        parser->nextState = nextState;
        parser->flags.EOL = false;
        parser->flags.skipWhitespace = false;
    }
    return s;
}

static void _finishAccumulateArg(natsMessageParser *parser, bool EOL)
{
    parser->state = parser->nextState;
    parser->flags.skipWhitespace = true;
    parser->flags.parsingHeaderName = false;
    parser->flags.parsingHeaderValue = false;
    parser->flags.EOL = EOL;
}

#define _msgError(_p, _f, ...) \
    nats_setErrorf(NATS_ERR, "message parsing error: %s:" _f, nats_printableString(&(_p)->msg->subject), __VA_ARGS__)

static natsStatus _processVarargs(natsMessageParser *parser)
{
    natsStatus s = NATS_OK;

    if (parser->flags.expectHeaders)
    {
        switch (parser->numVarargs)
        {
        case 2: // totalBytes headerBytes
            IFOK(s, nats_strToSizet(&parser->totalBytes, parser->varargs[0].bytes, parser->varargs[0].len));
            IFOK(s, nats_strToSizet(&parser->headerBytes, parser->varargs[1].bytes, parser->varargs[1].len));
            break;
        case 3: // reply-to totalBytes headerBytes
            IFOK(s, ALWAYS_OK(parser->msg->reply = *nats_bytesAsString(&parser->varargs[0])));
            IFOK(s, nats_strToSizet(&parser->totalBytes, parser->varargs[1].bytes, parser->varargs[1].len));
            IFOK(s, nats_strToSizet(&parser->headerBytes, parser->varargs[2].bytes, parser->varargs[2].len));
            break;
        default:
            s = _msgError(parser, "HMSG: expected 4 or 5 arguments, got %d", parser->numVarargs + 2);
            break;
        }
    }
    else
    {
        switch (parser->numVarargs)
        {
        case 1: // totalBytes
            s = nats_strToSizet(&parser->totalBytes, parser->varargs[0].bytes, parser->varargs[0].len);
            break;
        case 2: // reply-to totalBytes
            IFOK(s, ALWAYS_OK(parser->msg->reply = *nats_bytesAsString(&parser->varargs[0])));
            IFOK(s, nats_strToSizet(&parser->totalBytes, parser->varargs[1].bytes, parser->varargs[1].len));
            break;
        default:
            s = _msgError(parser, "HMSG: expected 3 or 4 arguments, got %d", parser->numVarargs + 2);
            break;
        }
    }

    return s;
}

natsStatus
nats_parseMessage(natsMessage **newMsg, natsMessageParser *parser, const uint8_t *data, const uint8_t *end, size_t *consumed)
{
    natsStatus s = NATS_OK;
    size_t c = 0;
    const uint8_t *remaining = data;

    CONNDEBUGf("Parsing message: '%.*s'", (int)(end - remaining), remaining);

    while ((STILL_OK(s)) && (parser->state != stateEnd))
    {
        // Get the next character to process. If we have reached the end of the buffer we are done.
        uint8_t ch;
        if (end - remaining == 0)
        {
            if (consumed != NULL)
                *consumed = c;
            return NATS_OK;
        }
        ch = *remaining;
        remaining++;
        c++;

        if (parser->flags.skipWhitespace && ((ch == ' ') || (ch == '\t')))
            continue;

        switch (parser->state)
        {
        case stateStart:
            CONNDEBUGf("stateStart: '%c'", ch);
            // Create the message
            s = _startAccumulateArg(parser, stateSubject, ch);
            continue; // stateStart

        case stateAccumulateArg:
            CONNDEBUGf("stateAccumulateArg: '%c'", ch);
            switch (ch)
            {
            case ' ':
            case '\t':
            case '\r':
                if (parser->flags.parsingHeaderName)
                {
                    parser->flags.parsingHeaderName = false;
                    // We are accumulating a header name, it must be terminated by a ':'
                    s = _msgError(parser, "expected header name or ':', got %x", ch);
                    continue;
                }
                if (parser->flags.parsingHeaderValue)
                {
                    if (ch == ' ' || ch == '\t')
                    {
                        s = nats_appendB(parser->argBuf, ch);
                        continue;
                    }
                    parser->flags.parsingHeaderValue = false;
                }
                // end of string
                _finishAccumulateArg(parser, ch == '\r');
                continue;

            case ':':
                if (parser->flags.parsingHeaderName)
                    _finishAccumulateArg(parser, false);
                else
                    s = nats_appendB(parser->argBuf, ch);
                continue;

            default:
                s = nats_appendB(parser->argBuf, ch);
                continue;
            }
            continue; // stateAccumulateArg

        case stateSubject:
            CONNDEBUGf("stateSubject: %x", ch);
            // Save the subject into the message right away.
            s = nats_pdupString(&parser->msg->subject, parser->msg->pool, nats_bufAsString(parser->argBuf));
            // Go on to collect SSID
            IFOK(s, _startAccumulateArg(parser, stateSSID, ch));
            continue; // stateSubject

        case stateSSID:
            CONNDEBUGf("stateSSID: %x", ch);
            // Our SSIDs are always numeric, parse the string here and store the
            // result in the message.
            s = nats_strToUint64(&parser->msg->x.in.ssid, nats_bufData(parser->argBuf), nats_bufLen(parser->argBuf));
            // Go on to collect the rest of the arguments
            IFOK(s, _startAccumulateArg(parser, stateVarargs, ch));
            continue; // stateSSID

        case stateVarargs:
            CONNDEBUGf("stateVarargs: '%x'", ch);
            if (parser->numVarargs >= 3)
            {
                s = _msgError(parser, "%s", "too many arguments in MSG line");
                continue;
            }
            if (parser->flags.EOL && (ch != '\n'))
            {
                s = _msgError(parser, "expected an LF following a CR, got %x", ch);
                continue;
            }

            if (parser->numVarargs > 0)
            {
                // If we were already gathering a vararg, we need to save it.
                int i = parser->numVarargs - 1;
                s = nats_pdupBytes(&parser->varargs[i], parser->msg->pool, nats_bufAsBytes(parser->argBuf));
                if (s != NATS_OK)
                    continue;
                parser->numVarargs++;
            }

            if (parser->flags.EOL)
            {
                // We are done with the varargs, we need to process them, and
                // proceed to the rest of the header.
                s = _processVarargs(parser);
                if (STILL_OK(s))
                    parser->state = stateNATSheader;
            }
            else
            {
                // Start accumulating the next vararg.
                s = _startAccumulateArg(parser, stateVarargs, ch);
            }
            continue; // stateVarargs

        case stateNATSheader:
            CONNDEBUGf("stateNATSheader: '%x'", ch);
            if (ch == 'N')
                parser->state = stateNATSheader_N;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader

        case stateNATSheader_N:
            CONNDEBUGf("stateNATSheader_N: '%x'", ch);
            if (ch == 'A')
                parser->state = stateNATSheader_NA;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_N

        case stateNATSheader_NA:
            CONNDEBUGf("stateNATSheader_NA: '%x'", ch);
            if (ch == 'T')
                parser->state = stateNATSheader_NAT;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NA

        case stateNATSheader_NAT:
            CONNDEBUGf("stateNATSheader_NAT: '%x'", ch);
            if (ch == 'S')
                parser->state = stateNATSheader_NATS;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NAT

        case stateNATSheader_NATS:
            CONNDEBUGf("stateNATSheader_NATS: '%x'", ch);
            if (ch == '/')
                parser->state = stateNATSheader_NATS_SLASH;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS

        case stateNATSheader_NATS_SLASH:
            CONNDEBUGf("stateNATSheader_NATS_SLASH: '%x'", ch);
            if (ch == '1')
                parser->state = stateNATSheader_NATS_SLASH_1;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS_SLASH

        case stateNATSheader_NATS_SLASH_1:
            CONNDEBUGf("stateNATSheader_NATS_SLASH_1: '%x'", ch);
            if (ch == '.')
                parser->state = stateNATSheader_NATS_SLASH_1_DOT;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS_SLASH_1

        case stateNATSheader_NATS_SLASH_1_DOT:
            CONNDEBUGf("stateNATSheader_NATS_SLASH_1_DOT: '%x'", ch);
            if (ch == '0')
                parser->state = stateNATSheader_NATS_SLASH_1_DOT_0;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS_SLASH_1_DOT

        case stateNATSheader_NATS_SLASH_1_DOT_0:
            CONNDEBUGf("stateNATSheader_NATS_SLASH_1_DOT_0: '%x'", ch);
            if (ch == '\r')
                parser->state = stateNATSheader_NATS_SLASH_1_DOT_0_CR;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS_SLASH_1_DOT_0

        case stateNATSheader_NATS_SLASH_1_DOT_0_CR:
            CONNDEBUGf("stateNATSheader_NATS_SLASH_1_DOT_0_CR: '%x'", ch);
            if (ch == '\n')
                parser->state = stateHeaderName;
            else
                s = _msgError(parser, "expected 'NATS/1.0', got %x", ch);
            continue; // stateNATSheader_NATS_SLASH_1_DOT_0_CR

        case stateHeaderName:
            if (ch == '\r')
            {
                // We are done with the headers, we will be back here after the LF.
                parser->state = stateEndHeader;
                continue;
            }
            CONNDEBUGf("stateHeaderName: '%x'", ch);
            parser->flags.parsingHeaderName = true;
            s = _startAccumulateArg(parser, stateHeaderValue, ch);
            continue; // stateHeaderName

        case stateHeaderValue:
            CONNDEBUGf("stateHeaderValue: '%x'", ch);
            // gather the header name from the accumulator
            s = nats_pdupString(&parser->headerName, parser->msg->pool, nats_bufAsString(parser->argBuf));
            parser->flags.parsingHeaderValue = true;
            IFOK(s, _startAccumulateArg(parser, stateEndHeaderLine, ch));
            continue; // stateHeaderValue

        case stateEndHeaderLine:
            CONNDEBUGf("stateEndHeaderLine: '%x'", ch);
            {
                if (ch != '\n')
                {
                    s = _msgError(parser, "expected LF, got %x", ch);
                    continue;
                }

                // Add the header to the message
                s = nats_setMessageHeader(parser->msg, &parser->headerName, nats_bufAsString(parser->argBuf));
                if (!STILL_OK(s))
                    continue;
                // Go back to collecting headers, we will be at the start of the line.
                parser->state = stateHeaderName;
            }
            continue; // stateEndHeaderLine

        case stateEndHeader:
            CONNDEBUGf("stateEndHeader: '%x'", ch);
            if (ch != '\n')
            {
                s = _msgError(parser, "expected LF, got %x", ch);
                continue;
            }
            // We are done with the headers, we will be back here after the LF.
            parser->state = stateEnd; // <>/<> PAYLOAD!
            continue; // stateEndHeader

        case stateEnd: // We are done, we should not be here.
            continue;

        default:
            s = _msgError(parser, "invalid state %d", parser->state);
            break;
        }
    }

    if ((STILL_OK(s)) && (parser->state == stateEnd))
    {
        if (consumed != NULL)
            *consumed = c;
        *newMsg = parser->msg;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_createMessageParser(natsMessageParser **newParser, natsPool *pool, bool expectHeaders)
{
    natsStatus s = NATS_OK;
    natsMessageParser *parser = NULL;

    if (newParser == NULL)
        s = nats_setDefaultError(NATS_INVALID_ARG);

    IFOK(s, CHECK_NO_MEMORY(parser = nats_palloc(pool, sizeof(natsMessageParser))));
    IFOK(s, nats_createMessage(&parser->msg, pool, NULL));
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);
    parser->msg->flags.outgoing = false;
    parser->flags.expectHeaders = expectHeaders;
    parser->state = stateStart;
    parser->flags.skipWhitespace = true;
    IFOK(s, nats_getGrowableBuf(&parser->argBuf, pool, 0));
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    *newParser = parser;
    return NATS_UPDATE_ERR_STACK(s);
}
