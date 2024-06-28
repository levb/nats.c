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
#include "json.h"

int jsonMaxNested = JSON_MAX_NEXTED;

typedef enum
{
    stateStart = 0,
    stateFields,
    stateElements,
    stateColon,
    stateString,
    stateStringEscape,
    stateStringUTF16,
    stateValue,
    stateStringValue,
    stateTrueValue,
    stateFalseValue,
    stateNullValue,
    stateArrayValue,
    stateObjectValue,
    stateNumericValue,
    stateEnd
} state;

struct __natsJSONParser_s
{
    int state;

    // The JSON object (or array) being parsed.
    nats_JSON *json;

    // 1 character can be pushed back and re-processed.
    char undoCh;

    // Toggles whitespace skipping.
    bool skipWhitespace;

    // The current field (or array element) being parsed.
    nats_JSONField *field;

    // Nested level for this parser, and a pointer to the next nested (in
    // chain).
    int nestedLevel;
    natsJSONParser *nested;

    // Used for parsing numbers and fixed strings 'true', 'false', 'null'.
    char scratchBuf[64];
    natsString scratch;

    // Used for parsing strings. nextState is set after parsing a string.
    natsBuf *strBuf;
    int nextState;

    // Toggle allowing a sign, dot, or 'e'/'E' when parsing a number.
    struct
    {
        unsigned numErrorOnSign : 1;
        unsigned numErrorOnDot : 1;
        unsigned numErrorOnE : 1;
    } flags;

    // Position in the JSON string.
    int line;
    int pos;
};

static natsStatus _addValueToArray(natsJSONParser *parser);
static natsStatus _createField(nats_JSONField **newField, natsPool *pool, natsString *name);
static natsStatus _createParser(natsJSONParser **newParser, natsPool *pool, bool isArray, natsJSONParser *from);
static natsStatus _finishBoolValue(natsJSONParser *parser);
static natsStatus _finishNumericValue(natsJSONParser *parser);
static natsStatus _finishNestedValue(natsJSONParser *parser, nats_JSON *obj);
static natsStatus _finishString(natsJSONParser *parser);
static natsStatus _finishValue(natsJSONParser *parser);
static void _startString(natsJSONParser *parser, int nextState);
static void _startValue(natsJSONParser *parser, int state, int typ, uint8_t firstCh);
static natsStatus _decodeUTF16(const char *data, char *val);

static inline natsStatus _addFieldToObject(natsJSONParser *parser)
{
    return natsStrHash_Set(parser->json->fields, &parser->field->name, (void *)parser->field);
}

static inline void _resetScratch(natsJSONParser *parser)
{
    memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
    parser->scratch.text = parser->scratchBuf;
    parser->scratch.len = 0;
}

static inline void _resetString(natsJSONParser *parser)
{
    nats_resetBuf(parser->strBuf);
    parser->nextState = 0;
}

static inline natsStatus _addByteToScratch(natsJSONParser *parser, uint8_t ch)
{
    if (parser->scratch.len >= (sizeof(parser->scratchBuf) - 1))
        return nats_setErrorf(NATS_ERR, "error parsing: insufficient scratch buffer, got '%s'", nats_printableString(&parser->scratch));
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
nats_createJSONParser(natsJSONParser **newParser, natsPool *pool)
{
    return _createParser(newParser, pool, false, NULL);
}

natsStatus
nats_parseJSON(nats_JSON **newJSON, natsJSONParser *parser, const uint8_t *data, const uint8_t *end, size_t *consumed)
{
    nats_JSON *json = NULL;
    natsStatus s = NATS_OK;
    size_t c = 0;
    size_t cNested = 0;
    const uint8_t *remaining = data;

#define _jsonError(_f, ...) \
    nats_setErrorf(NATS_ERR, "JSON parsing error line %d, pos %d: " _f, parser->line + 1, parser->pos, __VA_ARGS__)

    JSONDEBUGf("Parsing JSON: LEN:%zu '%.*s'", end - remaining, (int)(end - remaining), remaining);

    while ((STILL_OK(s)) && (parser->state != stateEnd))
    {
        // Some states don't need to consume a character, process them first.
        switch (parser->state)
        {
        case stateObjectValue:
        case stateArrayValue:
            json = NULL;
            s = nats_parseJSON(&json, parser->nested, remaining, end, &cNested);
            JSONDEBUGf("<>/<> -%d- Finished nested JSON: consumed:%zu, pos:%zu, remaining:%zu, end:%zu", parser->nestedLevel, cNested, c, end - remaining, end - data);
            if (s != NATS_OK)
                continue;
            if (json != NULL)
                _finishNestedValue(parser, json);
            remaining += cNested;
            c += cNested;
        }

        // Get the next character to process.
        char ch = parser->undoCh;
        if (ch == 0)
        {
            // If we have reached the end of the buffer, and there's no "undo" character, we are done.
            if (end - remaining == 0)
            {
                JSONDEBUGf("<>/<> -%d- NO more: pos:%zu, remaining:%zu, end:%zu", parser->nestedLevel, c, end - remaining, end - data);
                if (consumed != NULL)
                    *consumed = c;
                return NATS_OK;
            }
            ch = *remaining;
            JSONDEBUGf("<>/<> -%d- more: '%c' pos:%zu, remaining:%zu, end:%zu", parser->nestedLevel, ch, c, end - remaining, end - data);
            remaining++;
            c++;
            parser->pos++;
        }
        else
        {
            JSONDEBUGf("<>/<> -%d-Undo character: '%c'", parser->nestedLevel, ch);
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
        case stateStart:
            if ((parser->json->array != NULL) && (ch == '['))
            {
                parser->state = stateElements;
            }
            else if ((parser->json->fields != NULL) && (ch == '{'))
            {
                parser->state = stateFields;
            }
            else
            {
                s = _jsonError("invalid character '%c', expected '{' or '[' at the start of JSON", ch);
            }
            continue; // stateStart

        case stateFields:
            switch (ch)
            {
            case '}':
                parser->state = stateEnd;
                parser->skipWhitespace = false; // Do not skip whitespace after the final '}'
                continue;
            case ',':
                // Ignore all commas between fields, nothing to do.
                continue;
            case '"':
                _startString(parser, stateColon);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected start of a named field", ch);
                continue;
            }
            continue; // stateFields

        case stateElements:
            switch (ch)
            {
            case ']':
                parser->state = stateEnd;
                parser->skipWhitespace = false; // Do not skip whitespace after the final '}'
                continue;
            case ',':
                parser->state = stateValue;
                s = _createField(&parser->field, parser->json->pool, &NATS_EMPTY_STRING);
                continue;
            default:
                parser->undoCh = ch;
                parser->state = stateValue;
                s = _createField(&parser->field, parser->json->pool, &NATS_EMPTY_STRING);
                continue;
            }
            continue; // stateElements

        case stateColon:
            switch (ch)
            {
            case ':':
                s = _createField(&parser->field, parser->json->pool, nats_bufAsString(parser->strBuf));
                parser->state = stateValue;
                continue;
            default:
                s = _jsonError("invalid character '%c', expected a ':'", ch);
                continue;
            }
            continue; // stateColon

        case stateValue:
            switch (ch)
            {
            case '"':
                _startString(parser, stateStringValue);
                parser->field->typ = TYPE_STR;
                continue;
            case 'n':
                _startValue(parser, stateNullValue, TYPE_NULL, ch);
                continue;
            case 't':
                _startValue(parser, stateTrueValue, TYPE_BOOL, ch);
                continue;
            case 'f':
                _startValue(parser, stateFalseValue, TYPE_BOOL, ch);
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
                _startValue(parser, stateNumericValue, TYPE_NUM, ch);
                parser->field->numTyp = ((ch == '-') || (ch == '+')) ? TYPE_INT : TYPE_UINT;
                continue;
            case '[':
                parser->state = stateArrayValue;
                // Create a new parser for the nested object. It will consume
                // starting with the next character.
                s = _createArrayParser(&(parser->nested), parser->json->pool, parser);
                continue;
            case '{':
                parser->state = stateObjectValue;
                // Create a new parser for the nested object. It will consume
                // starting with the next character.
                s = _createObjectParser(&(parser->nested), parser->json->pool, parser);
                continue;
            default:
                s = _jsonError("invalid character '%c', expected a start of a value", ch);
                continue;
            }
            continue; // stateValue

        case stateNullValue:
            switch (ch)
            {
            case 'u':
            case 'l':
                s = _addByteToScratch(parser, ch);
                if ((STILL_OK(s)) && (parser->scratch.len == sizeof("null") - 1))
                {
                    if (!unsafe_streq(parser->scratchBuf, "null"))
                    {
                        s = _jsonError("invalid string '%s', expected 'null", nats_printableString(&parser->scratch));
                        continue;
                    }
                    JSONDEBUGf("added field: (null) \"%s\"", nats_printableString(&parser->field->name));
                    s = _finishValue(parser);
                }
                continue;
            default:
                s = _jsonError("invalid character '%c', expected 'null'", ch);
                continue;
            }
            continue; // stateNullValue

        case stateTrueValue:
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
            continue; // stateTrueValue

        case stateFalseValue:
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
            continue; // stateFalseValue

        case stateNumericValue:
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
                    if (parser->flags.numErrorOnSign)
                    {
                        s = _jsonError("error parsing a number: unexpected sign after %s", nats_printableString(&parser->scratch));
                        continue;
                    }

                    parser->flags.numErrorOnSign = true; // only 1 sign allowed
                }
                if (ch == '.')
                {
                    if (parser->flags.numErrorOnDot)
                    {
                        s = _jsonError("error parsing a number: unexpected '.' after %s", nats_printableString(&parser->scratch));
                        continue;
                    }

                    parser->flags.numErrorOnDot = true; // only 1 '.' allowed
                    parser->field->numTyp = TYPE_DOUBLE;
                }
                if (ch == 'e' || ch == 'E')
                {
                    if (parser->flags.numErrorOnE)
                    {
                        s = _jsonError("error parsing a number: unexpected 'e' after %s", nats_printableString(&parser->scratch));
                        continue;
                    }

                    parser->flags.numErrorOnE = true;     // only 1 'e' allowed
                    parser->flags.numErrorOnSign = false; // allow sign in exponent
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
            continue; // stateNumericValue

        case stateString:
            switch (ch)
            {
            case '"':
                // end of string
                s = _finishString(parser);
                continue;
            case '\\':
                parser->state = stateStringEscape;
                continue;
            default:
                s = nats_appendB(parser->strBuf, ch);
                continue;
            }
            continue; // stateString

        case stateStringEscape:
            // Whatever character comes, the next one will not be escaped;
            // except UTF16 handled separately below.
            parser->state = stateString;

            switch (ch)
            {
            case 'b':
                s = nats_appendB(parser->strBuf, '\b');
                continue;
            case 'f':
                s = nats_appendB(parser->strBuf, '\f');
                continue;
            case 'n':
                s = nats_appendB(parser->strBuf, '\n');
                continue;
            case 'r':
                s = nats_appendB(parser->strBuf, '\r');
                continue;
            case 't':
                s = nats_appendB(parser->strBuf, '\t');
                continue;
            case 'u':
                parser->state = stateStringUTF16;
                memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
                parser->scratch.len = 0;
                continue;
            case '"':
            case '\\':
            case '/':
                s = nats_appendB(parser->strBuf, ch);
                continue;
            default:
                s = _jsonError("error parsing string '%s': invalid control character", nats_printableString(nats_bufAsString(parser->strBuf)));
                continue;
            }
            continue; // stateStringEscape

        case stateStringUTF16:
            if (parser->scratch.len < sizeof("ABCD")) // hex number
            {
                _addByteToScratch(parser, ch);
                if (parser->scratch.len == sizeof("ABCD"))
                {
                    char val = 0;
                    s = _decodeUTF16(parser->scratchBuf, &val);
                    if (s != NATS_OK)
                    {
                        s = _jsonError("error parsing string '%s': invalid unicode character", nats_printableString(&parser->scratch));
                        continue;
                    }
                    s = nats_appendB(parser->strBuf, val);
                    parser->state = stateString;
                    memset(parser->scratchBuf, 0, sizeof(parser->scratchBuf));
                    parser->scratch.len = 0;
                }
            }
            continue; // stateStringUTF16

        default:
            s = _jsonError("invalid state %d", parser->state);
            break;
        }
    }

    if ((STILL_OK(s)) && (parser->state == stateEnd))
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
        return nats_setErrorf(NATS_ERR, "json reached maximum nested objects of %d", jsonMaxNested);

    IFOK(s, CHECK_NO_MEMORY(parser = nats_palloc(pool, sizeof(natsJSONParser))));
    IFOK(s, CHECK_NO_MEMORY(json = nats_palloc(pool, sizeof(nats_JSON))));
    if (isArray)
    {
        IFOK(s, CHECK_NO_MEMORY(json->array = nats_palloc(pool, sizeof(nats_JSONArray))));
    }
    else
    {
        IFOK(s, natsStrHash_Create(&(json->fields), pool, 4));
    }
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    json->pool = pool;
    parser->json = json;
    parser->state = (nestedLevel == 0 ? stateStart : (isArray ? stateElements : stateFields));
    parser->skipWhitespace = true;
    parser->nestedLevel = nestedLevel;

    if (nestedLevel)
    {
        parser->nestedLevel = nestedLevel;

        parser->undoCh = from->undoCh;
        parser->line = from->line;
        parser->pos = from->pos;
        parser->strBuf = from->strBuf;
        nats_resetBuf(parser->strBuf);
    }
    else
    {
        IFOK(s, nats_getGrowableBuf(&parser->strBuf, pool, 0));
        if (s != NATS_OK)
            return NATS_UPDATE_ERR_STACK(s);
    }

    *newParser = parser;
    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_createField(nats_JSONField **newField, natsPool *pool, natsString *name)
{
    natsStatus s = NATS_OK;
    nats_JSONField *field = NULL;

    field = nats_palloc(pool, sizeof(nats_JSONField));
    if (field == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    s = nats_pdupString(&field->name, pool, name);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);
    field->typ = TYPE_NOT_SET;
    *newField = field;

    return NATS_OK;
}

static natsStatus
_decodeUTF16(const char *data, char *val)
{
    int res = 0;
    int j;

    if (safe_strlen((const char *)data) < 4)
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
    parser->state = stateString;
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

    parser->flags.numErrorOnSign = false;
    parser->flags.numErrorOnDot = false;
    parser->flags.numErrorOnE = false;
}

static natsStatus _finishString(natsJSONParser *parser)
{
    natsStatus s = NATS_OK;
    // TODO: <>/<> Not clean to do this here, but will suffice for now
    switch (parser->nextState)
    {
    case stateStringValue:
        s = nats_pdupString(&parser->field->value.vstr, parser->json->pool, nats_bufAsString(parser->strBuf));
        if (!STILL_OK(s))
            return nats_setDefaultError(s);

        JSONDEBUGf("added field: (string) \"%s\":\"%s\"", nats_printableString(&parser->field->name), nats_printableString(&parser->field->value.vstr));
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
    parser->state = isArray ? stateElements : stateFields;
    parser->skipWhitespace = true;
    return NATS_OK;
}

static natsStatus _finishBoolValue(natsJSONParser *parser)
{
    parser->field->typ = TYPE_BOOL;
    if (unsafe_streq(parser->scratchBuf, "true"))
        parser->field->value.vbool = true;
    else if (unsafe_streq(parser->scratchBuf, "false"))
        parser->field->value.vbool = false;
    else
        return nats_setErrorf(NATS_ERR, "error parsing boolean '%s'", nats_printableString(&parser->scratch));

    JSONDEBUGf("added field: (bool) \"%s\":%s", nats_printableString(&parser->field->name), parser->scratchBuf);
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
        JSONDEBUGf("added field: (int) \"%s\":%lld", nats_printableString(&parser->field->name), parser->field->value.vint);
        break;
    case TYPE_UINT:
        parser->field->value.vuint = strtoull((const char *)parser->scratchBuf, NULL, 10);
        JSONDEBUGf("added field: (uint) \"%s\":%lld", nats_printableString(&parser->field->name), parser->field->value.vuint);
        break;
    case TYPE_DOUBLE:
        parser->field->value.vdec = strtold((const char *)parser->scratchBuf, NULL);
        JSONDEBUGf("added field: (double) \"%s\":%Lf", nats_printableString(&parser->field->name), parser->field->value.vdec);
        break;
    }
    return _finishValue(parser);
}

static natsStatus _finishNestedValue(natsJSONParser *parser, nats_JSON *obj)
{
    switch (parser->state)
    {
    case stateArrayValue:
        if (obj->array == NULL)
            return nats_setError(NATS_ERR, "unexpected error parsing array");
        if (obj->array->typ == TYPE_NOT_SET)
            obj->array->typ = TYPE_NULL;
        parser->field->typ = TYPE_ARRAY;
        parser->field->value.varr = obj->array;
        JSONDEBUGf("added array value: %d elements, type %d", obj->array->size, obj->array->typ);
        break;
    case stateObjectValue:
        if (obj->fields == NULL)
            return nats_setError(NATS_ERR, "unexpected error parsing object");
        parser->field->typ = TYPE_OBJECT;
        parser->field->value.vobj = obj;
        JSONDEBUGf("added object value: %d fields", natsStrHash_Count(obj->fields));
        break;
    default:
        return nats_setErrorf(NATS_ERR, "unexpected error parsing nested object '%.s'", nats_printableString(&parser->field->name));
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
        return nats_setErrorf(NATS_ERR, "array content of different types '%s'", nats_printableString(&field->name));

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
        newValues = nats_palloc(parser->json->pool, newCap * a->eltSize);
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
        ((natsString **)(a->values))[a->size++] = &field->value.vstr;
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
