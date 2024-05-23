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
#define JSON_MAX_NUM_SIZE   ((int) sizeof(long double))

// Forward declarations due to recursive calls
static natsStatus _jsonParse(nats_JSON **newJSON, natsPool *Pool, int *parsedLen, const char *jsonStr, int jsonLen, int nested);
static natsStatus _jsonParseValue(char **str, natsPool *pool, nats_JSONField *field, int nested);

#define JSON_GET_AS(jt, t) \
natsStatus      s      = NATS_OK;                       \
nats_JSONField  *field = NULL;                          \
s = nats_JSONGetField(json, fieldName, (jt), &field);   \
if ((s == NATS_OK) && (field == NULL))                  \
{                                                       \
    *value = 0;                                         \
    return NATS_OK;                                     \
}                                                       \
else if (s == NATS_OK)                                  \
{                                                       \
    switch (field->numTyp)                              \
    {                                                   \
        case TYPE_INT:                                  \
            *value = (t)field->value.vint;  break;      \
        case TYPE_UINT:                                 \
            *value = (t)field->value.vuint; break;      \
        default:                                        \
            *value = (t)field->value.vdec;              \
    }                                                   \
}                                                       \
return NATS_UPDATE_ERR_STACK(s);

#define JSON_ARRAY_AS(_p, _t) \
int i;                                              \
_t* values = (_t*) natsPool_Alloc((_p), arr->size * sizeof(_t)); \
if (values == NULL)                                 \
    return nats_setDefaultError(NATS_NO_MEMORY);    \
for (i=0; i<arr->size; i++)                         \
    values[i] = ((_t*) arr->values)[i];              \
*array     = values;                                \
*arraySize = arr->size;                             \
return NATS_OK;

#define JSON_ARRAY_AS_NUM(_p, _t) \
int i;                                                          \
_t* values = (_t*) natsPool_Alloc((_p), arr->size * sizeof(_t));             \
if (values == NULL)                                             \
    return nats_setDefaultError(NATS_NO_MEMORY);                \
for (i=0; i<arr->size; i++)                                     \
{                                                               \
    void *ptr = NULL;                                           \
    ptr = (void*) ((char*)(arr->values)+(i*JSON_MAX_NUM_SIZE)); \
    values[i] = *(_t*) ptr;                                      \
}                                                               \
*array     = values;                                            \
*arraySize = arr->size;                                         \
return NATS_OK;

#define JSON_GET_ARRAY(_t, _f)                                      \
    natsStatus s = NATS_OK;                                       \
    nats_JSONField *field = NULL;                                 \
    s = nats_JSONGetArrayField(json, fieldName, (_t), &field);     \
    if ((s == NATS_OK) && (field == NULL))                        \
    {                                                             \
        *array = NULL;                                            \
        *arraySize = 0;                                           \
        return NATS_OK;                                           \
    }                                                             \
    else if (s == NATS_OK)                                        \
        s = (_f)(json->pool, field->value.varr, array, arraySize); \
    return NATS_UPDATE_ERR_STACK(s);

static natsStatus
_jsonCreateField(nats_JSONField **newField, natsPool *pool, char *fieldName)
{
    nats_JSONField *field = NULL;

    field = (nats_JSONField*) natsPool_Alloc(pool, sizeof(nats_JSONField));
    if (field == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    field->name = fieldName;
    field->typ  = TYPE_NOT_SET;

    *newField = field;

    return NATS_OK;
}

static char*
_jsonTrimSpace(char *ptr)
{
    while ((*ptr != '\0')
            && ((*ptr == ' ') || (*ptr == '\t') || (*ptr == '\r') || (*ptr == '\n')))
    {
        ptr += 1;
    }
    return ptr;
}

static natsStatus
_decodeUni(char **iPtr, char *val)
{
    int     res = 0;
    char    *i  = *iPtr;
    int     j;

    if (strlen(i) < 5)
        return NATS_ERR;

    i++;
    for (j=0; j<4; j++)
    {
        char c = i[j];
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
    *val   = (char) res;
    *iPtr += 5;

    return NATS_OK;
}

static natsStatus
_jsonGetStr(char **ptr, char **value)
{
    char *p = *ptr;
    char *o = *ptr;

    while ((*p != '\0') && (*p != '"'))
    {
        if (*p != '\\')
        {
            if (o != p)
                *o = *p;
            o++;
            p++;
            continue;
        }
        p++;
        // Escaped character here...
        if (*p == '\0')
        {
            *o = '\0';
            return nats_setError(NATS_ERR,
                                 "error parsing string '%s': invalid control character at the end",
                                 o);
        }
        // based on what http://www.json.org/ says a string should be
        switch (*p)
        {
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'n': *o++ = '\n'; break;
            case 'r': *o++ = '\r'; break;
            case 't': *o++ = '\t'; break;
            case '"':
            case '\\':
            case '/':
                *o++ = *p;
                break;
            case 'u':
            {
                char        val = 0;
                natsStatus  s   = _decodeUni(&p, &val);
                if (s != NATS_OK)
                {
                    return nats_setError(NATS_ERR,
                                         "error parsing string '%s': invalid unicode character",
                                         p);
                }
                *o++ = val;
                p--;
                break;
            }
            default:
                return nats_setError(NATS_ERR,
                                     "error parsing string '%s': invalid control character",
                                     p);
        }
        p++;
    }

    if (*p != '\0')
    {
        *o = '\0';
        *value = *ptr;
        *ptr = (char*) (p + 1);
        return NATS_OK;
    }
    return nats_setError(NATS_ERR,
                         "error parsing string '%s': unexpected end of JSON input",
                         *ptr);
}

static natsStatus
_jsonGetNum(char **ptr, nats_JSONField *field)
{
    char        *p             = *ptr;
    bool        expIsNegative  = false;
    uint64_t    uintVal        = 0;
    uint64_t    decVal         = 0;
    uint64_t    decPower       = 1;
    long double sign           = 1.0;
    long double ePower         = 1.0;
    int         decPCount      = 0;
    int         numTyp         = 0;

    while (isspace((unsigned char) *p))
        p++;

    sign = (*p == '-' ? -1.0 : 1.0);

    if ((*p == '-') || (*p == '+'))
        p++;

    while (isdigit((unsigned char) *p))
        uintVal = uintVal * 10 + (*p++ - '0');

    if (*p == '.')
    {
        p++;
        numTyp = TYPE_DOUBLE;
    }

    while (isdigit((unsigned char) *p))
    {
        decVal = decVal * 10 + (*p++ - '0');
        decPower *= 10;
        decPCount++;
    }

    if ((*p == 'e') || (*p == 'E'))
    {
        int64_t eVal = 0;

        numTyp = TYPE_DOUBLE;

        p++;

        expIsNegative = (*p == '-' ? true : false);

        if ((*p == '-') || (*p == '+'))
            p++;

        while (isdigit((unsigned char) *p))
            eVal = eVal * 10 + (*p++ - '0');

        if (expIsNegative)
        {
            if (decPower > 0)
                ePower = (long double) decPower;
        }
        else
        {
            if (decPCount > eVal)
            {
                eVal = decPCount - eVal;
                expIsNegative = true;
            }
            else
            {
                eVal -= decPCount;
            }
        }
        while (eVal != 0)
        {
            ePower *= 10;
            eVal--;
        }
    }

    // If we don't end with a ' ', ',', ']', or '}', this is syntax error.
    if ((*p != ' ') && (*p != ',') && (*p != '}') && (*p != ']'))
        return nats_setError(NATS_ERR,
                             "error parsing number '%s': missing separator or unexpected end of JSON input",
                             *ptr);

    if (numTyp == TYPE_DOUBLE)
    {
        long double res = 0.0;

        if (decVal > 0)
            res = sign * (long double) (uintVal * decPower + decVal);
        else
            res = sign * (long double) uintVal;

        if (ePower > 1)
        {
            if (expIsNegative)
                res /= ePower;
            else
                res *= ePower;
        }
        else if (decVal > 0)
        {
            res /= decPower;
        }
        field->value.vdec = res;
    }
    else if (sign < 0)
    {
        numTyp = TYPE_INT;
        field->value.vint = -((int64_t) uintVal);
    }
    else
    {
        numTyp = TYPE_UINT;
        field->value.vuint = uintVal;
    }
    *ptr = p;
    field->numTyp = numTyp;
    return NATS_OK;
}

static natsStatus
_jsonGetBool(char **ptr, bool *val)
{
    if (strncmp(*ptr, "true", 4) == 0)
    {
        *val = true;
        *ptr += 4;
        return NATS_OK;
    }
    else if (strncmp(*ptr, "false", 5) == 0)
    {
        *val = false;
        *ptr += 5;
        return NATS_OK;
    }
    return nats_setError(NATS_ERR,
                         "error parsing boolean, got: '%s'", *ptr);
}

static natsStatus
_jsonGetArray(char **ptr, natsPool *pool, nats_JSONArray **newArray, int nested)
{
    natsStatus      s       = NATS_OK;
    char            *p      = *ptr;
    bool            end     = false;
    int             typ     = TYPE_NOT_SET;
    nats_JSONField  field;
    nats_JSONArray  array;

    if (nested >= jsonMaxNested)
        return nats_setError(NATS_ERR, "json reached maximum nested arrays of %d", jsonMaxNested);

    // Initialize our stack variable
    memset(&array, 0, sizeof(nats_JSONArray));

    while ((s == NATS_OK) && (*p != '\0'))
    {
        p = _jsonTrimSpace(p);

        if ((typ == TYPE_NOT_SET) && (*p == ']'))
        {
            array.typ = TYPE_NULL;
            end = true;
            break;
        }

        // Initialize the field before parsing.
        memset(&field, 0, sizeof(nats_JSONField));

        s = _jsonParseValue(&p, pool, &field, nested);
        if (s == NATS_OK)
        {
            if (typ == TYPE_NOT_SET)
            {
                typ       = field.typ;
                array.typ = field.typ;

                // Set the element size based on type.
                switch (typ)
                {
                    case TYPE_STR:      array.eltSize = sizeof(char*);              break;
                    case TYPE_BOOL:     array.eltSize = sizeof(bool);               break;
                    case TYPE_NUM:      array.eltSize = JSON_MAX_NUM_SIZE;          break;
                    case TYPE_OBJECT:   array.eltSize = sizeof(nats_JSON*);         break;
                    case TYPE_ARRAY:    array.eltSize = sizeof(nats_JSONArray*);    break;
                    default:
                        s = nats_setError(NATS_ERR,
                                          "array of type %d not supported", typ);
                }
            }
            else if (typ != field.typ)
            {
                s = nats_setError(NATS_ERR,
                                  "array content of different types '%s'",
                                  *ptr);
            }
        }
        if (s != NATS_OK)
            break;

        if (array.size + 1 > array.cap)
        {
            char **newValues  = NULL;
            int newCap      = 2 * array.cap;

            if (newCap == 0)
                newCap = 4;

            newValues = (char**) NATS_REALLOC(array.values, newCap * array.eltSize);
            if (newValues == NULL)
            {
                s = nats_setDefaultError(NATS_NO_MEMORY);
                break;
            }
            array.values = (void**) newValues;
            array.cap    = newCap;
        }
        // Set value based on type
        switch (typ)
        {
            case TYPE_STR:
                ((char**)array.values)[array.size++] = field.value.vstr;
                break;
            case TYPE_BOOL:
                ((bool*)array.values)[array.size++] = field.value.vbool;
                 break;
            case TYPE_NUM:
            {
                void    *numPtr = NULL;
                size_t  sz   = 0;

                switch (field.numTyp)
                {
                    case TYPE_INT:
                        numPtr = &(field.value.vint);
                        sz     = sizeof(int64_t);
                        break;
                    case TYPE_UINT:
                        numPtr = &(field.value.vuint);
                        sz     = sizeof(uint64_t);
                        break;
                    default:
                        numPtr = &(field.value.vdec);
                        sz     = sizeof(long double);
                }
                memcpy((void*)(((char *)array.values)+(array.size*array.eltSize)), numPtr, sz);
                array.size++;
                break;
            }
            case TYPE_OBJECT:
                ((nats_JSON**)array.values)[array.size++] = field.value.vobj;
                break;
            case TYPE_ARRAY:
                ((nats_JSONArray**)array.values)[array.size++] = field.value.varr;
                break;
        }

        p = _jsonTrimSpace(p);
        if (*p == '\0')
            break;

        if (*p == ']')
        {
            end = true;
            break;
        }
        else if (*p == ',')
        {
            p += 1;
        }
        else
        {
            s = nats_setError(NATS_ERR, "expected ',' got '%s'", p);
        }
    }
    if ((s == NATS_OK) && !end)
    {
        s = nats_setError(NATS_ERR,
                          "unexpected end of array: '%s'",
                          (*p != '\0' ? p : "NULL"));
    }
    if (s == NATS_OK)
    {
        *newArray = NATS_MALLOC(sizeof(nats_JSONArray));
        if (*newArray == NULL)
        {
            s = nats_setDefaultError(NATS_NO_MEMORY);
        }
        else
        {
            memcpy(*newArray, &array, sizeof(nats_JSONArray));
            *ptr = (char*) (p + 1);
        }
    }

    return NATS_UPDATE_ERR_STACK(s);
}

#define JSON_STATE_START        (0)
#define JSON_STATE_NO_FIELD_YET (1)
#define JSON_STATE_FIELD        (2)
#define JSON_STATE_SEPARATOR    (3)
#define JSON_STATE_VALUE        (4)
#define JSON_STATE_NEXT_FIELD   (5)
#define JSON_STATE_END          (6)

static natsStatus
_jsonParseValue(char **str, natsPool *pool, nats_JSONField *field, int nested)
{
    natsStatus  s    = NATS_OK;
    char        *ptr = *str;

    // Parsing value here. Determine the type based on first character.
    if (*ptr == '"')
    {
        ptr += 1;
        field->typ = TYPE_STR;
        s = _jsonGetStr(&ptr, &field->value.vstr);
    }
    else if ((*ptr == 't') || (*ptr == 'f'))
    {
        field->typ = TYPE_BOOL;
        s = _jsonGetBool(&ptr, &field->value.vbool);
    }
    else if (isdigit((unsigned char) *ptr) || (*ptr == '-'))
    {
        field->typ = TYPE_NUM;
        s = _jsonGetNum(&ptr, field);
    }
    else if (*ptr == '[')
    {
        ptr += 1;
        field->typ = TYPE_ARRAY;
        s = _jsonGetArray(&ptr, pool, &field->value.varr, nested+1);
    }
    else if (*ptr == '{')
    {
        nats_JSON   *object = NULL;
        int         objLen  = 0;

        ptr += 1;
        field->typ = TYPE_OBJECT;
        s = _jsonParse(&object, pool, &objLen, ptr, -1, nested+1);
        if (s == NATS_OK)
        {
            field->value.vobj = object;
            ptr += objLen;
        }
    }
    else if ((*ptr == 'n') && (strstr(ptr, "null") == ptr))
    {
        ptr += 4;
        field->typ = TYPE_NULL;
    }
    else
    {
        s = nats_setError(NATS_ERR,
                            "looking for value, got: '%s'", ptr);
    }
    if (s == NATS_OK)
        *str = ptr;

    return NATS_UPDATE_ERR_STACK(s);
}

static natsStatus
_jsonParse(nats_JSON **newJSON, natsPool *pool, int *parsedLen, const char *jsonStr, int jsonLen, int nested)
{
    natsStatus      s         = NATS_OK;
    nats_JSON       *json     = NULL;
    nats_JSONField  *field    = NULL;
    void            *oldField = NULL;
    char            *ptr;
    char            *fieldName = NULL;
    int             state;
    char            *copyStr  = NULL;
    bool            breakLoop = false;

    if (parsedLen != NULL)
        *parsedLen = 0;

    if (pool == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    if (nested >= jsonMaxNested)
        return nats_setError(NATS_ERR, "json reached maximum nested objects of %d", jsonMaxNested);

    if (jsonLen < 0)
    {
        if (jsonStr == NULL)
            return nats_setDefaultError(NATS_INVALID_ARG);

        jsonLen = (int) strlen(jsonStr);
    }

    json = natsPool_Alloc(pool, sizeof(nats_JSON));
    if (json == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    json->pool = pool;

    s = natsStrHash_Create(&(json->fields), json->pool, 4);
    if (s == NATS_OK)
    {
        json->str = NATS_MALLOC(jsonLen + 1);
        if (json->str == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);

        if (s == NATS_OK)
        {
            memcpy(json->str, jsonStr, jsonLen);
            json->str[jsonLen] = '\0';
        }
    }
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    ptr = json->str;
    copyStr = NATS_STRDUP(ptr);
    if (copyStr == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    state = (nested == 0 ? JSON_STATE_START : JSON_STATE_NO_FIELD_YET);

    while ((s == NATS_OK) && (*ptr != '\0') && !breakLoop)
    {
        ptr = _jsonTrimSpace(ptr);
        if (*ptr == '\0')
            break;
        switch (state)
        {
            case JSON_STATE_START:
            {
                // Should be the start of the JSON string
                if (*ptr != '{')
                {
                    s = nats_setError(NATS_ERR, "incorrect JSON string: '%s'", ptr);
                    break;
                }
                ptr += 1;
                state = JSON_STATE_NO_FIELD_YET;
                break;
            }
            case JSON_STATE_NO_FIELD_YET:
            case JSON_STATE_FIELD:
            {
                // Check for end, which is valid only in state == JSON_STATE_NO_FIELD_YET
                if (*ptr == '}')
                {
                    if (state == JSON_STATE_NO_FIELD_YET)
                    {
                        ptr += 1;
                        state = JSON_STATE_END;
                        break;
                    }
                    s = nats_setError(NATS_ERR,
                                      "expected beginning of field, got: '%s'",
                                      ptr);
                    break;
                }
                // Check for
                // Should be the first quote of a field name
                if (*ptr != '"')
                {
                    s = nats_setError(NATS_ERR, "missing quote: '%s'", ptr);
                    break;
                }
                ptr += 1;
                s = _jsonGetStr(&ptr, &fieldName);
                if (s != NATS_OK)
                    break;
                s = _jsonCreateField(&field, pool, fieldName);
                if (s != NATS_OK)
                {
                    NATS_UPDATE_ERR_STACK(s);
                    break;
                }
                s = natsStrHash_Set(json->fields, fieldName, false, (void*) field, &oldField);
                if (s != NATS_OK)
                {
                    NATS_UPDATE_ERR_STACK(s);
                    break;
                }
                if (oldField != NULL)
                {
                    NATS_FREE(oldField);
                    oldField = NULL;
                }
                state = JSON_STATE_SEPARATOR;
                break;
            }
            case JSON_STATE_SEPARATOR:
            {
                // Should be the separation between field name and value.
                if (*ptr != ':')
                {
                    s = nats_setError(NATS_ERR, "missing value for field '%s': '%s'", fieldName, ptr);
                    break;
                }
                ptr += 1;
                state = JSON_STATE_VALUE;
                break;
            }
            case JSON_STATE_VALUE:
            {
                s = _jsonParseValue(&ptr, pool, field, nested);
                if (s == NATS_OK)
                    state = JSON_STATE_NEXT_FIELD;
                break;
            }
            case JSON_STATE_NEXT_FIELD:
            {
                // We should have a ',' separator or be at the end of the string
                if ((*ptr != ',') && (*ptr != '}'))
                {
                    s =  nats_setError(NATS_ERR, "missing separator: '%s' (%s)", ptr, copyStr);
                    break;
                }
                if (*ptr == ',')
                    state = JSON_STATE_FIELD;
                else
                    state = JSON_STATE_END;
                ptr += 1;
                break;
            }
            case JSON_STATE_END:
            {
                if (nested > 0)
                {
                    breakLoop = true;
                    break;
                }
                // If we are here it means that there was a character after the '}'
                // so that's considered a failure.
                s = nats_setError(NATS_ERR,
                                  "invalid characters after end of JSON: '%s'",
                                  ptr);
                break;
            }
        }
    }
    if (s == NATS_OK)
    {
        if (state != JSON_STATE_END)
            s = nats_setError(NATS_ERR, "%s", "JSON string not properly closed");
    }
    if (s == NATS_OK)
    {
        if (parsedLen != NULL)
            *parsedLen = (int) (ptr - json->str);
        *newJSON = json;
    }
    else
        nats_JSONDestroy(json);

    NATS_FREE(copyStr);

    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONParse(nats_JSON **newJSON, natsPool *pool, const char *jsonStr, int jsonLen)
{
    natsStatus s = _jsonParse(newJSON, pool, NULL, jsonStr, jsonLen, 0);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetField(nats_JSON *json, const char *fieldName, int fieldType, nats_JSONField **retField)
{
    nats_JSONField *field = NULL;

    field = (nats_JSONField*) natsStrHash_Get(json->fields, (char*) fieldName);
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
nats_JSONGetStr(nats_JSON *json, const char *fieldName, char **value)
{
    natsStatus      s      = NATS_OK;
    nats_JSONField  *field = NULL;

    s = nats_JSONGetField(json, fieldName, TYPE_STR, &field);
    if (s == NATS_OK)
    {
        if ((field == NULL) || (field->value.vstr == NULL))
        {
            *value = NULL;
            return NATS_OK;
        }
        else
        {
            char *tmp = NATS_STRDUP(field->value.vstr);
            if (tmp == NULL)
                return nats_setDefaultError(NATS_NO_MEMORY);
            *value = tmp;
        }
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetStrPtr(nats_JSON *json, const char *fieldName, const char **str)
{
    natsStatus      s;
    nats_JSONField  *field = NULL;

    s = nats_JSONGetField(json, fieldName, TYPE_STR, &field);
    if (s == NATS_OK)
    {
        if (field == NULL)
            *str = NULL;
        else
            *str = field->value.vstr;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetBytes(nats_JSON *json, const char *fieldName, unsigned char **value, int *len)
{
    natsStatus      s;
    const char      *str = NULL;

    *value = NULL;
    *len   = 0;

    s = nats_JSONGetStrPtr(json, fieldName, &str);
    if ((s == NATS_OK) && (str != NULL))
        s = nats_Base64_Decode(str, value, len);
    return NATS_UPDATE_ERR_STACK(s);
}

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
    natsStatus      s      = NATS_OK;
    nats_JSONField  *field = NULL;

    s = nats_JSONGetField(json, fieldName, TYPE_BOOL, &field);
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
    natsStatus      s      = NATS_OK;
    nats_JSONField  *field = NULL;

    s = nats_JSONGetField(json, fieldName, TYPE_OBJECT, &field);
    if (s == NATS_OK)
    {
        *value = (field == NULL ? NULL : field->value.vobj);
        return NATS_OK;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetTime(nats_JSON *json, const char *fieldName, int64_t *timeUTC)
{
    natsStatus  s           = NATS_OK;
    char        *str        = NULL;

    s = nats_JSONGetStr(json, fieldName, &str);
    if ((s == NATS_OK) && (str == NULL))
    {
        *timeUTC = 0;
        return NATS_OK;
    }
    else if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    s = nats_parseTime(str, timeUTC);
    NATS_FREE(str);
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetArrayField(nats_JSON *json, const char *fieldName, int fieldType, nats_JSONField **retField)
{
    nats_JSONField  *field   = NULL;

    field = (nats_JSONField*) natsStrHash_Get(json->fields, (char*) fieldName);
    if ((field == NULL) || (field->typ == TYPE_NULL))
    {
        *retField = NULL;
        return NATS_OK;
    }

    // Check parsed type matches what is being asked.
    if (field->typ != TYPE_ARRAY)
        return nats_setError(NATS_INVALID_ARG,
                             "Field '%s' is not an array, it has type: %d",
                             field->name, field->typ);
    // If empty array, return NULL/OK
    if (field->value.varr->typ == TYPE_NULL)
    {
        *retField = NULL;
        return NATS_OK;
    }
    if (fieldType != field->value.varr->typ)
        return nats_setError(NATS_INVALID_ARG,
                             "Asked for field '%s' as an array of type: %d, but it is an array of type: %d",
                             field->name, fieldType, field->typ);

    *retField = field;
    return NATS_OK;
}

static natsStatus
_jsonArrayAsStrings(natsPool *pool, nats_JSONArray *arr, char ***array, int *arraySize)
{
    natsStatus  s = NATS_OK;
    int         i;

    char **values = natsPool_Alloc(pool, arr->size * arr->eltSize);
    if (values == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    for (i=0; i<arr->size; i++)
    {
        values[i] = NATS_STRDUP((char*)(arr->values[i]));
        if (values[i] == NULL)
        {
            s = nats_setDefaultError(NATS_NO_MEMORY);
            break;
        }
    }
    if (s != NATS_OK)
    {
        int j;

        for (j=0; j<i; j++)
            NATS_FREE(values[i]);

        NATS_FREE(values);
    }
    else
    {
        *array     = values;
        *arraySize = arr->size;
    }
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_JSONGetArrayStr(nats_JSON *json, const char *fieldName, char ***array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_STR, _jsonArrayAsStrings);
}

static natsStatus
_jsonArrayAsBools(natsPool *pool, nats_JSONArray *arr, bool **array, int *arraySize)
{
    JSON_ARRAY_AS(pool, bool);
}

natsStatus
nats_JSONGetArrayBool(nats_JSON *json, const char *fieldName, bool **array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_BOOL, _jsonArrayAsBools);
}

static natsStatus
_jsonArrayAsDoubles(natsPool *pool, nats_JSONArray *arr, long double **array, int *arraySize)
{
    JSON_ARRAY_AS_NUM(pool, long double);
}

natsStatus
nats_JSONGetArrayDouble(nats_JSON *json, const char *fieldName, long double **array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsDoubles);
}

static natsStatus
_jsonArrayAsInts(natsPool *pool, nats_JSONArray *arr, int **array, int *arraySize)
{
    JSON_ARRAY_AS_NUM(pool, int);
}

natsStatus
nats_JSONGetArrayInt(nats_JSON *json, const char *fieldName, int **array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsInts);
}

static natsStatus
_jsonArrayAsLongs(natsPool *pool, nats_JSONArray *arr, int64_t **array, int *arraySize)
{
    JSON_ARRAY_AS_NUM(pool, int64_t);
}

natsStatus
nats_JSONGetArrayLong(nats_JSON *json, const char *fieldName, int64_t **array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsLongs);
}

static natsStatus
_jsonArrayAsULongs(natsPool *pool, nats_JSONArray *arr, uint64_t **array, int *arraySize)
{
    JSON_ARRAY_AS_NUM(pool, uint64_t);
}

natsStatus
nats_JSONGetArrayULong(nats_JSON *json, const char *fieldName, uint64_t **array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_NUM, _jsonArrayAsULongs);
}

static natsStatus
_jsonArrayAsObjects(natsPool *pool, nats_JSONArray *arr, nats_JSON ***array, int *arraySize)
{
    JSON_ARRAY_AS(pool, nats_JSON*);
}

natsStatus
nats_JSONGetArrayObject(nats_JSON *json, const char *fieldName, nats_JSON ***array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_OBJECT, _jsonArrayAsObjects);
}

static natsStatus
_jsonArrayAsArrays(natsPool *pool, nats_JSONArray *arr, nats_JSONArray ***array, int *arraySize)
{
    JSON_ARRAY_AS(pool, nats_JSONArray*);
}

natsStatus
nats_JSONGetArrayArray(nats_JSON *json, const char *fieldName, nats_JSONArray ***array, int *arraySize)
{
    JSON_GET_ARRAY(TYPE_ARRAY, _jsonArrayAsArrays);
}

natsStatus
nats_JSONRange(nats_JSON *json, int expectedType, int expectedNumType, jsonRangeCB cb, void *userInfo)
{
    natsStrHashIter iter;
    char            *fname  = NULL;
    void            *val    = NULL;
    natsStatus      s       = NATS_OK;

    natsStrHashIter_Init(&iter, json->fields);
    while ((s == NATS_OK) && natsStrHashIter_Next(&iter, &fname, &val))
    {
        nats_JSONField *f = (nats_JSONField*) val;

        if (f->typ != expectedType)
            s = nats_setError(NATS_ERR, "field '%s': expected value type of %d, got %d",
                              f->name, expectedType, f->typ);
        else if ((f->typ == TYPE_NUM) && (f->numTyp != expectedNumType))
            s = nats_setError(NATS_ERR, "field '%s': expected numeric type of %d, got %d",
                              f->name, expectedNumType, f->numTyp);
        else
            s = cb(userInfo, (const char*) f->name, f);
    }
    natsStrHashIter_Done(&iter);
    return NATS_UPDATE_ERR_STACK(s);
}

void
nats_JSONDestroy(nats_JSON *json)
{
}

natsStatus
nats_EncodeTimeUTC(char *buf, size_t bufLen, int64_t timeUTC)
{
    int64_t     t  = timeUTC / (int64_t) 1E9;
    int64_t     ns = timeUTC - ((int64_t) t * (int64_t) 1E9);
    struct tm   tp;
    int         n;

    // We will encode at most: "YYYY:MM:DDTHH:MM:SS.123456789+12:34"
    // so we need at least 35+1 characters.
    if (bufLen < 36)
        return nats_setError(NATS_INVALID_ARG,
                             "buffer to encode UTC time is too small (%d), needs 36",
                             (int) bufLen);

    if (timeUTC == 0)
    {
        snprintf(buf, bufLen, "%s", "0001-01-01T00:00:00Z");
        return NATS_OK;
    }

    memset(&tp, 0, sizeof(struct tm));
#ifdef _WIN32
    _gmtime64_s(&tp, (const __time64_t*) &t);
#else
    gmtime_r((const time_t*) &t, &tp);
#endif
    n = (int) strftime(buf, bufLen, "%FT%T", &tp);
    if (n == 0)
        return nats_setDefaultError(NATS_ERR);

    if (ns > 0)
    {
        char nsBuf[15];
        int i, nd;

        nd = snprintf(nsBuf, sizeof(nsBuf), ".%" PRId64, ns);
        for (; (nd > 0) && (nsBuf[nd-1] == '0'); )
            nd--;

        for (i=0; i<nd; i++)
            *(buf+n++) = nsBuf[i];
    }
    *(buf+n) = 'Z';
    *(buf+n+1) = '\0';

    return NATS_OK;
}

static natsStatus
_marshalLongVal(natsBuffer *buf, bool comma, const char *fieldName, bool l, int64_t lval, uint64_t uval)
{
    natsStatus s = NATS_OK;
    char       temp[32];
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
    uint64_t u = (uint64_t) (neg ? -d : d);
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
    IFOK(s, natsBuf_Append(out_buf, buf + w, sizeof(buf) - w));
    IFOK(s, natsBuf_AppendString(out_buf, "\""));
    return NATS_UPDATE_ERR_STACK(s);
}

natsStatus
nats_marshalMetadata(natsBuffer *buf, bool comma, const char *fieldName, natsMetadata md)
{
    natsStatus s = NATS_OK;
    int i;
    const char *start = (comma ? ",\"" : "\"");

    if (md.Count <= 0)
        return NATS_OK;

    IFOK(s, natsBuf_AppendString(buf, start));
    IFOK(s, natsBuf_AppendString(buf, fieldName));
    IFOK(s, natsBuf_Append(buf, (const uint8_t*)"\":{", 3));
    for (i = 0; (s == NATS_OK) && (i < md.Count); i++)
    {
        IFOK(s, natsBuf_AppendByte(buf, '"'));
        IFOK(s, natsBuf_AppendString(buf, md.List[i * 2]));
        IFOK(s, natsBuf_Append(buf, (const uint8_t *)"\":\"", 3));
        IFOK(s, natsBuf_AppendString(buf, md.List[i * 2 + 1]));
        IFOK(s, natsBuf_AppendByte(buf, '"'));

        if (i != md.Count - 1)
            IFOK(s, natsBuf_AppendByte(buf, ','));
    }
    IFOK(s, natsBuf_AppendByte(buf, '}'));
    return NATS_OK;
}

static natsStatus
_addMD(void *closure, const char *fieldName, nats_JSONField *f)
{
    natsMetadata *md = (natsMetadata *)closure;

    char *name = NATS_STRDUP(fieldName);
    char *value = NATS_STRDUP(f->value.vstr);
    if ((name == NULL) || (value == NULL))
    {
        NATS_FREE(name);
        NATS_FREE(value);
        return nats_setDefaultError(NATS_NO_MEMORY);
    }

    md->List[md->Count * 2] = name;
    md->List[md->Count * 2 + 1] = value;
    md->Count++;
    return NATS_OK;
}

natsStatus
nats_unmarshalMetadata(nats_JSON *json, natsPool *pool, const char *fieldName, natsMetadata *md)
{
    natsStatus s = NATS_OK;
    nats_JSON *mdJSON = NULL;
    int n;

    md->List = NULL;
    md->Count = 0;
    if (json == NULL)
        return NATS_OK;

    s = nats_JSONGetObject(json, fieldName, &mdJSON);
    if ((s != NATS_OK) || (mdJSON == NULL))
        return NATS_OK;

    n = natsStrHash_Count(mdJSON->fields);
    md->List = NATS_CALLOC(n * 2, sizeof(char *));
    if (md->List == NULL)
        s = nats_setDefaultError(NATS_NO_MEMORY);
    IFOK(s, nats_JSONRange(mdJSON, TYPE_STR, 0, _addMD, md));

    return s;
}

natsStatus
nats_cloneMetadata(natsPool *pool, natsMetadata *clone, natsMetadata md)
{
    natsStatus s = NATS_OK;
    int i = 0;
    int n;
    char **list = NULL;

    clone->Count = 0;
    clone->List = NULL;
    if (md.Count == 0)
        return NATS_OK;

    n = md.Count * 2;
    list = natsPool_Alloc(pool, n * sizeof(char *));
    if (list == NULL)
        s = nats_setDefaultError(NATS_NO_MEMORY);

    for (i = 0; (s == NATS_OK) && (i < n); i++)
    {
        list[i] = nats_StrdupPool(pool, md.List[i]);
        if (list[i] == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }

    if (s == NATS_OK)
    {
        clone->List = (const char **)list;
        clone->Count = md.Count;
    }

    return s;
}

