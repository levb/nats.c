// Copyright 2015-2018 The NATS Authors
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

#ifndef MEM_STRING_H_
#define MEM_STRING_H_

#include <stddef.h>

#define NATS_EMPTY {0}
#define NATS_BYTES(_quoted_literal) {.len = sizeof(_quoted_literal) - 1, .bytes = (uint8_t *)_quoted_literal}
#define NATS_STR(_quoted_literal) {.len = sizeof(_quoted_literal) - 1, .text = (char *)_quoted_literal}
#define NATS_STRC(_str) {.len = safe_strlen(_str), .text = _str}

static natsString NATS_EMPTY_STRING = NATS_EMPTY;
static natsBytes NATS_EMPTY_BYTES = NATS_EMPTY;

static natsBytes *nats_stringAsBytes(natsString *str) { return (natsBytes *)str; }
static natsString *nats_bytesAsString(natsBytes *bytes) { return (natsString *)bytes; }

static inline void nats_clearString(natsString *str)
{
    if (str != NULL)
    {
        str->len = 0;
        str->text = NULL;
    }
}

static inline void nats_clearBytes(natsBytes *bytes)
{
    if (bytes != NULL)
    {
        bytes->len = 0;
        bytes->bytes = NULL;
    }
}

static inline bool nats_equalStrings(natsString *str1, natsString *str2)
{
    if (str1 == str2)
        return true;

    if ((str1 == NULL) || (str2 == NULL))
        return false;
    if (str1->len != str2->len)
        return false;

    int n = strncmp(str1->text, str2->text, str1->len);
    return n == 0;
}

static inline bool nats_equalsCString(const natsString *str, const char *cstr)
{
    return ((str != NULL) && (str->text == cstr)) ||
           ((str != NULL) && (cstr != NULL) && (strcmp(str->text, cstr) == 0));
}

static inline bool nats_equalsBytes(natsBytes *b1, natsBytes *b2)
{
    return nats_equalStrings(nats_bytesAsString(b1), nats_bytesAsString(b2));
}

static inline bool nats_strIsEmpty(const char *p) { return (p == NULL) || (*p == '\0'); }
static inline char nats_toLower(char c) { return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c; }
static inline char nats_toUpper(char c) { return (c >= 'a' && c <= 'z') ? (c & ~0x20) : c; }
static inline size_t safe_strlen(const char *s) { return nats_strIsEmpty(s) ? 0 : strlen(s); }
static inline char *safe_strchr(const char *s, uint8_t find) { return nats_strIsEmpty(s) ? NULL : strchr(s, (int)find); }
static inline char *safe_strrchr(const char *s, uint8_t find) { return nats_strIsEmpty(s) ? NULL : strrchr(s, (int)find); }
static inline char *safe_strstr(const char *s, const char *find) { return (nats_strIsEmpty(s) || nats_strIsEmpty(find)) ? NULL : strstr(s, find); }

static inline bool safe_streq(const char *s1, const char *s2)
{
    return ((s1 == s2) ||
            ((s1 != NULL) && (s2 != NULL) && strcmp(s1, s2) == 0));
}

#define unsafe_strlen(s) strlen(s)
#define unsafe_strchr(s, find) strchr((s), (find))
#define unsafe_strrchr(s, find) strrchr((s), (find))
#define unsafe_strstr(s, find) strstr((s), (find))
#define unsafe_streq(s1, s2) (strcmp((s1), (s2)) == 0)

static inline int nats_strFindInArray(const char **array, int count, const char *str)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(array[i], str) == 0)
            return i;
    }
    return -1;
}

static inline size_t nats_strRemoveFromArray(char **array, int count, const char *str)
{
    int i = nats_strFindInArray((const char **)array, count, str);
    if (i < 0)
        return count;

    for (int j = i + 1; j < count; j++)
        array[j - 1] = array[j];

    return count - 1;
}

natsStatus nats_strToUint64(uint64_t *result, const uint8_t *d, size_t len);

static inline natsStatus nats_strToSizet(size_t *result, const uint8_t *d, size_t len)
{
    uint64_t v = 0;
    natsStatus s = nats_strToUint64(&v, d, len);
    if (result != NULL)
        *result = (size_t)v;
    return s;
}

const char *nats_printableU(const uint8_t *data, size_t len, size_t limit);
const char *nats_printableByte(uint8_t ch);

static inline const char *nats_printableBytes(natsBytes *bb, size_t limit)
{
    if (bb == NULL)
        return "<null>";
    return nats_printableU(bb->bytes, bb->len, limit);
}

static inline const char *nats_printableString(natsString *buf)
{
    if (buf == NULL)
        return "<null>";
    return nats_printableU((const uint8_t *)buf->text, buf->len, 64);
}

static inline const char *nats_printableC(const char *str, size_t len)
{
    return nats_printableU((const uint8_t *)str, len, 64);
}

typedef struct
{
    uint64_t lower;
    uint64_t upper;
} natsValidBitmask;

static inline void nats_setValidASCIIChar(natsValidBitmask *bitmask, char ch)
{
    int index = ch / 64;
    int bit_position = ch % 64;
    uint64_t mask = (uint64_t)1 << bit_position;
    if (index == 0)
        bitmask->lower |= mask;
    else
        bitmask->upper |= mask;
}

static inline void nats_setInvalidASCIIChar(natsValidBitmask *bitmask, char ch)
{
    int index = ch / 64;
    int bit_position = ch % 64;
    uint64_t mask = (uint64_t)1 << bit_position;
    if (index == 0)
        bitmask->lower &= ~mask;
    else
        bitmask->upper &= ~mask;
}

static inline void nats_setValidASCII(natsValidBitmask *bitmask, const char *chars)
{
    while (*chars != '\0')
        nats_setValidASCIIChar(bitmask, (uint8_t)*chars++);
}

// Check if a character is valid
static inline natsStatus nats_validateByte(uint8_t ch, natsValidBitmask bitmask)
{
    bool valid = ((ch & 0x80) != 0); // pass any possible UTF through in headers, subjects
    if (!valid)
    {
        int index = ch / 64;
        int bit_position = ch % 64;
        uint64_t mask = (uint64_t)1 << bit_position;
        if (index == 0)
            valid = (bitmask.lower & mask) != 0;
        else
            valid = (bitmask.upper & mask) != 0;
    }
    if (!valid)
        return nats_setErrorf(NATS_ERR, "invalid character: %s", nats_printableByte(ch));
    else
        return NATS_OK;
}

// Bitmasks for valid characters, see mem_string.c
extern natsValidBitmask NATS_VALID_HEADER_NAME_CHARS;
extern natsValidBitmask NATS_VALID_HEADER_VALUE_CHARS;
extern natsValidBitmask NATS_VALID_SUBJECT_CHARS;

// Bitmask for int numbers only, simple enough to define inline.
static const natsValidBitmask NATS_VALID_DIGITS_ONLY = {.lower = 0x03FF000000000000, .upper = 0x0000000000000000};

void nats_initCharacterValidation(void);

#endif /* MEM_STRING_H_ */
