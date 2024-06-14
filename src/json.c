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

#define JSON_GET_ARRAY(_p, _t, _f)                           \
    natsStatus s = NATS_OK;                                  \
    nats_JSONField *field = NULL;                            \
    s = nats_JSONRefArray(json, fieldName, (_t), &field);    \
    if ((s == NATS_OK) && (field == NULL))                   \
    {                                                        \
        *array = NULL;                                       \
        *arraySize = 0;                                      \
        return NATS_OK;                                      \
    }                                                        \
    else if (s == NATS_OK)                                   \
        s = (_f)((_p), field->value.varr, array, arraySize); \
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
nats_JSONDupStr(nats_JSON *json, natsPool *pool, const char *fieldName, const char **value)
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
nats_JSONRefObject(nats_JSON *json, const char *fieldName, nats_JSON **value)
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

natsStatus
nats_JSONRefArray(nats_JSON *json, const char *fieldName, int fieldType, nats_JSONField **retField)
{
    nats_JSONField *field = NULL;

    field = (nats_JSONField *)natsStrHash_Get(json->fields, (char *)fieldName, strlen(fieldName));
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
_jsonArrayAsStrings(natsPool *pool, nats_JSONArray *arr, const char ***array, int *arraySize)
{
    int i;

    const char **values = natsPool_Alloc(pool, arr->size * arr->eltSize);
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
nats_JSONDupStringArray(nats_JSON *json, natsPool *pool, const char *fieldName, const char ***array, int *arraySize)
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
