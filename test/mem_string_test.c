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
#include "test.h"

typedef struct
{
    const char *name;
    natsString *str1;
    natsString *str2;
    bool expected;
} nats_strEqualsTestCase;

typedef struct
{
    const char *name;
    natsString *str;
    const char *cstr;
    bool expected;
} nats_strEqualsCTestCase;

typedef struct
{
    const char *name;
    const char *p;
    bool expected;
} nats_isEmptyCTestCase;

typedef struct
{
    const char *name;
    char input;
    char expected;
} nats_charTestCase;

typedef struct
{
    const char *name;
    const char *s;
    size_t expected;
} safe_strlenTestCase;

typedef struct
{
    const char *name;
    const char *s;
    char find;
    char *expected;
} safe_strchrTestCase;

typedef struct
{
    const char *name;
    const char *s;
    const char *find;
    char *expected;
} safe_strstrTestCase;

typedef struct
{
    const char *name;
    const char *s1;
    const char *s2;
    bool expected;
} safe_streqTestCase;

typedef struct {
    const char *name;
    natsString *str1;
    natsString *str2;
    bool expected;
} nats_strEqualsTestCase;

typedef struct {
    const char *name;
    natsString *str;
    const char *cstr;
    bool expected;
} nats_strEqualsCTestCase;

typedef struct {
    const char *name;
    const char *p;
    bool expected;
} nats_isEmptyCTestCase;

typedef struct {
    const char *name;
    char input;
    char expected;
} nats_charTestCase;

typedef struct {
    const char *name;
    const char *s;
    size_t expected;
} safe_strlenTestCase;

typedef struct {
    const char *name;
    const char *s;
    char find;
    char *expected;
} safe_strchrTestCase;

typedef struct {
    const char *name;
    const char *s;
    const char *find;
    char *expected;
} safe_strstrTestCase;

typedef struct {
    const char *name;
    const char *s1;
    const char *s2;
    bool expected;
} safe_streqTestCase;

void Test_nats_strEquals(void)
{
    natsString str1 = {"hello", 5};
    natsString str2 = {"hello", 5};
    natsString str3 = {"world", 5};
    natsString str4 = {"hello world", 11};
    natsString emptyStr = {"", 0};

    nats_strEqualsTestCase cases[] = {
        {"same strings", &str1, &str2, true},
        {"different strings", &str1, &str3, false},
        {"different lengths", &str1, &str4, false},
        {"one NULL string", &str1, NULL, false},
        {"both NULL strings", NULL, NULL, true},
        {"empty strings", &emptyStr, &emptyStr, true}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(nats_equalStrings(cases[i].str1, cases[i].str2) == cases[i].expected);
    }
}

void Test_nats_strEqualsC(void)
{
    natsString str1 = {"hello", 5};
    natsString str2 = {"world", 5};
    natsString emptyStr = {"", 0};

    nats_strEqualsCTestCase cases[] = {
        {"same string and C string", &str1, "hello", true},
        {"different string and C string", &str1, "world", false},
        {"C string is NULL", &str1, NULL, false},
        {"both NULL", NULL, NULL, false},
        {"empty strings", &emptyStr, "", true}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(nats_equalsCString(cases[i].str, cases[i].cstr) == cases[i].expected);
    }
}

void Test_nats_isEmptyC(void)
{
    nats_isEmptyCTestCase cases[] = {
        {"NULL string", NULL, true},
        {"empty string", "", true},
        {"non-empty string", "hello", false}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(nats_strIsEmpty(cases[i].p) == cases[i].expected);
    }
}

void Test_nats_toLower(void)
{
    nats_charTestCase cases[] = {
        {"uppercase to lowercase A", 'A', 'a'},
        {"uppercase to lowercase Z", 'Z', 'z'},
        {"lowercase a", 'a', 'a'},
        {"non-alphabetic 1", '1', '1'}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(nats_toLower(cases[i].input) == cases[i].expected);
    }
}

void Test_nats_toUpper(void)
{
    nats_charTestCase cases[] = {
        {"lowercase to uppercase a", 'a', 'A'},
        {"lowercase to uppercase z", 'z', 'Z'},
        {"uppercase A", 'A', 'A'},
        {"non-alphabetic 1", '1', '1'}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(nats_toUpper(cases[i].input) == cases[i].expected);
    }
}

void Test_safe_strlen(void)
{
    safe_strlenTestCase cases[] = {
        {"NULL string", NULL, 0},
        {"empty string", "", 0},
        {"non-empty string hello", "hello", 5}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(safe_strlen(cases[i].s) == cases[i].expected);
    }
}

void Test_safe_strchr(void)
{
    safe_strchrTestCase cases[] = {
        {"NULL string", NULL, 'h', NULL},
        {"empty string", "", 'h', NULL},
        {"non-empty string, char present", "hello", 'e', &"hello"[1]},
        {"non-empty string, char absent", "hello", 'x', NULL}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(safe_strchr(cases[i].s, cases[i].find) == cases[i].expected);
    }
}

void Test_safe_strstr(void)
{
    safe_strstrTestCase cases[] = {
        {"NULL string", NULL, "he", NULL},
        {"empty string", "", "he", NULL},
        {"non-empty string, substring present", "hello", "ell", &"hello"[1]},
        {"non-empty string, substring absent", "hello", "world", NULL}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(safe_strstr(cases[i].s, cases[i].find) == cases[i].expected);
    }
}

void Test_safe_streq(void)
{
    safe_streqTestCase cases[] = {
        {"same strings", "hello", "hello", true},
        {"different strings", "hello", "world", false}};

    for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        test(cases[i].name);
        testCond(safe_streq(cases[i].s1, cases[i].s2) == cases[i].expected);
    }
}

