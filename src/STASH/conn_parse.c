// Copyright 2015-2020 The NATS Authors
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

#include "json.h"
#include "conn.h"

typedef enum
{
    OP_START = 0,
    OP_END,
    OP_PLUS,
    OP_PLUS_O,
    OP_PLUS_OK,
    OP_MINUS,
    OP_MINUS_E,
    OP_MINUS_ER,
    OP_MINUS_ERR,
    OP_MINUS_ERR_SPC,
    MINUS_ERR_ARG,
    OP_M,
    OP_MS,
    OP_MSG,
    OP_MSG_SPC,
    MSG_ARG,
    OP_H,
    OP_P,
    OP_PI,
    OP_PIN,
    OP_PO,
    OP_PON,
    OP_I,
    OP_IN,
    OP_INF,
    OP_INFO,
    INFO_ARG,
    END_LINE,
    END_LINE_CR,

} natsOp;

struct __natsParser
{
    natsOp state;
    natsOp nextState;

    bool skipWhitespace;
    bool expectHeadersInMessage;

    natsStatus (*completef)(natsParser *ps, natsConnection *nc);

    union
    {
        struct
        {
            natsJSONParser *parser;
            nats_JSON *obj;
        } json;
        struct
        {
            natsMessageParser *parser;
            natsMessage *msg;
        } msg;
    } args;
};

natsStatus natsConn_createParser(natsParser **ps, natsPool *pool)
{
    return CHECK_NO_MEMORY(
        *ps = nats_palloc(pool, sizeof(natsParser)));
}

bool natsConn_expectingNewOp(natsParser *ps)
{
    return ps == NULL || ps->state == OP_START;
}

static natsStatus _completeINFO(natsParser *ps, natsConnection *nc)
{
    natsStatus s = natsConn_processInfo(nc, ps->args.json.obj);
    CONNTRACEf("ParseOp: completed INFO: %s", (STILL_OK(s) ? "OK" : "ERROR"));
    return s;
}

static natsStatus _completeMessage(natsParser *ps, natsConnection *nc)
{
    natsStatus s = NATS_OK;
    // natsStatus s = natsConn_processInfo(nc, ps->args.json.obj);
    CONNTRACEf("ParseOp: completed [H]MSG but don't know what to do with it yet: %s", (STILL_OK(s) ? "OK" : "ERROR"));
    return NATS_OK;
}

static natsStatus _completePONG(natsParser *ps, natsConnection *nc)
{
    natsStatus s = natsConn_processPong(nc);
    CONNTRACEf("ParseOp: completed PONG: %s", (STILL_OK(s) ? "OK" : "ERROR"));
    return s;
}

static natsStatus _completePING(natsParser *ps, natsConnection *nc)
{
    natsStatus s = natsConn_processPing(nc);
    CONNTRACEf("ParseOp: completed PING: %s", (STILL_OK(s) ? "OK" : "ERROR"));
    return s;
}

// parse is the fast protocol parser engine.
natsStatus
natsConn_parseOp(natsConnection *nc, uint8_t *buf, uint8_t *end, size_t *consumed)
{
    natsStatus s = NATS_OK;
    natsParser *ps = nc->ps;
    uint8_t *p = buf;
    uint8_t b;

    for (; (STILL_OK(s)) && (p < end) && (ps->state != OP_END); p++)
    {
        b = *p;

        if ((ps->skipWhitespace) && ((b == ' ') || (b == '\t')))
            continue;

        switch (ps->state)
        {
        case OP_START:
        {
            ps->skipWhitespace = false;
            switch (b)
            {
            case 'M':
            case 'm':
                ps->expectHeadersInMessage = false;
                ps->state = OP_M;
                break;
            case 'H':
            case 'h':
                ps->expectHeadersInMessage = true;
                ps->state = OP_H;
                break;
            case 'P':
            case 'p':
                ps->state = OP_P;
                break;
            // case '+':
            //     ps->state = OP_PLUS;
            //     break;
            // case '-':
            //     ps->state = OP_MINUS;
            //     break;
            case 'I':
            case 'i':
                ps->state = OP_I;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected an operation, got: '%c'", b);
            }
            continue;
        }
        case END_LINE:
        {
            switch (b)
            {
            case '\r':
                ps->state = END_LINE_CR;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a CRLF, got: '%x'", b);
            }
            continue;
        }
        case END_LINE_CR:
        {
            switch (b)
            {
            case '\n':
                ps->state = ps->nextState;
                ps->nextState = 0;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a CRLF, got: '%x'", b);
            }
            continue;
        }
        case OP_I:
        {
            switch (b)
            {
            case 'N':
            case 'n':
                ps->state = OP_IN;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
            }
            continue;
        }
        case OP_IN:
        {
            switch (b)
            {
            case 'F':
            case 'f':
                ps->state = OP_INF;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
            }
            continue;
        }
        case OP_INF:
        {
            switch (b)
            {
            case 'O':
            case 'o':
                ps->state = OP_INFO;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
            }
            continue;
        }
        case OP_INFO:
        {
            switch (b)
            {
            case ' ':
            case '\t':
                s = nats_createJSONParser(&(ps->args.json.parser), nc->opPool);
                if (s != NATS_OK)
                    continue;
                ps->args.json.obj = NULL;
                ps->state = INFO_ARG;
                ps->skipWhitespace = true;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a space, got: '%c'", b);
            }
            continue;
        }
        case INFO_ARG:
        {
            size_t consumedByJSON = 0;
            s = nats_parseJSON(&ps->args.json.obj, ps->args.json.parser, p, end, &consumedByJSON);
            p += consumedByJSON;
            if (s != NATS_OK)
                continue;

            if (ps->args.json.obj != NULL)
            {
                ps->state = END_LINE;
                ps->completef = _completeINFO;
                ps->nextState = OP_END;
            }
            continue;
        }
        case OP_H:
        {
            switch (b)
            {
            case 'M':
            case 'm':
                ps->state = OP_M;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a [H]MSG, got: '%c'", b);
            }
            continue;
        }
        case OP_M:
        {
            switch (b)
            {
            case 'S':
            case 's':
                ps->state = OP_MS;
                continue;
                ;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a [H]MSG, got: '%c'", b);
            }
            continue;
        }
        case OP_MS:
        {
            switch (b)
            {
            case 'G':
            case 'g':
                ps->state = OP_MSG;
                continue;
                ;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a HMSG, got: '%c'", b);
            }
            continue;
        }
        case OP_MSG:
        {
            switch (b)
            {
            case ' ':
            case '\t':
                s = nats_createMessageParser(&(ps->args.msg.parser), nc->opPool, ps->expectHeadersInMessage);
                if (s != NATS_OK)
                    continue;
                ps->args.msg.msg = NULL;
                ps->state = MSG_ARG;
                ps->skipWhitespace = true;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a space, got: '%c'", b);
            }
            continue;
        }
        case MSG_ARG:
        {
            size_t consumedByMsg = 0;
            s = nats_parseMessage(&ps->args.msg.msg, ps->args.msg.parser, p, end, &consumedByMsg);
            p += consumedByMsg;
            if (s != NATS_OK)
                continue;

            if (ps->args.msg.msg != NULL)
            {
                s = _completeMessage(ps, nc);
            }
            nats_ReleaseMessage(ps->args.msg.msg); // release the message even in error
            ps->state = OP_END;
            continue;
        }
        case OP_P:
        {
            switch (b)
            {
            case 'I':
            case 'i':
                ps->state = OP_PI;
                continue;
                ;
            case 'O':
            case 'o':
                ps->state = OP_PO;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a PING or PONG, got: '%c'", b);
            }
            continue;
        }
        case OP_PO:
        {
            switch (b)
            {
            case 'N':
            case 'n':
                ps->state = OP_PON;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a PONG, got: '%c'", b);
            }
            continue;
        }
        case OP_PON:
        {
            switch (b)
            {
            case 'G':
            case 'g':
                ps->state = END_LINE;
                ps->completef = _completePONG;
                ps->nextState = OP_END;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a PING, got: '%c'", b);
            }
            continue;
        }
        case OP_PI:
        {
            switch (b)
            {
            case 'N':
            case 'n':
                ps->state = OP_PIN;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a PING, got: '%c'", b);
            }
            continue;
        }
        case OP_PIN:
        {
            switch (b)
            {
            case 'G':
            case 'g':
                ps->state = END_LINE;
                ps->completef = _completePING;
                ps->nextState = OP_END;
                continue;
            default:
                s = nats_setErrorf(NATS_PROTOCOL_ERROR, "Expected a PING, got: '%c'", b);
            }
            continue;
        }
        default:
            s = nats_setErrorf(NATS_PROTOCOL_ERROR, "(unreachable) invalid state: %d", ps->state);
        }
    }

    if (consumed != NULL)
        *consumed = p - buf;

    if ((STILL_OK(s)) && (ps->state == OP_END) && (ps->completef != NULL))
    {
        s = ps->completef(ps, nc);
        ps->state = OP_START;
    }

    if (s != NATS_OK)
    {
        snprintf(nc->errStr, sizeof(nc->errStr), "Parse Error [%u]: '%.*s'", ps->state, (int)(end - p), p);
    }
    return NATS_UPDATE_ERR_STACK(s);
}
