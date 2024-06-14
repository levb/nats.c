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

#include <string.h>
#include <stdio.h>

#include "natsp.h"

#include "url.h"
#include "servers.h"
#include "json.h"
#include "conn.h"

// cloneMsgArg is used when the split buffer scenario has the pubArg in the existing read buffer, but
// we need to hold onto it into the next read.
// static natsStatus
// _cloneMsgArg(natsConnection *nc)
// {
//     natsStatus  s;
//     natsParser  *ps = nc->ps;
//     int         subjLen = natsBuf_Len(ps->ma.subject);

//     s = natsBuf_InitWith(&(ps->argBufRec),
//                                 ps->scratch,
//                                 0,
//                                 sizeof(ps->scratch));
//     if (s == NATS_OK)
//     {
//         ps->argBuf = &(ps->argBufRec);

//         s = natsBuf_Append(ps->argBuf,
//                            natsBuf_Data(ps->ma.subject),
//                            natsBuf_Len(ps->ma.subject));
//         if (s == NATS_OK)
//         {
//             natsBuf_Destroy(ps->ma.subject);
//             ps->ma.subject = NULL;

//             s = natsBuf_InitWith(&(ps->ma.subjectRec),
//                                         ps->scratch,
//                                         subjLen,
//                                         subjLen);
//             if (s == NATS_OK)
//                 ps->ma.subject = &(ps->ma.subjectRec);
//         }
//     }
//     if ((s == NATS_OK) && (ps->ma.reply != NULL))
//     {
//         s = natsBuf_Append(ps->argBuf,
//                            natsBuf_Data(ps->ma.reply),
//                            natsBuf_Len(ps->ma.reply));
//         if (s == NATS_OK)
//         {
//             int replyLen = natsBuf_Len(ps->ma.reply);

//             natsBuf_Destroy(ps->ma.reply);
//             ps->ma.reply = NULL;

//             s = natsBuf_InitWith(&(ps->ma.replyRec),
//                                         ps->scratch + subjLen,
//                                         replyLen,
//                                         replyLen);
//             if (s == NATS_OK)
//                 ps->ma.reply = &(ps->ma.replyRec);
//         }
//     }

//     return s;
// }

// struct slice
// {
//     uint8_t *start;
//     int     len;
// };

// static natsStatus
// _processMsgArgs(natsParser *ps, natsConnection *nc, uint8_t *buf, int bufLen)
// {
//     natsStatus      s       = NATS_OK;
//     int             start   = -1;
//     int             index   = 0;
//     int             i;
//     uint8_t         b;
//     struct slice    slices[5];
//     char            errTxt[256];
//     int             indexLimit = 3;
//     int             minArgs    = 3;
//     int             maxArgs    = 4;
//     bool            hasHeaders = (ps->hdr >= 0 ? true : false);

//     // If headers, the content should be:
//     // <subject> <sid> [reply] <hdr size> <overall size>
//     // otherwise:
//     // <subject> <sid> [reply] <overall size>
//     if (hasHeaders)
//     {
//         indexLimit = 4;
//         minArgs    = 4;
//         maxArgs    = 5;
//     }

//     for (i = 0; i < bufLen; i++)
//     {
//         b = buf[i];

//         if (((b == ' ') || (b == '\t') || (b == '\r') || (b == '\n')))
//         {
//             if (start >=0)
//             {
//                 if (index > indexLimit)
//                 {
//                     s = NATS_PROTOCOL_ERROR;
//                     break;
//                 }
//                 slices[index].start = buf + start;
//                 slices[index].len   = i - start;
//                 index++;
//                 start = -1;
//             }
//         }
//         else if (start < 0)
//         {
//             start = i;
//         }
//     }
//     if ((s == NATS_OK) && (start >= 0))
//     {
//         if (index > indexLimit)
//         {
//             s = NATS_PROTOCOL_ERROR;
//         }
//         else
//         {
//             slices[index].start = buf + start;
//             slices[index].len   = i - start;
//             index++;
//         }
//     }
//     if ((s == NATS_OK) && ((index == minArgs) || (index == maxArgs)))
//     {
//         int maSizeIndex  = index-1; // position of size is always last.
//         int hdrSizeIndex = index-2; // position of hdr size is always before last.

//         s = natsBuf_InitWith(&(ps->ma.subjectRec),
//                                     slices[0].start,
//                                     slices[0].len,
//                                     slices[0].len);
//         if (s == NATS_OK)
//         {
//             ps->ma.subject = &(ps->ma.subjectRec);

//             ps->ma.sid   = nats_ParseInt64((const char *)slices[1].start, slices[1].len);

//             if (index == minArgs)
//             {
//                 ps->ma.reply = NULL;
//             }
//             else
//             {
//                 s = natsBuf_InitWith(&(ps->ma.replyRec),
//                                             slices[2].start,
//                                             slices[2].len,
//                                             slices[2].len);
//                 if (s == NATS_OK)
//                 {
//                     ps->ma.reply = &(ps->ma.replyRec);
//                 }
//             }
//         }
//         if (s == NATS_OK)
//         {
//             if (hasHeaders)
//             {
//                 ps->ma.hdr = (int) nats_ParseInt64((const char*)slices[hdrSizeIndex].start,
//                                                        slices[hdrSizeIndex].len);
//             }
//             ps->ma.size = (int) nats_ParseInt64((const char*)slices[maSizeIndex].start,
//                                                     slices[maSizeIndex].len);
//         }
//     }
//     else
//     {
//         snprintf(errTxt, sizeof(errTxt), "%s", "processMsgArgs Parse Error: wrong number of arguments");
//         s = NATS_PROTOCOL_ERROR;
//     }
//     if (ps->ma.sid < 0)
//     {
//         snprintf(errTxt, sizeof(errTxt), "processMsgArgs Bad or Missing Sid: '%.*s'",
//                  bufLen, buf);
//         s = NATS_PROTOCOL_ERROR;
//     }
//     if (ps->ma.size < 0)
//     {
//         snprintf(errTxt, sizeof(errTxt), "processMsgArgs Bad or Missing Size: '%.*s'",
//                  bufLen, buf);
//         s = NATS_PROTOCOL_ERROR;
//     }
//     if (hasHeaders && ((ps->ma.hdr < 0) || (ps->ma.hdr > ps->ma.size)))
//     {
//         snprintf(errTxt, sizeof(errTxt), "processMsgArgs Bad or Missing Header Size: '%.*s'",
//                  bufLen, buf);
//         s = NATS_PROTOCOL_ERROR;
//     }

//     if (s != NATS_OK)
//     {
//         // natsConn_Lock(nc); <>//<>
//         snprintf(nc->errStr, sizeof(nc->errStr), "%s", errTxt);
//         nc->err = s;
//         // natsConn_Unlock(nc);
//     }

//     return s;
// }

static natsStatus _completeINFO(natsParser *ps, natsConnection *nc)
{
    natsStatus s = natsConn_processInfo(nc, ps->json);
    CONNLOGf("ParseOp: completed INFO: %s", (s == NATS_OK ? "OK" : "ERROR"));
    return s;
}

// parse is the fast protocol parser engine.
natsStatus
natsParser_ParseOp(natsParser *ps, natsConnection *nc, uint8_t *buf, uint8_t *end, size_t *consumed)
{
    natsStatus s = NATS_OK;
    uint8_t *p = buf;
    uint8_t b;

    for (; (s == NATS_OK) && (p < end) && (ps->state != OP_END); p++)
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
            // case 'M':
            // case 'm':
            //     ps->state = OP_M;
            //     ps->hdr = -1;
            //     ps->ma.hdr = -1;
            //     break;
            // case 'H':
            // case 'h':
            //     ps->state = OP_H;
            //     ps->hdr = 0;
            //     ps->ma.hdr = 0;
            //     break;
            // case 'P':
            // case 'p':
            //     ps->state = OP_P;
            //     break;
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
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected an operation, got: '%c'", b);
            }
            continue;
        }
        case CRLF:
        {
            switch (b)
            {
            case '\r':
                ps->state = CRLF_CR;
                continue;
            default:
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected a CRLF, got: '%x'", b);
            }
            continue;
        }
        case CRLF_CR:
        {
            switch (b)
            {
            case '\n':
                ps->state = ps->nextState;
                ps->nextState = 0;
                continue;
            default:
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected a CRLF, got: '%x'", b);
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
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
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
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
            }
            continue;
        }
        case OP_INF:
        {
            CONNLOGf("ParseOp OP_INF: '%c'", b);
            switch (b)
            {
            case 'O':
            case 'o':
                ps->state = OP_INFO;
                continue;
            default:
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected INFO, got: '%c'", b);
            }
            continue;
        }
        case OP_INFO:
        {
            CONNLOGf("ParseOp OP_INFO: '%c'", b);
            switch (b)
            {
            case ' ':
            case '\t':
                s = natsJSONParser_Create(&(ps->jsonParser), nc->opPool);
                if (s != NATS_OK)
                    continue;
                ps->json = NULL;
                ps->state = INFO_ARG;
                ps->skipWhitespace = true;
                continue;
            default:
                s = nats_setError(NATS_PROTOCOL_ERROR, "Expected a space, got: '%c'", b);
            }
            continue;
        }
        case INFO_ARG:
        {
            size_t consumedByJSON = 0;
            s = natsJSONParser_Parse(&ps->json, ps->jsonParser, p, end, &consumedByJSON);
            p += consumedByJSON;
            if (s != NATS_OK)
                continue;

            if (ps->json != NULL)
            {
                ps->state = CRLF;
                ps->completef = _completeINFO;
                ps->nextState = OP_END;
            }
            continue;
        }
            //             case OP_H:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'M':
            //                     case 'm':
            //                         ps->state = OP_M;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_M:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'S':
            //                     case 's':
            //                         ps->state = OP_MS;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MS:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'G':
            //                     case 'g':
            //                         ps->state = OP_MSG;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MSG:
            //             {
            //                 switch (b)
            //                 {
            //                     case ' ':
            //                     case '\t':
            //                         ps->state = OP_MSG_SPC;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MSG_SPC:
            //             {
            //                 switch (b)
            //                 {
            //                     case ' ':
            //                     case '\t':
            //                         continue;
            //                     default:
            //                         ps->state      = MSG_ARG;
            //                         ps->afterSpace = i;
            //                         break;
            //                 }
            //                 break;
            //             }
            //             case MSG_ARG:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\r':
            //                         ps->drop = 1;
            //                         break;
            //                     case '\n':
            //                     {
            //                         uint8_t *start = NULL;
            //                         size_t  len    = 0;

            //                         if (ps->argBuf != NULL)
            //                         {
            //                             start = natsBuf_Data(ps->argBuf);
            //                             len   = natsBuf_Len(ps->argBuf);
            //                         }
            //                         else
            //                         {
            //                             start = buf + ps->afterSpace;
            //                             len   = (i - ps->drop) - ps->afterSpace;
            //                         }

            //                         s = _processMsgArgs(ps, nc, start, len);
            //                         if (s == NATS_OK)
            //                         {
            //                             ps->drop        = 0;
            //                             ps->afterSpace  = i+1;
            //                             ps->state       = MSG_PAYLOAD;

            //                             // jump ahead with the index. If this overruns
            //                             // what is left we fall out and process split
            //                             // buffer.
            //                             i = ps->afterSpace + ps->ma.size - 1;
            //                         }
            //                         break;
            //                     }
            //                     default:
            //                     {
            //                         if (ps->argBuf != NULL)
            //                             s = natsBuf_AppendByte(ps->argBuf, b);
            //                         break;
            //                     }
            //                 }
            //                 break;
            //             }
            //             case MSG_PAYLOAD:
            //             {
            //                 bool done = false;

            //                 if (ps->msgBuf != NULL)
            //                 {
            //                     if (natsBuf_Len(ps->msgBuf) >= ps->ma.size)
            //                     {
            //                         s = natsConn_processMsg(nc,
            //                                                 natsBuf_Data(ps->msgBuf),
            //                                                 natsBuf_Len(ps->msgBuf));
            //                         done = true;
            //                     }
            //                     else
            //                     {
            //                         // copy as much as we can to the buffer and skip ahead.
            //                         int toCopy = ps->ma.size - natsBuf_Len(ps->msgBuf);
            //                         int avail  = bufLen - i;

            //                         if (avail < toCopy)
            //                             toCopy = avail;

            //                         if (toCopy > 0)
            //                         {
            //                             s = natsBuf_Append(ps->msgBuf, buf+i, toCopy);
            //                             if (s == NATS_OK)
            //                                 i += toCopy - 1;
            //                         }
            //                         else
            //                         {
            //                             s = natsBuf_AppendByte(ps->msgBuf, b);
            //                         }
            //                     }
            //                 }
            //                 else if (i-ps->afterSpace >= ps->ma.size)
            //                 {
            //                     uint8_t *start  = NULL;
            //                     size_t  len     = 0;

            //                     start = buf + ps->afterSpace;
            //                     len   = (i - ps->afterSpace);

            //                     s = natsConn_processMsg(nc, start, len);

            //                     done = true;
            //                 }

            //                 if (done)
            //                 {
            //                     natsBuf_Destroy(ps->argBuf);
            //                     ps->argBuf = NULL;
            //                     natsBuf_Destroy(ps->msgBuf);
            //                     ps->msgBuf = NULL;
            //                     ps->state = MSG_END;
            //                 }

            //                 break;
            //             }
            //             case MSG_END:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\n':
            //                         ps->drop        = 0;
            //                         ps->afterSpace  = i+1;
            //                         ps->state       = OP_START;
            //                         break;
            //                     default:
            //                         continue;
            //                 }
            //                 break;
            //             }
            //             case OP_PLUS:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'O':
            //                     case 'o':
            //                         ps->state = OP_PLUS_O;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PLUS_O:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'K':
            //                     case 'k':
            //                         ps->state = OP_PLUS_OK;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PLUS_OK:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\n':
            //                         natsConn_processOK(nc);
            //                         ps->drop  = 0;
            //                         ps->state = OP_START;
            //                         break;
            //                 }
            //                 break;
            //             }
            //             case OP_MINUS:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'E':
            //                     case 'e':
            //                         ps->state = OP_MINUS_E;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MINUS_E:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'R':
            //                     case 'r':
            //                         ps->state = OP_MINUS_ER;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MINUS_ER:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'R':
            //                     case 'r':
            //                         ps->state = OP_MINUS_ERR;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MINUS_ERR:
            //             {
            //                 switch (b)
            //                 {
            //                     case ' ':
            //                     case '\t':
            //                         ps->state = OP_MINUS_ERR_SPC;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_MINUS_ERR_SPC:
            //             {
            //                 switch (b)
            //                 {
            //                     case ' ':
            //                     case '\t':
            //                         continue;
            //                     default:
            //                         ps->state       = MINUS_ERR_ARG;
            //                         ps->afterSpace  = i;
            //                         break;
            //                 }
            //                 break;
            //             }
            //             case MINUS_ERR_ARG:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\r':
            //                         ps->drop = 1;
            //                         break;
            //                     case '\n':
            //                     {
            //                         // uint8_t *start = NULL;
            //                         // size_t  len    = 0;

            //                         // if (ps->argBuf != NULL)
            //                         // {
            //                         //     start = natsBuf_Data(ps->argBuf);
            //                         //     len   = natsBuf_Len(ps->argBuf);
            //                         // }
            //                         // else
            //                         // {
            //                         //     start = buf + ps->afterSpace;
            //                         //     len   = (i - ps->drop) - ps->afterSpace;
            //                         // }

            //                         // <>//<>
            //                         // natsConn_processErr(nc, start, len);

            //                         ps->drop        = 0;
            //                         ps->afterSpace  = i+1;
            //                         ps->state       = OP_START;

            //                         if (ps->argBuf != NULL)
            //                         {
            //                             natsBuf_Destroy(ps->argBuf);
            //                             ps->argBuf = NULL;
            //                         }

            //                         break;
            //                     }
            //                     default:
            //                     {
            //                         if (ps->argBuf != NULL)
            //                             s = natsBuf_AppendByte(ps->argBuf, b);

            //                         break;
            //                     }
            //                 }
            //                 break;
            //             }
            //             case OP_P:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'I':
            //                     case 'i':
            //                         ps->state = OP_PI;
            //                         break;
            //                     case 'O':
            //                     case 'o':
            //                         ps->state = OP_PO;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PO:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'N':
            //                     case 'n':
            //                         ps->state = OP_PON;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PON:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'G':
            //                     case 'g':
            //                         ps->state = OP_PONG;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PONG:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\n':
            //                         // <>//<>
            //                         // natsConn_processPong(nc);

            //                         ps->drop        = 0;
            //                         ps->afterSpace  = i+1;
            //                         ps->state       = OP_START;
            //                         break;
            //                 }
            //                 break;
            //             }
            //             case OP_PI:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'N':
            //                     case 'n':
            //                         ps->state = OP_PIN;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PIN:
            //             {
            //                 switch (b)
            //                 {
            //                     case 'G':
            //                     case 'g':
            //                         ps->state = OP_PING;
            //                         break;
            //                     default:
            //                         goto parseErr;
            //                 }
            //                 break;
            //             }
            //             case OP_PING:
            //             {
            //                 switch (b)
            //                 {
            //                     case '\n':
            //                         // <>//<>
            //                         // natsConn_processPing(nc);

            //                         ps->drop        = 0;
            //                         ps->afterSpace  = i+1;
            //                         ps->state       = OP_START;
            //                         break;
            //                 }
            //                 break;
            //             }
        default:
            s = nats_setError(NATS_PROTOCOL_ERROR, "(unreachable) invalid state: %d", ps->state);
        }
    }

    //     // Check for split buffer scenarios
    //     if ((s == NATS_OK)
    //         && ((ps->state == MSG_ARG)
    //                 || (ps->state == MINUS_ERR_ARG)
    //                 || (ps->state == INFO_ARG))
    //         && (ps->argBuf == NULL))
    //     {
    //         s = natsBuf_InitWith(&(ps->argBufRec),
    //                                     ps->scratch,
    //                                     0,
    //                                     sizeof(ps->scratch));
    //         if (s == NATS_OK)
    //         {
    //             ps->argBuf = &(ps->argBufRec);
    //             s = natsBuf_Append(ps->argBuf,
    //                                buf + ps->afterSpace,
    //                                (i - ps->drop) - ps->afterSpace);
    //         }
    //     }
    //     // Check for split msg
    //     if ((s == NATS_OK)
    //         && (ps->state == MSG_PAYLOAD) && (ps->msgBuf == NULL))
    //     {
    //         // We need to clone the msgArg if it is still referencing the
    //         // read buffer and we are not able to process the msg.
    //         if (ps->argBuf == NULL)
    //             s = _cloneMsgArg(nc);

    //         if (s == NATS_OK)
    //         {
    //             size_t remainingInScratch;
    //             size_t toCopy;

    // #ifdef _WIN32
    // // Suppresses the warning that ps->argBuf may be NULL.
    // // If ps->argBuf is NULL above, then _cloneMsgArg() will set it. If 's'
    // // is NATS_OK here, then ps->argBuf can't be NULL.
    // #pragma warning(suppress: 6011)
    // #endif

    //             // If we will overflow the scratch buffer, just create a
    //             // new buffer to hold the split message.
    //             remainingInScratch = sizeof(ps->scratch) - natsBuf_Len(ps->argBuf);
    //             toCopy = bufLen - ps->afterSpace;

    //             if (ps->ma.size > remainingInScratch)
    //             {
    //                 s = natsBuf_CreateCalloc(&(ps->msgBuf), ps->ma.size);
    //             }
    //             else
    //             {
    //                 s = natsBuf_InitWith(&(ps->msgBufRec),
    //                                             ps->scratch + natsBuf_Len(ps->argBuf),
    //                                             0, remainingInScratch);
    //                 if (s == NATS_OK)
    //                     ps->msgBuf = &(ps->msgBufRec);
    //             }
    //             if (s == NATS_OK)
    //                 s = natsBuf_Append(ps->msgBuf,
    //                                    buf + ps->afterSpace,
    //                                    toCopy);
    //         }
    //     }

    //     if (s != NATS_OK)
    //     {
    //         // Let's clear all our pointers...
    //         natsBuf_Destroy(ps->argBuf);
    //         ps->argBuf = NULL;
    //         natsBuf_Destroy(ps->msgBuf);
    //         ps->msgBuf = NULL;
    //         natsBuf_Destroy(ps->ma.subject);
    //         ps->ma.subject = NULL;
    //         natsBuf_Destroy(ps->ma.reply);
    //         ps->ma.reply = NULL;
    //     }

    if (consumed != NULL)
        *consumed = p - buf;

    if ((s == NATS_OK) && (ps->state == OP_END) && (ps->completef != NULL))
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

natsStatus
natsParser_Create(natsParser **newParser)
{
    natsParser *parser = (natsParser *)natsHeap_Alloc(sizeof(natsParser));

    if (parser == NULL)
        return NATS_NO_MEMORY;

    *newParser = parser;

    return NATS_OK;
}
