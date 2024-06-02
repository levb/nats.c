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
#include "mem.h"

void Test_Pool(void)
{
    natsPool *pool = NULL;

    test("Create pool");
    natsStatus s = natsPool_CreateNamed(&pool, 1234, "mem-test");

    size_t expectedLength = sizeof(natsPool) + sizeof(natsSmall);
    testCond((s == NATS_OK) &&
             (pool != NULL) &&
             (pool->small != NULL) &&
             (pool->small->len == expectedLength) &&
             (pool->pageSize == 1234));

    test("Allocate some small blocks in the first chunk");
    uint8_t *ptr1 = natsPool_Alloc(pool, 10);
    uint8_t *ptr2 = natsPool_Alloc(pool, 20);
    uint8_t *ptr3 = natsPool_Alloc(pool, 30);
    expectedLength += 10 + 20 + 30;
    testCond((ptr1 != NULL) && (ptr2 != NULL) && (ptr3 != NULL) &&
                (pool->small->len == expectedLength) &&
                (uint8_t*)pool + sizeof(natsPool) == ptr1 &&
                ptr2 == ptr1 + 10 &&
                ptr3 == ptr2 + 20);

}
