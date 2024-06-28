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

#include "json.h"
#include "conn.h"

static natsString _serverID = NATS_STR("server_id");
static natsString _version = NATS_STR("version");
static natsString _host = NATS_STR("host");
static natsString _port = NATS_STR("port");
static natsString _authRequired = NATS_STR("auth_required");
static natsString _tlsRequired = NATS_STR("tls_required");
static natsString _tlsAvailable = NATS_STR("tls_available");
static natsString _maxPayload = NATS_STR("max_payload");
static natsString _connectURLs = NATS_STR("connect_urls");
static natsString _proto = NATS_STR("proto");
static natsString _CID = NATS_STR("client_id");
static natsString _nonce = NATS_STR("nonce");
static natsString _clientIP = NATS_STR("client_ip");
static natsString _lameDuckMode = NATS_STR("ldm");
static natsString _headers = NATS_STR("headers");

natsStatus
nats_unmarshalServerInfo(nats_JSON *json, natsPool *pool, natsServerInfo *info)
{
    natsStatus s = NATS_OK;
    IFOK(s, nats_strdupJSONIfDiff(&info->id, json, pool, &_serverID));
    IFOK(s, nats_strdupJSONIfDiff(&info->version, json, pool, &_version));
    IFOK(s, nats_strdupJSONIfDiff(&info->host, json, pool, &_host));
    IFOK(s, nats_getJSONInt(&info->port, json, &_port));
    IFOK(s, nats_getJSONBool(&info->authRequired, json, &_authRequired));
    IFOK(s, nats_getJSONBool(&info->tlsRequired, json, &_tlsRequired));
    IFOK(s, nats_getJSONBool(&info->tlsAvailable, json, &_tlsAvailable));
    IFOK(s, nats_getJSONLong(&info->maxPayload, json, &_maxPayload));
    IFOK(s, nats_dupJSONStringArrayIfDiff(&info->connectURLs, &info->connectURLsCount, json, pool, &_connectURLs));
    IFOK(s, nats_getJSONInt(&info->proto, json, &_proto));
    IFOK(s, nats_getJSONULong(&info->CID, json, &_CID));
    IFOK(s, nats_strdupJSONIfDiff(&info->nonce, json, pool, &_nonce));
    IFOK(s, nats_strdupJSONIfDiff(&info->clientIP, json, pool, &_clientIP));
    IFOK(s, nats_getJSONBool(&info->lameDuckMode, json, &_lameDuckMode));
    IFOK(s, nats_getJSONBool(&info->headers, json, &_headers));

    return NATS_UPDATE_ERR_STACK(s);
}
