// Copyright 2015-2022 The NATS Authors
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

#ifndef DEV_MODE_H_
#define DEV_MODE_H_

// Comment/uncomment to replace some function calls with direct structure
// access
#define DEV_MODE (1)
#define DEV_MODE_MEM (1)

#ifndef _WIN32
#define __SHORT_FILE__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#else
#define __SHORT_FILE__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#endif

#define DEVNOLOG(s)
#define DEVNOLOGf(fmt, ...)
#define DEVNOLOGx(file, line, func, fmt, ...) 

#ifdef DEV_MODE

#define DEV_MODE_ARGS , const char *file, int line, const char *func
#define DEVLOGx(file, line, func, fmt, ...) fprintf(stderr, "DEV: %s:%d: %s: " fmt "\n", (file), (line), (func), __VA_ARGS__)
#define DEVLOG(str) DEVLOGx(__SHORT_FILE__, __LINE__, __func__, "%s", str)
#define DEVLOGf(fmt, ...) DEVLOGx(__SHORT_FILE__, __LINE__, __func__, fmt, __VA_ARGS__)

#else

#define DEV_MODE_ARGS
#define DEVLOGx DEVNOLOGx

#endif // DEV_MODE

#ifdef DEV_MODE_MEM

#define MEMLOG DEVLOG
#define MEMLOGf DEVLOGf
#define MEMLOGx DEVLOGx
#define DEV_MODE_POOL_NAME_ARG , const char *name

#else
#define MEMLOG DEVNOLOG
#define MEMLOGf DEVNOLOGf
#define MEMLOGx DEVNOLOGx
#define DEV_MODE_POOL_NAME_ARG
#endif

#ifdef DEV_MODE_CONN
#define CONNLOG DEVLOG
#define CONNLOGf DEVLOGf
#else
#define CONNLOG DEVNOLOG
#define CONNLOGf DEVNOLOGf
#endif

#endif /* DEV_MODE_H_ */
