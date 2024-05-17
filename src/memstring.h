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

#ifndef STRING_H_
#define STRING_H_

#include <stdlib.h>
#include <string.h>

#define nats_ToLower(c) (uint8_t)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c)
#define nats_ToUpper(c) (uint8_t)((c >= 'a' && c <= 'z') ? (c & ~0x20) : c)

typedef struct {
    size_t      len;
    uint8_t     *data;
} natsString;


#define NATS_STR(str)     { sizeof(str) - 1, (uint8_t *) str }
#define NATS_EMPTY_STR     { 0, NULL }

#define natsString_Set(str, text) ((str)->len = sizeof(text) - 1, (str)->data = (uint8_t *)(text), str)
#define natsString_SetStr(str, text) ((str)->len = strlen(text), (str)->data = (uint8_t *)(text), str)
#define natsString_Reset(str) ((str)->len = 0, (str)->data = NULL, str)

void natsString_Lower(uint8_t *dst, uint8_t *src, size_t n);

#define ngx_base64_encoded_length(len)  (((len + 2) / 3) * 4)
#define ngx_base64_decoded_length(len)  (((len + 3) / 4) * 3)

#endif /* STRING_H_ */
