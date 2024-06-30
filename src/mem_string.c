// Copyright 2024 The NATS Authors
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

#include <ctype.h>

void nats_strlow(uint8_t *dst, uint8_t *src, size_t n)
{
    while (n)
    {
        *dst = nats_toLower(*src);
        dst++;
        src++;
        n--;
    }
}

size_t
nats_strnlen(uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {

        if (p[i] == '\0')
        {
            return i;
        }
    }

    return n;
}

uint8_t *
nats_cpystrn(uint8_t *dst, uint8_t *src, size_t n)
{
    if (n == 0)
    {
        return dst;
    }

    while (--n)
    {
        *dst = *src;

        if (*dst == '\0')
        {
            return dst;
        }

        dst++;
        src++;
    }

    *dst = '\0';

    return dst;
}

natsStatus nats_strToUint64(uint64_t *result, const uint8_t *d, size_t len)
{
    uint64_t v = 0;

    for (size_t i = 0; i < len; i++)
    {
        if ((d[i] < '0') || (d[i] > '9'))
        {
            return nats_setErrorf(NATS_ERR, "invalid number: %.*s", (int)len, d);
        }

        v = v * 10 + (d[i] - '0');
    }

    if (result != NULL)
        *result = v;
    return NATS_OK;
}

#ifdef DEV_MODE

#define PRINTBUF_SIZE 128
#define NUM_PRINT_BUFFERS 10
static char _printbuf[NUM_PRINT_BUFFERS][PRINTBUF_SIZE];
static int _printbufIndex = 0;

const char *nats_printableU(const uint8_t *data, size_t len, size_t limit)
{
    if (data == NULL)
        return "<null>";
    if (limit == 0)
        limit = len;
    if (len > limit)
        len = limit;
    const uint8_t *end = data + len;

    char *out = _printbuf[_printbufIndex++ % NUM_PRINT_BUFFERS];
    const size_t maxbuf = PRINTBUF_SIZE - 1 - 2;
    size_t i = 0;
    out[i++] = '\'';
    for (const uint8_t *p = data; (p < end) && (i < maxbuf); p++)
    {
        if (isprint(*p))
        {
            out[i++] = *p;
        }
        else if (*p == '\n')
        {
            out[i++] = '\\';
            out[i++] = 'n';
        }
        else if (*p == '\r')
        {
            out[i++] = '\\';
            out[i++] = 'r';
        }
        else
        {
            out[i++] = '?';
        }

        if (i >= maxbuf - 3)
        {
            out[i++] = '.';
            out[i++] = '.';
            out[i++] = '.';
        }
    }
    out[i++] = '\'';
    out[i] = '\0';
    return out;
}

const char *nats_printableByte(uint8_t ch)
{
    char *out = _printbuf[_printbufIndex++ % NUM_PRINT_BUFFERS];
    const size_t maxbuf = PRINTBUF_SIZE - 1;
    char chbuf[3] = {0};

    if (isprint(ch))
    {
        chbuf[0] = ch;
    }
    else if (ch == '\n')
    {
        chbuf[0] = '\\';
        chbuf[1] = 'n';
    }
    else if (ch == '\r')
    {
        chbuf[0] = '\\';
        chbuf[1] = 'r';
    }
    else
    {
        chbuf[0] = '?';
    }

    snprintf(out, maxbuf, "'%s' (0x%2X)", chbuf, ch);
    return out;
}

// NATS_VALID_HEADER_NAME_CHARS reports whether c is a valid byte in a header
// field name. RFC 7230 says:
//
//  - header-field   = field-name ":" OWS field-value OWS
//  - field-name     = token
//  - tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" /
//          "_" / "`" / "|" / "~" / DIGIT / ALPHA
//  - token = 1*tchar
#define _ALPHA "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define _DIGITS "0123456789"
#define _NAME_EXTRA_CHARS "!#$%&'*+-.^_`|~"
#define _NAMECHARS _ALPHA _DIGITS _NAME_EXTRA_CHARS
natsValidBitmask NATS_VALID_HEADER_NAME_CHARS = NATS_EMPTY;

// NATS_VALID_HEADER_VALUE_CHARS reports whether c is a valid byte in a header
// field value. RFC 7230 says:
//
//	- field-content  = field-vchar [ 1*( SP / HTAB ) field-vchar ]
//	- field-vchar    = VCHAR | obs-text
//	- obs-text       = %x80-FF
//
// RFC 5234 says:
//
//	- HTAB           =  %x09
//	- SP             =  %x20
//	- VCHAR          =  %x21-7E

#define _VCHAR "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
#define _VALUE_EXTRA_CHARS " \t"
#define _VALUECHARS _VCHAR _VALUE_EXTRA_CHARS
natsValidBitmask NATS_VALID_HEADER_VALUE_CHARS = NATS_EMPTY;

// NATS_VALID_SUBJECT_CHARS reports whether c is a valid byte in a subject.
natsValidBitmask NATS_VALID_SUBJECT_CHARS = NATS_EMPTY;

void nats_initCharacterValidation(void)
{
    nats_setValidASCII(&NATS_VALID_HEADER_NAME_CHARS, _NAMECHARS);
    nats_setValidASCII(&NATS_VALID_HEADER_VALUE_CHARS, _VALUECHARS);

    // In subjects, everything is allowed except for spaces and control
    // characters.
    NATS_VALID_SUBJECT_CHARS.lower = 0xFFFFFFFFFFFFFFFF;
    NATS_VALID_SUBJECT_CHARS.upper = 0xFFFFFFFFFFFFFFFF;
    for (int i = 0; i < 0x20; i++)
        nats_setInvalidASCIIChar(&NATS_VALID_SUBJECT_CHARS, i);
    nats_setInvalidASCIIChar(&NATS_VALID_SUBJECT_CHARS, 0x7F);
    nats_setInvalidASCIIChar(&NATS_VALID_SUBJECT_CHARS, ' ');
}

#endif // DEV_MODE
