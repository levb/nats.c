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

#include <ctype.h>

#include "err.h"
#include "mem.h"
#include "conn.h"
#include "hash.h"
#include "util.h"
#include "json.h"

int jsonMaxNested = JSON_MAX_NEXTED;

// A long double memory size is larger (or equal to) u/int64_t, so use that
// as the maximum size of a num element in an array.
#define JSON_MAX_NUM_SIZE ((int)sizeof(long double))

#define JSON_STATE_START (0)
#define JSON_STATE_END (1)
#define JSON_STATE_FIELDS (2)
#define JSON_STATE_ELEMENTS (3)
#define JSON_STATE_COLON (4)
#define JSON_STATE_STRING (5)
#define JSON_STATE_STRING_ESCAPE (6)
#define JSON_STATE_STRING_UTF16 (7)
#define JSON_STATE_VALUE (8)
#define JSON_STATE_VALUE_STRING (9)
#define JSON_STATE_VALUE_TRUE (10)
#define JSON_STATE_VALUE_FALSE (11)
#define JSON_STATE_VALUE_NULL (12)
#define JSON_STATE_VALUE_ARRAY (13)
#define JSON_STATE_VALUE_OBJECT (14)
#define JSON_STATE_VALUE_NUMERIC (15)

struct _natsJSONParser_s
{
    int state;

    // The JSON object (or array) being parsed.
    nats_JSON *json;

    // 1 character can be pushed back and re-processed.
    uint8_t undoCh;

    // Toggles whitespace skipping.
    bool skipWhitespace;

    // The current field (or array element) being parsed.
    nats_JSONField *field;

    // Nested level for this parser, and a pointer to the next nested (in
    // chain).
    int nestedLevel;
    natsJSONParser *nested;

    // Used for parsing numbers and fixed strings 'true', 'false', 'null'.
    uint8_t scratchBuf[64];
    natsString scratch;

    // Used for parsing strings. nextState is set after parsing a string.
    natsBuffer *strBuf;
    int nextState;

    // Toggle allowing a sign, dot, or 'e'/'E' when parsing a number.
    bool numErrorOnSign;
    bool numErrorOnDot;
    bool numErrorOnE;

    // Position in the JSON string.
    int line;
    int pos;
};

static natsStatus _addValueToArray(natsJSONParser *parser);
static natsStatus _createField(nats_JSONField **newField, natsPool *pool, const uint8_t *fieldName, size_t len);
static natsStatus _createParser(natsJSONParser **newParser, natsPool *pool, bool isArray, natsJSONParser *from);
static natsStatus _finishBoolValue(natsJSONParser *parser);
static natsStatus _finishNumericValue(natsJSONParser *parser);
static natsStatus _finishNestedValue(natsJSONParser *parser, nats_JSON *obj);
static natsStatus _finishString(natsJSONParser *parser);
static natsStatus _finishValue(natsJSONParser *parser);
static void _startString(natsJSONParser *parser, int nextState);
static void _startValue(natsJSONParser *parser, int state, int typ, uint8_t firstCh);
static natsStatus _decodeUTF16(const uint8_t *data, char *val);

static inline natsStatus _addFieldToObject(natsJSONParser *parser)
{
    return natsStrHash_Set(parser->json->fields, parser->field->name, (void *)parser->field);
}

static inline void _resetScratch(natsJSONParser *parser)
{
    memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
    parser->scratch.data = parser->scratchBuf;
    parser->scratch.len = 0;
}

static inline void _resetString(natsJSONParser *parser)
{
    natsBuf_Reset(parser->strBuf);
    parser->nextState = 0;
}

static inline natsStatus _addByteToScratch(natsJSONParser *parser, uint8_t ch)
{
    if (parser->scratch.len >= (sizeof(parser->scratchBuf) - 1))
        return nats_setError(NATS_ERR, "error parsing: insufficient scratch buffer, got '%.*s'", natsString_Printable(&parser->scratch));
    parser->scratchBuf[parser->scratch.len++] = ch;
    return NATS_OK;
}

static inline natsStatus _createObjectParser(natsJSONParser **newParser, natsPool *pool, natsJSONParser *from)
{
    return _createParser(newParser, pool, false, from);
}

static inline natsStatus _createArrayParser(natsJSONParser **newParser, natsPool *pool, natsJSONParser *from)
{
    return _createParser(newParser, pool, true, from);
}

natsStatus
natsJSONParser_Create(natsJSONParser **newParser, natsPool *pool)
{
    return _createParser(newParser, pool, false, NULL);
}

natsStatus
natsJSONParser_Parse(nats_JSON **newJSON, natsJSONParser *parser, const natsString *buf, size_t *consumed)
{
    nats_JSON *json = NULL;
    natsStatus s = NATS_OK;
    size_t c = 0;
    size_t cNested = 0;
    natsString remaining = *buf;

#define _jsonError(_f, ...) \
    nats_setError(NATS_ERR, "JSON parsing error line %d, pos %d: " _f, parser->line + 1, parser->pos, __VA_ARGS__)
    JSONLOGf("Parsing JSON: '%.*s'", natsString_Printable(&remaining));

    while ((s == NATS_OK) && (parser->state != JSON_STATE_END))
    {
        // Some states don't need to consume a character, process them first.
        switch (parser->state)
        {
        case JSON_STATE_VALUE_OBJECT:
        case JSON_STATE_VALUE_ARRAY:
            json = NULL;
            s = natsJSONParser_Parse(&json, parser->nested, &remaining, &cNested);
            if (s != NATS_OK)
                continue;
            if (json != NULL)
                _finishNestedValue(parser, json);
            remaining.data += cNested;
            remaining.len -= cNested;
            c += cNested;
            continue;
        }

        // Get the next character to process.
        char ch = parser->undoCh;
        if (ch == 0)
        {
            // If we have reached the end of the buffer, and there's no "undo" character, we are done.
            if (remaining.len == 0)
            {
                if (consumed != NULL)
                    *consumed = c;
                return NATS_OK;
            }
            ch = *remaining.data;
            remaining.data++;
            remaining.len--;
            c++;
            parser->pos++;
        }
        else
        {
            parser->undoCh = 0;
        }

        if (ch == '\n')
        {
            parser->line++;
            parser->pos = 0;
            continue;
        }

        if (parser->skipWhitespace &&
            ((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n')))
            continue;

        switch (parser->state)
        {
        case JSON_STATE_START:
            if ((parser->json->array != NULL) && (ch == '['))
            {
                parser->state = JSON_STATE_ELEMENTS;
            }
            else if ((parser->json->fields != NULL) && (ch == '{'))
            {
                parser->state = JSON_STATE_FIELDS;
            }
            else
            {
                s = _jsonError("invalid character '%c', expected '{' or '[' at the start of JSON", ch);
            }
            continue; // JSON_STATE_START

        case JSON_STATE_FIELDS:
            switch (ch)
            {
            case '}':
                parser->state = JSON_STATE_END;
                parser->skipWhitespace = false; // Do not skip whitespace after the final '}'
                continue;
            case ',':
                // Ignore all commas between fields, nothing to do.
                continue;
            case '"':
                _startString(parser, JSON_STATE_COLON);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected start of a named field", ch);
                continue;
            }
            continue; // JSON_STATE_FIELDS

        case JSON_STATE_ELEMENTS:
            switch (ch)
            {
            case ']':
                parser->state = JSON_STATE_END;
                parser->skipWhitespace = false; // Do not skip whitespace after the final '}'
                continue;
            case ',':
                parser->state = JSON_STATE_VALUE;
                s = _createField(&parser->field, parser->json->pool, (uint8_t *)"array", 5);
                continue;
            default:
                parser->undoCh = ch;
                parser->state = JSON_STATE_VALUE;
                s = _createField(&parser->field, parser->json->pool, (uint8_t *)"array", 5);
                continue;
            }
            continue; // JSON_STATE_ELEMENTS

        case JSON_STATE_COLON:
            switch (ch)
            {
            case ':':
                s = _createField(&parser->field, parser->json->pool, natsBuf_Data(parser->strBuf), natsBuf_Len(parser->strBuf));
                parser->state = JSON_STATE_VALUE;
                continue;
            default:
                s = _jsonError("invalid character '%c', expected a ':'", ch);
                continue;
            }
            continue; // JSON_STATE_COLON

        case JSON_STATE_VALUE:
            switch (ch)
            {
            case '"':
                _startString(parser, JSON_STATE_VALUE_STRING);
                parser->field->typ = TYPE_STR;
                continue;
            case 'n':
                _startValue(parser, JSON_STATE_VALUE_NULL, TYPE_NULL, ch);
                continue;
            case 't':
                _startValue(parser, JSON_STATE_VALUE_TRUE, TYPE_BOOL, ch);
                continue;
            case 'f':
                _startValue(parser, JSON_STATE_VALUE_FALSE, TYPE_BOOL, ch);
                continue;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '-':
            case '+':
            case '.':
                _startValue(parser, JSON_STATE_VALUE_NUMERIC, TYPE_NUM, ch);
                parser->field->numTyp = ((ch == '-') || (ch == '+')) ? TYPE_INT : TYPE_UINT;
                continue;
            case '[':
                parser->state = JSON_STATE_VALUE_ARRAY;
                // Create a new parser for the nested object. It will consume
                // starting with the next character.
                s = _createArrayParser(&(parser->nested), parser->json->pool, parser);
                continue;
            case '{':
                parser->state = JSON_STATE_VALUE_OBJECT;
                // Create a new parser for the nested object. It will consume
                // starting with the next character.
                s = _createObjectParser(&(parser->nested), parser->json->pool, parser);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected a start of a value", ch);
                continue;
            }
            continue; // JSON_STATE_VALUE

        case JSON_STATE_VALUE_NULL:
            switch (ch)
            {
            case 'u':
            case 'l':
                s = _addByteToScratch(parser, ch);
                if ((s == NATS_OK) && (parser->scratch.len == sizeof("null") - 1))
                {
                    if (nats_strcmp(parser->scratchBuf, "null") != 0)
                    {
                        s = _jsonError("invalid string '%s', expected 'null", parser->scratch);
                        continue;
                    }
                    JSONLOGf("added field: (null) \"%s\"", parser->field->name);
                    s = _finishValue(parser);
                }
                continue;
            default:
                s = _jsonError("invalid character '%c', expected 'null'", ch);
                continue;
            }
            continue; // JSON_STATE_VALUE_NULL

        case JSON_STATE_VALUE_TRUE:
            switch (ch)
            {
            case 'r':
            case 'u':
            case 'e':
                s = _addByteToScratch(parser, ch);

                IFOK(s, (parser->scratch.len == sizeof("true") - 1) ? _finishBoolValue(parser) : NATS_OK);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected 'true'", ch);
                continue;
            }
            continue; // JSON_STATE_VALUE_TRUE

        case JSON_STATE_VALUE_FALSE:
            switch (ch)
            {
            case 'a':
            case 'l':
            case 's':
            case 'e':
                s = _addByteToScratch(parser, ch);
                IFOK(s, (parser->scratch.len == sizeof("false") - 1) ? _finishBoolValue(parser) : NATS_OK);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected 'false'", ch);
                continue;
            }
            continue; // JSON_STATE_VALUE_FALSE

        case JSON_STATE_VALUE_NUMERIC:
            switch (ch)
            {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '-':
            case '+':
            case '.':
            case 'e':
            case 'E':
                if (ch == '+' || ch == '-')
                {
                    if (parser->numErrorOnSign)
                    {
                        s = _jsonError("error parsing a number: unexpected sign after %s", parser->scratch);
                        continue;
                    }

                    parser->numErrorOnSign = true; // only 1 sign allowed
                }
                if (ch == '.')
                {
                    if (parser->numErrorOnDot)
                    {
                        s = _jsonError("error parsing a number: unexpected '.' after %s", parser->scratch);
                        continue;
                    }

                    parser->numErrorOnDot = true; // only 1 '.' allowed
                    parser->field->numTyp = TYPE_DOUBLE;
                }
                if (ch == 'e' || ch == 'E')
                {
                    if (parser->numErrorOnE)
                    {
                        s = _jsonError("error parsing a number: unexpected 'e' after %s", parser->scratch);
                        continue;
                    }

                    parser->numErrorOnE = true;     // only 1 'e' allowed
                    parser->numErrorOnSign = false; // allow sign in exponent
                    parser->field->numTyp = TYPE_DOUBLE;
                }
                s = _addByteToScratch(parser, ch);
                continue;

            default:
                // Any other character is the end of the numeric value. Return
                // the character to the input stream to re-process.
                parser->undoCh = ch;
                s = _finishNumericValue(parser);
                continue;
            }
            continue; // JSON_STATE_VALUE_NUMERIC

        case JSON_STATE_STRING:
            switch (ch)
            {
            case '"':
                // end of string
                s = _finishString(parser);
                continue;
            case '\\':
                parser->state = JSON_STATE_STRING_ESCAPE;
                continue;
            default:
                s = natsBuf_AppendByte(parser->strBuf, ch);
                continue;
            }
            continue; // JSON_STATE_STRING

        case JSON_STATE_STRING_ESCAPE:
            // Whatever character comes, the next one will not be escaped;
            // except UTF16 handled separately below.
            parser->state = JSON_STATE_STRING;

            switch (ch)
            {
            case 'b':
                s = natsBuf_AppendByte(parser->strBuf, '\b');
                continue;
            case 'f':
                s = natsBuf_AppendByte(parser->strBuf, '\f');
                continue;
            case 'n':
                s = natsBuf_AppendByte(parser->strBuf, '\n');
                continue;
            case 'r':
                s = natsBuf_AppendByte(parser->strBuf, '\r');
                continue;
            case 't':
                s = natsBuf_AppendByte(parser->strBuf, '\t');
                continue;
            case 'u':
                parser->state = JSON_STATE_STRING_UTF16;
                memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
                parser->scratch.len = 0;
                continue;
            case '"':
            case '\\':
            case '/':
                s = natsBuf_AppendByte(parser->strBuf, ch);
                continue;
            default:
                s = _jsonError("error parsing string '%s': invalid control character", ch);
                continue;
            }
            continue; // JSON_STATE_STRING_ESCAPE

        case JSON_STATE_STRING_UTF16:
            if (parser->scratch.len < sizeof("ABCD")) // hex number
            {
                _addByteToScratch(parser, ch);
                if (parser->scratch.len == sizeof("ABCD"))
                {
                    char val = 0;
                    s = _decodeUTF16(parser->scratchBuf, &val);
                    if (s != NATS_OK)
                    {
                        s = _jsonError("error parsing string '%s': invalid unicode character", parser->scratch);
                        continue;
                    }
                    s = natsBuf_AppendByte(parser->strBuf, val);
                    parser->state = JSON_STATE_STRING;
                    memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
                    parser->scratch.len = 0;
                }
            }
            continue; // JSON_STATE_STRING_UTF16

        default:
            s = _jsonError("invalid state %d", parser->state);
            break;
        }
    }

    if ((s == NATS_OK) && (parser->state == JSON_STATE_END))
    {
        if (consumed != NULL)
            *consumed = c;
        *newJSON = parser->json;
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_createParser(natsJSONParser **newParser, natsPool *pool, bool isArray, natsJSONParser *from)
{
    natsStatus s = NATS_OK;
    natsJSONParser *parser = NULL;
    nats_JSON *json = NULL;
    int nestedLevel = 0;

    if (newParser == NULL)
        s = nats_setDefaultError(NATS_INVALID_ARG);

    if (from != NULL)
        nestedLevel = from->nestedLevel + 1;
    if (nestedLevel >= jsonMaxNested)
        return nats_setError(NATS_ERR, "json reached maximum nested objects of %d", jsonMaxNested);

    IFOK(s, natsPool_AllocS((void **)&parser, pool, sizeof(natsJSONParser)));
    IFOK(s, natsPool_AllocS((void **)&json, pool, sizeof(nats_JSON)));
    if (isArray)
    {
        IFOK(s, natsPool_AllocS((void **)&json->array, pool, sizeof(nats_JSONArray)));
    }
    else
    {
        IFOK(s, natsStrHash_Create(&(json->fields), pool, 4));
    }
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    json->pool = pool;
    parser->json = json;
    parser->state = (nestedLevel == 0 ? JSON_STATE_START : (isArray ? JSON_STATE_ELEMENTS : JSON_STATE_FIELDS));
    parser->skipWhitespace = true;
    parser->nestedLevel = nestedLevel;

    if (nestedLevel)
    {
        parser->nestedLevel = nestedLevel;

        parser->undoCh = from->undoCh;
        parser->line = from->line;
        parser->pos = from->pos;
        parser->strBuf = from->strBuf;
        natsBuf_Reset(parser->strBuf);
    }
    else
    {
        IFOK(s, natsBuf_CreateInPool(&parser->strBuf, pool, 32));
        if (s != NATS_OK)
            return NATS_UPDATE_ERR_STACK(s);
    }

    *newParser = parser;
    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_createField(nats_JSONField **newField, natsPool *pool, const uint8_t *fieldName, size_t len)
{
    nats_JSONField *field = NULL;

    field = (nats_JSONField *)natsPool_Alloc(pool, sizeof(nats_JSONField));
    if (field == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    field->name = natsPool_Alloc(pool, len + 1);
    if (field->name == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    memcpy(field->name, fieldName, len);
    field->typ = TYPE_NOT_SET;

    *newField = field;

    return NATS_OK;
}

static natsStatus
_decodeUTF16(const uint8_t *data, char *val)
{
    int res = 0;
    int j;

    if (nats_strlen(data) < 4)
        return NATS_ERR;

    for (j = 0; j < 4; j++)
    {
        char c = data[j];
        if ((c >= '0') && (c <= '9'))
            c = c - '0';
        else if ((c >= 'a') && (c <= 'f'))
            c = c - 'a' + 10;
        else if ((c >= 'A') && (c <= 'F'))
            c = c - 'A' + 10;
        else
            return NATS_ERR;

        res = (res << 4) + c;
    }
    *val = (char)res;
    return NATS_OK;
}

static void _startString(natsJSONParser *parser, int nextState)
{
    _resetString(parser);
    parser->state = JSON_STATE_STRING;
    parser->nextState = nextState;
    parser->skipWhitespace = false;
}

static void _startValue(natsJSONParser *parser, int state, int typ, uint8_t firstCh)
{
    _resetScratch(parser);
    if (firstCh != 0)
        _addByteToScratch(parser, firstCh);
    parser->state = state;
    parser->skipWhitespace = false; // true for all types except arrays
    parser->field->typ = typ;

    parser->numErrorOnSign = false;
    parser->numErrorOnDot = false;
    parser->numErrorOnE = false;
}

static natsStatus _finishString(natsJSONParser *parser)
{
    natsBuf_AppendByte(parser->strBuf, '\0');

    // TODO: <>/<> Not clean to do this here, but will suffice for now
    switch (parser->nextState)
    {
    case JSON_STATE_VALUE_STRING:
        parser->field->value.vstr = natsPool_StrdupC(parser->json->pool, (char *)natsBuf_Data(parser->strBuf));
        JSONLOGf("added field: (string) \"%s\":\"%s\"", parser->field->name, parser->field->value.vstr);
        return _finishValue(parser);

    default:
        parser->state = parser->nextState;
        parser->skipWhitespace = true;
        return NATS_OK;
    }
}

static natsStatus _finishValue(natsJSONParser *parser)
{
    natsStatus s = NATS_OK;
    bool isArray = (parser->json->array != NULL);

    s = isArray ? _addValueToArray(parser) : _addFieldToObject(parser);
    if (s != NATS_OK)
        return s;

    parser->field = NULL;
    parser->state = isArray ? JSON_STATE_ELEMENTS : JSON_STATE_FIELDS;
    parser->skipWhitespace = true;
    return NATS_OK;
}

static natsStatus _finishBoolValue(natsJSONParser *parser)
{
    parser->field->typ = TYPE_BOOL;
    if (nats_strcmp(parser->scratchBuf, "true") == 0)
        parser->field->value.vbool = true;
    else if (nats_strcmp(parser->scratchBuf, "false") == 0)
        parser->field->value.vbool = false;
    else
        return nats_setError(NATS_ERR, "error parsing boolean '%s'", parser->scratch);

    JSONLOGf("added field: (bool) \"%s\":%s", parser->field->name, parser->scratchBuf);
    return _finishValue(parser);
}

static natsStatus _finishNumericValue(natsJSONParser *parser)
{
    parser->field->typ = TYPE_NUM;
    // numType has been set while scanning for '+', '-', '.', and 'e'.
    switch (parser->field->numTyp)
    {
    case TYPE_INT:
        parser->field->value.vint = strtoll((const char *)parser->scratchBuf, NULL, 10);
        JSONLOGf("added value: (int) \"%s\":%lld", parser->field->name, parser->field->value.vint);
        break;
    case TYPE_UINT:
        parser->field->value.vuint = strtoull((const char *)parser->scratchBuf, NULL, 10);
        JSONLOGf("added value: (uint) \"%s\":%lld", parser->field->name, parser->field->value.vuint);
        break;
    case TYPE_DOUBLE:
        parser->field->value.vdec = strtold((const char *)parser->scratchBuf, NULL);
        JSONLOGf("added value: (double) \"%s\":%Lf", parser->field->name, parser->field->value.vdec);
        break;
    }
    return _finishValue(parser);
}

static natsStatus _finishNestedValue(natsJSONParser *parser, nats_JSON *obj)
{
    switch (parser->state)
    {
    case JSON_STATE_VALUE_ARRAY:
        if (obj->array == NULL)
            return nats_setError(NATS_ERR, "%s", "unexpected error parsing array");
        if (obj->array->typ == TYPE_NOT_SET)
            obj->array->typ = TYPE_NULL;
        parser->field->typ = TYPE_ARRAY;
        parser->field->value.varr = obj->array;
        JSONLOGf("added array value: %d elements, type %d", obj->array->size, obj->array->typ);
        break;
    case JSON_STATE_VALUE_OBJECT:
        if (obj->fields == NULL)
            return nats_setError(NATS_ERR, "%s", "unexpected error parsing object");
        parser->field->typ = TYPE_OBJECT;
        parser->field->value.vobj = obj;
        JSONLOGf("added object value: %d fields", natsStrHash_Count(obj->fields));
        break;
    default:
        return nats_setError(NATS_ERR, "unexpected error parsing nested object '%s'", parser->field->name);
    }
    parser->nested = NULL;
    return _finishValue(parser);
}

static natsStatus
_addValueToArray(natsJSONParser *parser)
{
    nats_JSONArray *a = parser->json->array;
    nats_JSONField *field = parser->field;
    int valueType = field->typ;

    if (a->typ == TYPE_NOT_SET)
        a->typ = valueType;
    if (a->typ != valueType)
        return nats_setError(NATS_ERR, "array content of different types '%s'", field->name);

    switch (a->typ)
    {
    case TYPE_STR:
        a->eltSize = sizeof(char *);
        break;
    case TYPE_BOOL:
        a->eltSize = sizeof(bool);
        break;
    case TYPE_NUM:
        a->eltSize = JSON_MAX_NUM_SIZE;
        break;
    case TYPE_OBJECT:
        a->eltSize = sizeof(nats_JSON *);
        break;
    case TYPE_ARRAY:
        a->eltSize = sizeof(nats_JSONArray *);
        break;
    default:
        return _jsonError("array of type %d not supported", a->typ);
    }

    if (a->size + 1 > a->cap)
    {
        void **newValues = NULL;
        size_t newCap = a->cap ? 2 * a->cap : 8;
        newValues = natsPool_Alloc(parser->json->pool, newCap * a->eltSize);
        if (newValues == NULL)
            return nats_setDefaultError(NATS_NO_MEMORY);

        memcpy(newValues, a->values, a->size * a->eltSize);
        a->values = newValues;
        a->cap = newCap;
    }
    // Set value based on type
    switch (a->typ)
    {
    case TYPE_STR:
        ((char **)a->values)[a->size++] = field->value.vstr;
        break;
    case TYPE_BOOL:
        ((bool *)a->values)[a->size++] = field->value.vbool;
        break;
    case TYPE_NUM:
    {
        void *numPtr = NULL;
        size_t sz = 0;

        switch (field->numTyp)
        {
        case TYPE_INT:
            numPtr = &(field->value.vint);
            sz = sizeof(int64_t);
            break;
        case TYPE_UINT:
            numPtr = &(field->value.vuint);
            sz = sizeof(uint64_t);
            break;
        default:
            numPtr = &(field->value.vdec);
            sz = sizeof(long double);
        }
        memcpy((void *)(((char *)a->values) + (a->size * a->eltSize)), numPtr, sz);
        a->size++;
        break;
    }
    case TYPE_OBJECT:
        ((nats_JSON **)a->values)[a->size++] = field->value.vobj;
        break;
    case TYPE_ARRAY:
        ((nats_JSONArray **)a->values)[a->size++] = field->value.varr;
        break;
    }

    return NATS_OK;
}

#define JSON_GET_AS(jt, t)                                \
    natsStatus s = NATS_OK;                               \
    nats_JSONField *field = NULL;                         \
    s = nats_JSONRefField(json, fieldName, (jt), &field); \
    if ((s == NATS_OK) && (field == NULL))                \
    {                                                     \
        *value = 0;                                       \
        return NATS_OK;                                   \
    }                                                     \
    else if (s == NATS_OK)                                \
    {                                                     \
        switch (field->numTyp)                            \
        {                                                 \
        case TYPE_INT:                                    \
            *value = (t)field->value.vint;                \
            break;                                        \
        case TYPE_UINT:                                   \
            *value = (t)field->value.vuint;               \
            break;                                        \
        default:                                          \
            *value = (t)field->value.vdec;                \
        }                                                 \
    }                                                     \
    return NATS_UPDATE_ERR_STACK(s);

#define JSON_ARRAY_AS(_p, _t)                                        \
    int i;                                                           \
    _t *values = (_t *)natsPool_Alloc((_p), arr->size * sizeof(_t)); \
    if (values == NULL)                                              \
        return nats_setDefaultError(NATS_NO_MEMORY);                 \
    for (i = 0; i < arr->size; i++)                                  \
        values[i] = ((_t *)arr->values)[i];                          \
    *array = values;                                                 \
    *arraySize = arr->size;                                          \
    return NATS_OK;

#define JSON_ARRAY_AS_NUM(_p, _t)                                        \
    int i;                                                               \
    _t *values = (_t *)natsPool_Alloc((_p), arr->size * sizeof(_t));     \
    if (values == NULL)                                                  \
        return nats_setDefaultError(NATS_NO_MEMORY);                     \
    for (i = 0; i < arr->size; i++)                                      \
    {                                                                    \
        void *ptr = NULL;                                                \
        ptr = (void *)((char *)(arr->values) + (i * JSON_MAX_NUM_SIZE)); \
        values[i] = *(_t *)ptr;                                          \
    }                                                                    \
    *array = values;                                                     \
    *arraySize = arr->size;                                              \
    return NATS_OK;

#define JSON_GET_ARRAY(_p, _t, _f)                                 \
    natsStatus s = NATS_OK;                                        \
    nats_JSONField *field = NULL;                                  \
    s = nats_JSONRefArray(json, (_p), fieldName, (_t), &field);    \
    if ((s == NATS_OK) && (field == NULL))                         \
    {                                                              \
        *array = NULL;                                             \
        *arraySize = 0;                                            \
        return NATS_OK;                                            \
    }                                                              \
    else if (s == NATS_OK)                                         \
        s = (_f)(json->pool, field->value.varr, array, arraySize); \
    return NATS_UPDATE_ERR_STACK(s);

natsStatus
nats_JSONRefField(nats_JSON *json, const char *fieldName, int fieldType, nats_JSONField **retField)
{
    nats_JSONField *field = NULL;

    field = (nats_JSONField *)natsStrHash_Get(json->fields, (char *)fieldName, strlen(fieldName));
    if ((field == NULL) || (field->typ == TYPE_NULL))
    {
        *retField = NULL;
        return NATS_OK;
    }

    // Check parsed type matches what is being asked.
    switch (fieldType)
    {
    case TYPE_INT:
    case TYPE_UINT:
    case TYPE_DOUBLE:
        if (field->typ != TYPE_NUM)
            return nats_setError(NATS_INVALID_ARG,
                                 "Asked for field '%s' as type %d, but got type %d when parsing",
                                 field->name, fieldType, field->typ);
        break;
    case TYPE_BOOL:
    case TYPE_STR:
    case TYPE_OBJECT:
        if (field->typ != fieldType)
            return nats_setError(NATS_INVALID_ARG,
                                 "Asked for field '%s' as type %d, but got type %d when parsing",
                                 field->name, fieldType, field->typ);
        break;
    default:
        return nats_setError(NATS_INVALID_ARG,
                             "Asked for field '%s' as type %d, but this type does not exist",
                             field->name, fieldType);
    }
    *retField = field;
    return NATS_OK;
}

natsStatus
nats_JSONGetStr(nats_JSON *json, natsPool *pool, const char *fieldName, char **value)
{
    natsStatus s = NATS_OK;
    nats_JSONField *field = NULL;

    s = nats_JSONRefField(json, fieldName, TYPE_STR, &field);
    if (s == NATS_OK)
    {
        if ((field == NULL) || (field->value.vstr == NULL))
        {
            *value = NULL;
            return NATS_OK;
        }
        else
        {
            char *tmp = natsPool_StrdupC(pool, field->value.vstr);
            if (tmp == NULL)
                return nats_setDefaultError(NATS_NO_MEMORY);
            *value = tmp;
        }
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONRefStr(nats_JSON *json, const char *fieldName, const char **str)
{
    natsStatus s;
    nats_JSONField *field = NULL;

    s = nats_JSONRefField(json, fieldName, TYPE_STR, &field);
    if (s == NATS_OK)
    {
        if (field == NULL)
            *str = NULL;
        else
            *str = field->value.vstr;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

// natsStatus
// nats_JSONGetBytes(nats_JSON *json, const char *fieldName, unsigned char **value, int *len)
// {
//     natsStatus s;
//     const char *str = NULL;

//     *value = NULL;
//     *len = 0;

//     s = nats_JSONRefStr(json, fieldName, &str);
//     if ((s == NATS_OK) && (str != NULL))
//         s = nats_Base64_Decode(str, value, len);
//     return NATS_UPDATE_ERR_STACK(s);
// }

natsStatus
nats_JSONGetInt(nats_JSON *json, const char *fieldName, int *value)
{
    JSON_GET_AS(TYPE_INT, int);
}

natsStatus
nats_JSONGetInt32(nats_JSON *json, const char *fieldName, int32_t *value)
{
    JSON_GET_AS(TYPE_INT, int32_t);
}

natsStatus
nats_JSONGetUInt16(nats_JSON *json, const char *fieldName, uint16_t *value)
{
    JSON_GET_AS(TYPE_UINT, uint16_t);
}

natsStatus
nats_JSONGetBool(nats_JSON *json, const char *fieldName, bool *value)
{
    natsStatus s = NATS_OK;
    nats_JSONField *field = NULL;

    s = nats_JSONRefField(json, fieldName, TYPE_BOOL, &field);
    if (s == NATS_OK)
    {
        *value = (field == NULL ? false : field->value.vbool);
        return NATS_OK;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetLong(nats_JSON *json, const char *fieldName, int64_t *value)
{
    JSON_GET_AS(TYPE_INT, int64_t);
}

natsStatus
nats_JSONGetULong(nats_JSON *json, const char *fieldName, uint64_t *value)
{
    JSON_GET_AS(TYPE_UINT, uint64_t);
}

natsStatus
nats_JSONGetDouble(nats_JSON *json, const char *fieldName, long double *value)
{
    JSON_GET_AS(TYPE_DOUBLE, long double);
}

natsStatus
nats_JSONGetObject(nats_JSON *json, const char *fieldName, nats_JSON **value)
{
    natsStatus s = NATS_OK;
    nats_JSONField *field = NULL;

    s = nats_JSONRefField(json, fieldName, TYPE_OBJECT, &field);
    if (s == NATS_OK)
    {
        *value = (field == NULL ? NULL : field->value.vobj);
        return NATS_OK;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

// natsStatus
// nats_JSONGetTime(nats_JSON *json, const char *fieldName, int64_t *timeUTC)
// {
//     natsStatus s = NATS_OK;
//     char *str = NULL;

//     s = nats_JSONGetStr(json, fieldName, &str);
//     if ((s == NATS_OK) && (str == NULL))
//     {
//         *timeUTC = 0;
//         return NATS_OK;
//     }
//     else if (s != NATS_OK)
//         return NATS_UPDATE_ERR_STACK(s);

//     s = nats_parseTime(str, timeUTC);
//     NATS_FREE(str);
//     return NATS_UPDATE_ERR_STACK(s);
// }

// natsStatus
// nats_JSONRefArray(nats_JSON *json, const char *fieldName, int fieldType, nats_JSONField **retField)
// {
//     nats_JSONField *field = NULL;

//     field = (nats_JSONField *)natsStrHash_Get(json->fields, (char *)fieldName);
//     if ((field == NULL) || (field->typ == TYPE_NULL))
//     {
//         *retField = NULL;
//         return NATS_OK;
//     }

//     // Check parsed type matches what is being asked.
//     if (field->typ != TYPE_ARRAY)
//         return nats_setError(NATS_INVALID_ARG,
//                              "Field '%s' is not an array, it has type: %d",
//                              field->name, field->typ);
//     // If empty array, return NULL/OK
//     if (field->value.varr->typ == TYPE_NULL)
//     {
//         *retField = NULL;
//         return NATS_OK;
//     }
//     if (fieldType != field->value.varr->typ)
//         return nats_setError(NATS_INVALID_ARG,
//                              "Asked for field '%s' as an array of type: %d, but it is an array of type: %d",
//                              field->name, fieldType, field->typ);

//     *retField = field;
//     return NATS_OK;
// }

static natsStatus
_jsonArrayAsStrings(natsPool *pool, nats_JSONArray *arr, char ***array, int *arraySize)
{
    int i;

    char **values = natsPool_Alloc(pool, arr->size * arr->eltSize);
    if (values == NULL)
        return NATS_UPDATE_ERR_STACK(nats_setDefaultError(NATS_NO_MEMORY));

    for (i = 0; i < arr->size; i++)
    {
        values[i] = natsPool_StrdupC(pool, (char *)(arr->values[i]));
        if (values[i] == NULL)
            return NATS_UPDATE_ERR_STACK(nats_setDefaultError(NATS_NO_MEMORY));
    }

    *array = values;
    *arraySize = arr->size;
    return NATS_OK;
}

natsStatus
nats_JSONGetArrayStr(nats_JSON *json, natsPool *pool, const char *fieldName, char ***array, int *arraySize)
{
    JSON_GET_ARRAY(pool, TYPE_STR, _jsonArrayAsStrings);
}

// static natsStatus
// _jsonArrayAsStringPtrs(natsPool *pool, nats_JSONArray *arr, const char ***array, int *arraySize)
// {
//     *array = (const char **)arr->values;
//     *arraySize = arr->size;
// }

// natsStatus
// nats_JSONGetArrayStrPtr(nats_JSON *json, const char *fieldName, char ***array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_STR, _jsonArrayAsStringPtrs);
// }

// static natsStatus
// _jsonArrayAsBools(natsPool *pool, nats_JSONArray *arr, bool **array, int *arraySize)
// {
//     JSON_ARRAY_AS(pool, bool);
// }

// natsStatus
// nats_JSONGetArrayBool(nats_JSON *json, const char *fieldName, bool **array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_BOOL, _jsonArrayAsBools);
// }

// static natsStatus
// _jsonArrayAsDoubles(natsPool *pool, nats_JSONArray *arr, long double **array, int *arraySize)
// {
//     JSON_ARRAY_AS_NUM(pool, long double);
// }

// natsStatus
// nats_JSONGetArrayDouble(nats_JSON *json, const char *fieldName, long double **array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsDoubles);
// }

// static natsStatus
// _jsonArrayAsInts(natsPool *pool, nats_JSONArray *arr, int **array, int *arraySize)
// {
//     JSON_ARRAY_AS_NUM(pool, int);
// }

// natsStatus
// nats_JSONGetArrayInt(nats_JSON *json, const char *fieldName, int **array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsInts);
// }

// static natsStatus
// _jsonArrayAsLongs(natsPool *pool, nats_JSONArray *arr, int64_t **array, int *arraySize)
// {
//     JSON_ARRAY_AS_NUM(pool, int64_t);
// }

// natsStatus
// nats_JSONGetArrayLong(nats_JSON *json, const char *fieldName, int64_t **array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsLongs);
// }

// static natsStatus
// _jsonArrayAsULongs(natsPool *pool, nats_JSONArray *arr, uint64_t **array, int *arraySize)
// {
//     JSON_ARRAY_AS_NUM(pool, uint64_t);
// }

// natsStatus
// nats_JSONGetArrayULong(nats_JSON *json, const char *fieldName, uint64_t **array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsULongs);
// }

// static natsStatus
// _jsonArrayAsObjects(natsPool *pool, nats_JSONArray *arr, nats_JSON ***array, int *arraySize)
// {
//     JSON_ARRAY_AS(pool, nats_JSON *);
// }

// natsStatus
// nats_JSONGetArrayObject(nats_JSON *json, const char *fieldName, nats_JSON ***array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_OBJECT, _jsonArrayAsObjects);
// }

// static natsStatus
// _jsonArrayAsArrays(natsPool *pool, nats_JSONArray *arr, nats_JSONArray ***array, int *arraySize)
// {
//     JSON_ARRAY_AS(pool, nats_JSONArray *);
// }

// natsStatus
// nats_JSONGetArrayArray(nats_JSON *json, const char *fieldName, nats_JSONArray ***array, int *arraySize)
// {
//     JSON_GET_ARRAY(TYPE_ARRAY, _jsonArrayAsArrays);
// }

natsStatus
nats_JSONRange(nats_JSON *json, int expectedType, int expectedNumType, jsonRangeCB cb, void *userInfo)
{
    natsStrHashIter iter;
    char *fname = NULL;
    void *val = NULL;
    natsStatus s = NATS_OK;

    natsStrHashIter_Init(&iter, json->fields);
    while ((s == NATS_OK) && natsStrHashIter_Next(&iter, &fname, &val))
    {
        nats_JSONField *f = (nats_JSONField *)val;

        if (f->typ != expectedType)
            s = nats_setError(NATS_ERR, "field '%s': expected value type of %d, got %d",
                              f->name, expectedType, f->typ);
        else if ((f->typ == TYPE_NUM) && (f->numTyp != expectedNumType))
            s = nats_setError(NATS_ERR, "field '%s': expected numeric type of %d, got %d",
                              f->name, expectedNumType, f->numTyp);
        else
            s = cb(userInfo, (const char *)f->name, f);
    }
    natsStrHashIter_Done(&iter);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_EncodeTimeUTC(char *buf, size_t bufLen, int64_t timeUTC)
{
    int64_t t = timeUTC / (int64_t)1E9;
    int64_t ns = timeUTC - ((int64_t)t * (int64_t)1E9);
    struct tm tp;
    int n;

    // We will encode at most: "YYYY:MM:DDTHH:MM:SS.123456789+12:34"
    // so we need at least 35+1 characters.
    if (bufLen < 36)
        return nats_setError(NATS_INVALID_ARG,
                             "buffer to encode UTC time is too small (%d), needs 36",
                             (int)bufLen);

    if (timeUTC == 0)
    {
        snprintf(buf, bufLen, "%s", "0001-01-01T00:00:00Z");
        return NATS_OK;
    }

    memset(&tp, 0, sizeof(struct tm));
#ifdef _WIN32
    _gmtime64_s(&tp, (const __time64_t *)&t);
#else
    gmtime_r((const time_t *)&t, &tp);
#endif
    n = (int)strftime(buf, bufLen, "%FT%T", &tp);
    if (n == 0)
        return nats_setDefaultError(NATS_ERR);

    if (ns > 0)
    {
        char nsBuf[15];
        int i, nd;

        nd = snprintf(nsBuf, sizeof(nsBuf), ".%" PRId64, ns);
        for (; (nd > 0) && (nsBuf[nd - 1] == '0');)
            nd--;

        for (i = 0; i < nd; i++)
            *(buf + n++) = nsBuf[i];
    }
    *(buf + n) = 'Z';
    *(buf + n + 1) = '\0';

    return NATS_OK;
}

static natsStatus
_marshalLongVal(natsBuffer *buf, bool comma, const char *fieldName, bool l, int64_t lval, uint64_t uval)
{
    natsStatus s = NATS_OK;
    char temp[32];
    const char *start = (comma ? ",\"" : "\"");

    if (l)
        snprintf(temp, sizeof(temp), "%" PRId64, lval);
    else
        snprintf(temp, sizeof(temp), "%" PRIi64, uval);

    s = natsBuf_AppendString(buf, start);
    IFOK(s, natsBuf_AppendString(buf, fieldName));
    IFOK(s, natsBuf_AppendString(buf, "\":"));
    IFOK(s, natsBuf_AppendString(buf, temp));

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_marshalLong(natsBuffer *buf, bool comma, const char *fieldName, int64_t lval)
{
    natsStatus s = _marshalLongVal(buf, comma, fieldName, true, lval, 0);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_marshalULong(natsBuffer *buf, bool comma, const char *fieldName, uint64_t uval)
{
    natsStatus s = _marshalLongVal(buf, comma, fieldName, false, 0, uval);
    return NATS_UPDATE_ERR_STACK(s);
}

// fmtFrac formats the fraction of v/10**prec (e.g., ".12345") into the
// tail of buf, omitting trailing zeros. It omits the decimal
// point too when the fraction is 0. It returns the index where the
// output bytes begin and the value v/10**prec.
static void
fmt_frac(uint8_t *buf, int w, uint64_t v, int prec, int *nw, uint64_t *nv)
{
    // Omit trailing zeros up to and including decimal point.
    bool print = false;
    int i;
    int digit;

    for (i = 0; i < prec; i++)
    {
        digit = v % 10;
        print = print || digit != 0;
        if (print)
        {
            w--;
            buf[w] = digit + '0';
        }
        v /= 10;
    }
    if (print)
    {
        w--;
        buf[w] = '.';
    }
    *nw = w;
    *nv = v;
}

// fmtInt formats v into the tail of buf.
// It returns the index where the output begins.
static int
fmt_int(uint8_t *buf, int w, uint64_t v)
{
    if (v == 0)
    {
        w--;
        buf[w] = '0';
    }
    else
    {
        while (v > 0)
        {
            w--;
            buf[w] = v % 10 + '0';
            v /= 10;
        }
    }
    return w;
}

natsStatus
nats_marshalDuration(natsBuffer *out_buf, bool comma, const char *field_name, int64_t d)
{
    // Largest time is 2540400h10m10.000000000s
    uint8_t buf[32];
    int w = 32;
    bool neg = d < 0;
    uint64_t u = (uint64_t)(neg ? -d : d);
    int prec;
    natsStatus s = NATS_OK;
    const char *start = (comma ? ",\"" : "\"");

    if (u < 1000000000)
    {
        // Special case: if duration is smaller than a second,
        // use smaller units, like 1.2ms
        w--;
        buf[w] = 's';
        w--;
        if (u == 0)
        {
            s = natsBuf_AppendString(out_buf, start);
            IFOK(s, natsBuf_AppendString(out_buf, field_name));
            IFOK(s, natsBuf_AppendString(out_buf, "\":\"0s\""));
            return NATS_UPDATE_ERR_STACK(s);
        }
        else if (u < 1000)
        {
            // print nanoseconds
            prec = 0;
            buf[w] = 'n';
        }
        else if (u < 1000000)
        {
            // print microseconds
            prec = 3;
            // U+00B5 'µ' micro sign == 0xC2 0xB5 (in reverse?)
            buf[w] = '\xB5';
            w--; // Need room for two bytes.
            buf[w] = '\xC2';
        }
        else
        {
            // print milliseconds
            prec = 6;
            buf[w] = 'm';
        }
        fmt_frac(buf, w, u, prec, &w, &u);
        w = fmt_int(buf, w, u);
    }
    else
    {
        w--;
        buf[w] = 's';

        fmt_frac(buf, w, u, 9, &w, &u);

        // u is now integer seconds
        w = fmt_int(buf, w, u % 60);
        u /= 60;

        // u is now integer minutes
        if (u > 0)
        {
            w--;
            buf[w] = 'm';
            w = fmt_int(buf, w, u % 60);
            u /= 60;

            // u is now integer hours
            // Stop at hours because days can be different lengths.
            if (u > 0)
            {
                w--;
                buf[w] = 'h';
                w = fmt_int(buf, w, u);
            }
        }
    }

    if (neg)
    {
        w--;
        buf[w] = '-';
    }

    s = natsBuf_AppendString(out_buf, start);
    IFOK(s, natsBuf_AppendString(out_buf, field_name));
    IFOK(s, natsBuf_AppendString(out_buf, "\":\""));
    IFOK(s, natsBuf_AppendBytes(out_buf, buf + w, sizeof(buf) - w));
    IFOK(s, natsBuf_AppendString(out_buf, "\""));
    return NATS_UPDATE_ERR_STACK(s);
}

// natsStatus
// nats_marshalMetadata(natsBuffer *buf, bool comma, const char *fieldName, natsMetadata md)
// {
//     natsStatus s = NATS_OK;
//     int i;
//     const char *start = (comma ? ",\"" : "\"");

//     if (md.Count <= 0)
//         return NATS_OK;

//     IFOK(s, natsBuf_AppendString(buf, start));
//     IFOK(s, natsBuf_AppendString(buf, fieldName));
//     IFOK(s, natsBuf_Append(buf, (const uint8_t *)"\":{", 3));
//     for (i = 0; (s == NATS_OK) && (i < md.Count); i++)
//     {
//         IFOK(s, natsBuf_AppendByte(buf, '"'));
//         IFOK(s, natsBuf_AppendString(buf, md.List[i * 2]));
//         IFOK(s, natsBuf_Append(buf, (const uint8_t *)"\":\"", 3));
//         IFOK(s, natsBuf_AppendString(buf, md.List[i * 2 + 1]));
//         IFOK(s, natsBuf_AppendByte(buf, '"'));

//         if (i != md.Count - 1)
//             IFOK(s, natsBuf_AppendByte(buf, ','));
//     }
//     IFOK(s, natsBuf_AppendByte(buf, '}'));
//     return NATS_OK;
// }

// static natsStatus
// _addMD(void *closure, const char *fieldName, nats_JSONField *f)
// {
//     natsMetadata *md = (natsMetadata *)closure;

//     char *name = NATS_STRDUP(fieldName);
//     char *value = NATS_STRDUP(f->value.vstr);
//     if ((name == NULL) || (value == NULL))
//     {
//         NATS_FREE(name);
//         NATS_FREE(value);
//         return nats_setDefaultError(NATS_NO_MEMORY);
//     }

//     md->List[md->Count * 2] = name;
//     md->List[md->Count * 2 + 1] = value;
//     md->Count++;
//     return NATS_OK;
// }

// natsStatus
// nats_unmarshalMetadata(nats_JSON *json, natsPool *pool, const char *fieldName, natsMetadata *md)
// {
//     natsStatus s = NATS_OK;
//     nats_JSON *mdJSON = NULL;
//     int n;

//     md->List = NULL;
//     md->Count = 0;
//     if (json == NULL)
//         return NATS_OK;

//     s = nats_JSONGetObject(json, fieldName, &mdJSON);
//     if ((s != NATS_OK) || (mdJSON == NULL))
//         return NATS_OK;

//     n = natsStrHash_Count(mdJSON->fields);
//     md->List = NATS_CALLOC(n * 2, sizeof(char *));
//     if (md->List == NULL)
//         s = nats_setDefaultError(NATS_NO_MEMORY);
//     IFOK(s, nats_JSONRange(mdJSON, TYPE_STR, 0, _addMD, md));

//     return s;
// }

// natsStatus
// nats_cloneMetadata(natsPool *pool, natsMetadata *clone, natsMetadata md)
// {
//     natsStatus s = NATS_OK;
//     int i = 0;
//     int n;
//     char **list = NULL;

//     clone->Count = 0;
//     clone->List = NULL;
//     if (md.Count == 0)
//         return NATS_OK;

//     n = md.Count * 2;
//     list = natsPool_Alloc(pool, n * sizeof(char *));
//     if (list == NULL)
//         s = nats_setDefaultError(NATS_NO_MEMORY);

//     for (i = 0; (s == NATS_OK) && (i < n); i++)
//     {
//         list[i] = nats_StrdupPool(pool, md.List[i]);
//         if (list[i] == NULL)
//             s = nats_setDefaultError(NATS_NO_MEMORY);
//     }

//     if (s == NATS_OK)
//     {
//         clone->List = (const char **)list;
//         clone->Count = md.Count;
//     }

//     return s;
// }
