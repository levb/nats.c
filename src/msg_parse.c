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
    stateSubject,
    stateSSID,
    stateMessageVarargs,
    stateHeaderN,
    stateHeaderNA,
    stateHeaderNAT,
    stateHeaderNATS,
    stateHeaderNATS_,
    stateHeaderNATS_1,
    stateHeaderNATS_1_,
    stateHeaderNATS_1_0,
    stateHeaderStatus1,
    stateHeaderStatus2,
    stateHeaderStatus3,
    stateHeaderStatusDescription,
    stateHeaderName,
    stateHeaderValue,
    stateEndHeaderNameValue,
    stateEndHeader,
    statePayload,
    stateAccumulate,
    stateRequireCRLF,
    stateRequireLF,
    stateEnd
} state;

const state stateInvalid = -1;

static inline void _setBreakOnColon(natsMessageParser *parser, bool v)
{
    parser->state.breakOnColon = v;
}

static inline void _setBreakOnWhitespace(natsMessageParser *parser, bool v)
{
    parser->state.breakOnWhitespace = v;
}

static inline void _skipWhitespace(natsMessageParser *parser)
{
    parser->state.skipWhitespace = true;
}

static inline void _dontSkipWhitespace(natsMessageParser *parser)
{
    parser->state.skipWhitespace = false;
}

static inline void _do(natsMessageParser *parser, state todo)
{
    parser->state.current = todo;
}

static inline void _thenDo(natsMessageParser *parser, state todo)
{
    parser->state.next = todo;
}

static inline void _skipWhitespaceThenDo(natsMessageParser *parser, state todo)
{
    _skipWhitespace(parser);
    _do(parser, todo);
}

static inline void _skipWhitespaceThenDoThenDo(natsMessageParser *parser, state todo, state then)
{
    _skipWhitespace(parser);
    _do(parser, todo);
    _thenDo(parser, then);
}

static inline void _doNow(natsMessageParser *parser, state todo)
{
    _dontSkipWhitespace(parser);
    _do(parser, todo);
}

static inline void _doNowThenDo(natsMessageParser *parser, state todo, state then)
{
    _dontSkipWhitespace(parser);
    _do(parser, todo);
    _thenDo(parser, then);
}

static inline natsStatus _startAccumulating(natsMessageParser *parser, state then, const natsValidBitmask *valid)
{
    natsStatus s = nats_resetBuf(parser->acc);
    if (!STILL_OK(s))
        return s;

    CONNTRACEf("<>/<> start accumulating: %s, EOL=%d, nextState: %d, pos:%zu",
               nats_printableString(nats_bufAsString(parser->acc)), parser->state.EOL, then, parser->pos);
    if (valid != NULL)
    {
        parser->accValidChars = *valid;
        parser->state.validateChars = true;
    }
    else
    {
        parser->state.validateChars = false;
    }

    // Skip all leading whitespace, then reset to false once we reach _accumulate.
    _skipWhitespaceThenDoThenDo(parser, stateAccumulate, then);
    return NATS_OK;
}

static inline void _finishAccumulating(natsMessageParser *parser)
{
    CONNTRACEf("<>/<> finished accumulating: %s, EOL=%d, nextState: %d, pos:%zu",
               nats_printableString(nats_bufAsString(parser->acc)), parser->state.EOL, parser->state.next, parser->pos);
    _doNowThenDo(parser, parser->state.next, stateInvalid);
}

static inline natsStatus _accummulate(natsMessageParser *parser, uint8_t ch)
{
    natsStatus s = NATS_OK;

    // as soon as we get here (first character), we are no longer skipping
    // whitespace.
    _dontSkipWhitespace(parser);

    IFOK(s, nats_validateByte(ch, parser->accValidChars));
    IFOK(s, nats_appendB(parser->acc, ch));
    if (STILL_OK(s))
        CONNTRACEf("stateAccumulate: added %s", nats_printableByte(ch));
    return s;
}

static inline void _requireCRLF(natsMessageParser *parser, state then)
{
    _doNowThenDo(parser, stateRequireCRLF, then);
}

static inline void _requireLF(natsMessageParser *parser, state then)
{
    _doNowThenDo(parser, stateRequireLF, then);
}

#define _msgErrorf(_f, ...) \
    nats_setErrorf(NATS_PROTOCOL_ERROR, "message parsing error: %s: " _f, nats_printableString(&parser->msg->subject), __VA_ARGS__)
#define _msgError(_f) \
    nats_setErrorf(NATS_PROTOCOL_ERROR, "message parsing error: %s: " _f, nats_printableString(&parser->msg->subject))

static inline natsStatus _saveVararg(natsMessageParser *parser)
{
    if (nats_bufLen(parser->acc) == 0)
        return _msgError("<>/<> internal error: unreachable: empty vararg in message line");
    if (parser->numVarargs >= NATS_MAX_MESSAGE_HEADER_ARGS)
        return _msgErrorf("too many args: %d", parser->numVarargs + 1);

    natsStatus s = nats_pdupBytes(&parser->varargs[parser->numVarargs], parser->msg->pool, nats_bufAsBytes(parser->acc));
    if (STILL_OK(s))
    {
        CONNDEBUGf("added vararg #%d: %s", parser->numVarargs, nats_printableBytes(&parser->varargs[parser->numVarargs], 64));
        parser->numVarargs++;
    }
    return s;
}

static natsStatus _processMessageVarargs(natsMessageParser *parser)
{
    natsStatus s = NATS_OK;

    if (parser->state.isHMSG)
    {
        switch (parser->numVarargs)
        {
        case 2: // totalBytes headerBytes
            IFOK(s, nats_strToSizet(&parser->msg->x.in.totalLen, parser->varargs[0].bytes, parser->varargs[0].len));
            IFOK(s, nats_strToSizet(&parser->msg->x.in.headerLen, parser->varargs[1].bytes, parser->varargs[1].len));
            break;
        case 3: // reply-to totalBytes headerBytes
            IFOK(s, ALWAYS_OK(parser->msg->reply = *nats_bytesAsString(&parser->varargs[0])));
            IFOK(s, nats_strToSizet(&parser->msg->x.in.totalLen, parser->varargs[0].bytes, parser->varargs[1].len));
            IFOK(s, nats_strToSizet(&parser->msg->x.in.headerLen, parser->varargs[1].bytes, parser->varargs[2].len));
            break;
        default:
            s = _msgErrorf("HMSG: expected 4 or 5 arguments, got %d", parser->numVarargs + 2);
            break;
        }
    }
    else
    {
        switch (parser->numVarargs)
        {
        case 1: // totalBytes
            IFOK(s, nats_strToSizet(&parser->msg->x.in.totalLen, parser->varargs[0].bytes, parser->varargs[0].len));
            break;
        case 2: // reply-to totalBytes
            IFOK(s, ALWAYS_OK(parser->msg->reply = *nats_bytesAsString(&parser->varargs[0])));
            IFOK(s, nats_strToSizet(&parser->msg->x.in.totalLen, parser->varargs[0].bytes, parser->varargs[1].len));
            break;
        default:
            s = _msgErrorf("HMSG: expected 3 or 4 arguments, got %d", parser->numVarargs + 2);
            break;
        }
    }

    return s;
}

natsStatus
nats_parseMessage(natsMessage **newMsg, natsMessageParser *parser, const uint8_t *data, const uint8_t *end, size_t *consumed)
{
    natsStatus s = NATS_OK;
    const uint8_t *remaining = data;
    size_t startPosThisTime = parser->pos;

    CONNTRACEf("Parsing message: '%.*s'", (int)(end - remaining), remaining);

    // Start with accumulating the subject right away.
    _setBreakOnColon(parser, false);
    _setBreakOnWhitespace(parser, true);
    s = _startAccumulating(parser, stateSubject, &NATS_VALID_SUBJECT_CHARS);

    // Run the parsing loop.
    uint8_t ch;
    for (; (STILL_OK(s)) && (parser->state.current != stateEnd); remaining++, parser->pos++)
    {
#define _reprocessChar()                \
    parser->state.reprocessChar = true; \
    parser->pos--;                      \
    remaining--;

        // Get the next character to process, or re-process the last one if
        // applicable. If we have reached the end of the buffer we are done.
        if (!parser->state.reprocessChar)
        {
            if (end - remaining == 0)
            {
                if (consumed != NULL)
                    *consumed = parser->pos - startPosThisTime;
                return NATS_OK;
            }
            ch = *remaining;
        }
        else
        {
            parser->state.reprocessChar = false;
        }

        CONNDEBUGf("<>/<> %d %c skipws: %d", parser->state.current, ch, parser->state.skipWhitespace);

        // Special handling of certain characters, applicable to most states.
        switch (ch)
        {
        case 0:
            if (parser->state.current != statePayload)
                s = _msgError("message header contains a null byte");
            continue;

        case ' ':
        case '\t':
            if (parser->state.skipWhitespace)
                continue;
            break;

        case '\r':
        case '\n':
            parser->state.EOL = true;
            parser->lineLen = 0;
            break;

        default:
            parser->state.EOL = false;
            parser->lineLen++;
            break;
        }
        if (parser->lineLen > NATS_MAX_MESSAGE_HEADER_LINE)
        {
            s = _msgErrorf("header line exceeds maximum length of %d", NATS_MAX_MESSAGE_HEADER_LINE);
            continue;
        }

        // Run the state machine.
        switch (parser->state.current)
        {
        case stateAccumulate:
            if (parser->state.EOL ||
                (parser->state.breakOnColon && (ch == ':')) ||
                (parser->state.breakOnWhitespace && (ch == ' ' || ch == '\t')))
            {
                _finishAccumulating(parser);
                _reprocessChar();
            }
            else
            {
                s = _accummulate(parser, ch);
            }
            continue;

        case stateSubject:
            if (parser->state.EOL)
            {
                s = _msgError("MSG line terminated early, before the mandatory subscription ID");
                continue;
            }
            if (nats_bufLen(parser->acc) == 0)
            {
                s = _msgError("MSG line does not contain a subject");
                continue;
            }

            // Save the subject into the message right away.
            s = nats_pdupString(&parser->msg->subject, parser->msg->pool, nats_bufAsString(parser->acc));
            CONNDEBUGf("stateSubject: set message subject to %s", nats_printableString(&parser->msg->subject));

            // Go on to collect SSID
            IFOK(s, _startAccumulating(parser, stateSSID, &NATS_VALID_DIGITS_ONLY));
            continue; // stateSubject

        case stateSSID:
            if (parser->state.EOL)
            {
                s = _msgError("MSG line terminated early, before the mandatory message length");
                continue;
            }
            if (nats_bufLen(parser->acc) == 0)
            {
                s = _msgError("MSG line does not contain a subscription ID");
                continue;
            }

            // Our SSIDs are always numeric, parse the string here and store the
            // result in the message.
            s = nats_strToUint64(&parser->msg->x.in.ssid, nats_bufData(parser->acc), nats_bufLen(parser->acc));
            CONNDEBUGf("stateSSID: set message SSID to %llu", (unsigned long long)parser->msg->x.in.ssid);

            // Validate varargs as subject characters, digits are included.
            IFOK(s, _startAccumulating(parser, stateMessageVarargs, &NATS_VALID_SUBJECT_CHARS));
            continue; // stateSSID

        case stateMessageVarargs:
            s = _saveVararg(parser);
            if (parser->state.EOL)
            {
                // We are done with the varargs, we need to process them, and
                // proceed to the rest of the header.
                IFOK(s, _processMessageVarargs(parser));

                // Proceed to NATS/1.0, after a required CRLF.
                IFOK(s, ALWAYS_OK(_requireLF(parser, parser->state.isHMSG ? stateHeaderN : statePayload)));
            }
            else
            {
                // Start accumulating the next MSG vararg. Keep th
                IFOK(s, _startAccumulating(parser, stateMessageVarargs, &NATS_VALID_SUBJECT_CHARS));
            }
            CONNDEBUGf("stateMessageVarargs: %s, pos:%zu", nats_printableByte(ch), parser->pos);
            continue; // stateVarargs

        case stateRequireCRLF:
            if (ch == '\r')
                _do(parser, stateRequireLF);
            else
                s = _msgErrorf("expected CRLF, got %s", nats_printableByte(ch));
            continue;

        case stateRequireLF:
            if (ch == '\n')
                _do(parser, parser->state.next);
            else
                s = _msgErrorf("expected LF, got %s", nats_printableByte(ch));
            continue;

        case stateHeaderN:
            if (ch != 'N')
            {
                s = _msgErrorf("expected 'N[ATS/1.0]', got %s", nats_printableByte(ch));
                continue;
            }
            parser->state.current = stateHeaderNA;
            parser->msg->x.in.startPos = parser->pos;
            continue;

        case stateHeaderNA:
            if (ch == 'A')
                parser->state.current = stateHeaderNAT;
            else
                s = _msgErrorf("expected 'A', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNAT:
            if (ch == 'T')
                parser->state.current = stateHeaderNATS;
            else
                s = _msgErrorf("expected 'T', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNATS:
            if (ch == 'S')
                parser->state.current = stateHeaderNATS_;
            else
                s = _msgErrorf("expected 'S', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNATS_:
            if (ch == '/')
                parser->state.current = stateHeaderNATS_1;
            else
                s = _msgErrorf("expected '/', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNATS_1:
            if (ch == '1')
                parser->state.current = stateHeaderNATS_1_;
            else
                s = _msgErrorf("expected '1', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNATS_1_:
            if (ch == '.')
                parser->state.current = stateHeaderNATS_1_0;
            else
                s = _msgErrorf("expected '.', got %s", nats_printableByte(ch));
            continue;

        case stateHeaderNATS_1_0:
            if (ch == '0')
            {
                if (parser->state.EOL)
                {
                    // We are done with the NATS header, but require a subsequent LF.
                    _requireCRLF(parser, stateHeaderName);
                    continue;
                }
                // We have a status + optional description in the line.
                _skipWhitespaceThenDo(parser, stateHeaderStatus1);
            }
            else
            {
                s = _msgErrorf("expected '0', got %s", nats_printableByte(ch));
            }
            continue;

        case stateHeaderStatus1:
            // no need to worry about anything but digits coming in here.
            parser->msg->x.in.status = (ch - '0');
            _doNow(parser, stateHeaderStatus2); // resets skipWhitespace
            continue;

        case stateHeaderStatus2:
            parser->msg->x.in.status = (parser->msg->x.in.status * 10) + (ch - '0');
            _do(parser, stateHeaderStatus3);
            continue;

        case stateHeaderStatus3:
            parser->msg->x.in.status = (parser->msg->x.in.status * 10) + (ch - '0');

            // if we are not at the end of the line, set up to accumulate the
            // description. Otherwise proceed to the named values.
            if (parser->state.EOL)
                _requireCRLF(parser, stateHeaderName);
            else
                _setBreakOnWhitespace(parser, false);
            _setBreakOnColon(parser, false);
            s = _startAccumulating(parser, stateHeaderStatusDescription, &NATS_VALID_HEADER_VALUE_CHARS);
            continue;

        case stateHeaderStatusDescription:
            if (!parser->state.EOL)
            {
                s = _msgError("unexpected: status description terminated but not \\r\\n");
                continue;
            }
            // Put the description in the message.
            if (nats_bufLen(parser->acc) > 0)
            {
                s = nats_pdupString(&parser->msg->x.in.statusDescription, parser->msg->pool, nats_bufAsString(parser->acc));
            }
            // Proceed to the named values.
            IFOK(s, ALWAYS_OK(_requireLF(parser, stateHeaderName)));
            continue;

        case stateHeaderName:
            if (ch == '\r')
            {
                // We are done with the headers, we will be back here after the LF.
                _requireLF(parser, stateEndHeader);
                continue;
            }

            _setBreakOnColon(parser, true);
            _setBreakOnWhitespace(parser, false); // spaces are not allowed in header names, until after ':'
            s = _startAccumulating(parser, stateHeaderValue, &NATS_VALID_HEADER_NAME_CHARS);
            continue; // stateHeaderName

        case stateHeaderValue:
            CONNDEBUGf("stateHeaderValue: %s", nats_printableByte(ch));
            // gather the header name from the accumulator

            if (nats_bufLen(parser->acc) == 0)
            {
                s = _msgError("header name is empty");
                continue;
            }
            // <>/<> TODO: use positional references to the buffer to avoid the copy when possible.
            IFOK(s, nats_pdupString(&parser->headerName, parser->msg->pool, nats_bufAsString(parser->acc)));
            IFOK(s, _startAccumulating(parser, stateEndHeaderNameValue, &NATS_VALID_HEADER_VALUE_CHARS));
            continue; // stateHeaderValue

        case stateEndHeaderNameValue:
            if (!parser->state.EOL)
            {
                s = _msgError("expected CRLF, got something else");
                continue;
            }

            natsString v = NATS_EMPTY;

            IFOK(s, nats_pdupString(&v, parser->msg->pool, nats_bufAsString(parser->acc)));
            IFOK(s, nats_setMessageHeader(parser->msg, &parser->headerName, &v));
            if (!STILL_OK(s))
                continue;
            // Go back to collecting headers, we will be at the start of the line.
            _requireLF(parser, stateHeaderName);
            continue; // stateEndHeaderNameValue

        case stateEndHeader:
            CONNDEBUGf("stateEndHeader: %s, pos:%zu", nats_printableByte(ch), parser->pos);
            parser->msg->x.in.headerLen = parser->pos - parser->msg->x.in.startPos;
            _reprocessChar();
            _doNow(parser, statePayload);
            continue; // stateEndHeader

        case statePayload:
            CONNDEBUGf("<>/<> statePayload: %s, pos:%zu", nats_printableByte(ch), parser->pos);
            parser->msg->x.in.payloadStartPos = parser->pos;
            if (!parser->state.isHMSG)
                parser->msg->x.in.startPos = parser->pos;

            size_t wantToConsume = parser->msg->x.in.totalLen - parser->msg->x.in.headerLen;
            if (wantToConsume > (size_t)(end - remaining))
            {
                wantToConsume = end - remaining;
            }
            else
            {
                _requireCRLF(parser, stateEnd);
            }
            CONNDEBUGf("<>/<> stateConsume: attempting %d bytes", wantToConsume);
            remaining = remaining + wantToConsume - 1;
            parser->pos = parser->pos + wantToConsume - 1;
            CONNDEBUGf("<>/<> stateConsume: totalLen: %zu, NOW pos:%zu, remaining:%zu", parser->msg->x.in.totalLen, parser->pos, end - remaining);
            continue;

        case stateEnd: // We are done, we should not be here.
            continue;

        default:
            nats_setErrorf(NATS_ILLEGAL_STATE, "invalid message parser state: %d", parser->state);
            break;
        }
    }

    if ((STILL_OK(s)) && (parser->state.current == stateEnd))
    {
        if (consumed != NULL)
            *consumed = parser->pos - startPosThisTime;
        *newMsg = parser->msg;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

// Let go of the message, preserve the accumulator buffer, zero out everything else.
static inline void _recycleMessageParser(natsMessageParser *parser)
{
    if (parser->msg != NULL)
        nats_ReleaseMessage(parser->msg);

    natsBuf *accumulatorBufferToKeep = parser->acc;
    memset(parser, 0, sizeof(natsMessageParser));
    parser->acc = accumulatorBufferToKeep;
}

natsStatus
nats_prepareToParseMessage(natsMessageParser *parser, natsPool *pool, bool isHMSG, size_t startPos)
{
    natsStatus s = NATS_OK;

    _recycleMessageParser(parser);

    IFOK(s, nats_createMessage(&parser->msg, pool, NULL));
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    parser->state.isHMSG = isHMSG;
    parser->pos = startPos;
    _skipWhitespaceThenDo(parser, stateStart);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_initMessageParser(natsMessageParser *parser, natsPool *pool)
{
    natsStatus s = NATS_OK;

    IFOK(s, nats_getGrowableBuf(&parser->acc, pool, 0));
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    return NATS_UPDATE_ERR_STACK(s);
}
