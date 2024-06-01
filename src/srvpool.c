// Copyright 2015-2024 The NATS Authors
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

#include "mem.h"
#include "url.h"
#include "hash.h"
#include "srvpool.h"
#include "err.h"
#include "conn.h"
#include "opts.h"

static natsStatus
_createSrv(natsSrv **newSrv, natsPool *pool, char *url, bool implicit, const char *tlsName)
{
    natsStatus  s = NATS_OK;
    natsSrv     *srv = (natsSrv*) natsPool_Alloc(pool, sizeof(natsSrv));
    if (srv == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    srv->isImplicit = implicit;

    s = natsUrl_Create(&(srv->url), pool, url);
    if ((s == NATS_OK) && (tlsName != NULL))
    {
        srv->tlsName = NATS_STRDUP(tlsName);
        if (srv->tlsName == NULL)
            s = nats_setDefaultError(NATS_NO_MEMORY);
    }
    if (s == NATS_OK)
        *newSrv = srv;
    else
        _freeSrv(srv);

    return NATS_UPDATE_ERR_STACK(s);
}

natsSrv*
natsSrvPool_GetCurrentServer(natsSrvPool *pool, const natsSrv *cur, int *index)
{
    natsSrv *s = NULL;
    int     i;

    for (i = 0; i < pool->size; i++)
    {
        s = pool->srvrs[i];
        if (s == cur)
        {
            if (index != NULL)
                *index = i;

            return s;
        }
    }

    if (index != NULL)
        *index = -1;

    return NULL;
}

// Pop the current server and put onto the end of the list. Select head of list as long
// as number of reconnect attempts under MaxReconnect.
natsSrv*
natsSrvPool_GetNextServer(natsSrvPool *pool, natsOptions *opts, const natsSrv *cur)
{
    natsSrv *s = NULL;
    int     i, j;

    s = natsSrvPool_GetCurrentServer(pool, cur, &i);
    if (i < 0)
        return NULL;

    // Shift left servers past current to the current's position
    for (j = i; j < pool->size - 1; j++)
        pool->srvrs[j] = pool->srvrs[j+1];

    if ((opts->maxReconnect < 0)
        || (s->reconnects < opts->maxReconnect))
    {
        // Move the current server to the back of the list
        pool->srvrs[pool->size - 1] = s;
    }
    else
    {
        // Remove the server from the list
        _freeSrv(s);
        pool->size--;
    }

    if (pool->size <= 0)
        return NULL;

    return pool->srvrs[0];
}

void
natsSrvPool_Destroy(natsSrvPool *pool)
{
    natsSrv *srv;
    int     i;

    if (pool == NULL)
        return;

    for (i = 0; i < pool->size; i++)
    {
        srv = pool->srvrs[i];
        _freeSrv(srv);
    }
    natsStrHash_Destroy(pool->urls);
    pool->urls = NULL;

    NATS_FREE(pool->srvrs);
    pool->srvrs = NULL;
    pool->size  = 0;
    NATS_FREE(pool->user);
    NATS_FREE(pool->pwd);
    NATS_FREE(pool);
}

static natsStatus
_addURLToPool(natsSrvPool *pool, char *sURL, bool implicit, const char *tlsName)
{
    natsStatus  s;
    natsSrv     *srv = NULL;
    bool        addedToMap = false;
    char        bareURL[256];

    s = _createSrv(&srv, sURL, implicit, tlsName);
    if (s != NATS_OK)
        return NATS_UPDATE_ERR_STACK(s);

    // For and explicit URL, we will save the user info if one is provided
    // and if not already done.
    if (!implicit && (pool->user == NULL) && (srv->url->username != NULL))
    {
        DUP_STRING(s, pool->user, srv->url->username);
        if ((s == NATS_OK) && !nats_isStringEmpty(srv->url->password))
            DUP_STRING(s, pool->pwd, srv->url->password);
        if (s != NATS_OK)
            return NATS_UPDATE_ERR_STACK(s);
    }

    // In the map, we need to add an URL that is just host:port
    snprintf(bareURL, sizeof(bareURL), "%s:%d", srv->url->host, srv->url->port);
    s = natsStrHash_Set(pool->urls, bareURL, true, (void*)1, NULL);
    if (s == NATS_OK)
    {
        addedToMap = true;
        if (pool->size + 1 > pool->cap)
        {
            natsSrv **newArray  = NULL;
            int     newCap      = 2 * pool->cap;

            newArray = (natsSrv**) NATS_REALLOC(pool->srvrs, newCap * sizeof(char*));
            if (newArray == NULL)
                s = nats_setDefaultError(NATS_NO_MEMORY);

            if (s == NATS_OK)
            {
                pool->cap = newCap;
                pool->srvrs = newArray;
            }
        }
        if (s == NATS_OK)
            pool->srvrs[pool->size++] = srv;
    }
    if (s != NATS_OK)
    {
        if (addedToMap)
            natsStrHash_Remove(pool->urls, sURL);

        _freeSrv(srv);
    }

    return NATS_UPDATE_ERR_STACK(s);
}

static void
_shufflePool(natsSrvPool *pool, int offset)
{
    int     i, j;
    natsSrv *tmp;

    if (pool->size <= offset+1)
        return;

    srand((unsigned int) nats_NowInNanoSeconds());

    for (i = offset; i < pool->size; i++)
    {
        j = offset + rand() % (i + 1 - offset);
        tmp = pool->srvrs[i];
        pool->srvrs[i] = pool->srvrs[j];
        pool->srvrs[j] = tmp;
    }
}

natsStatus
natsSrvPool_addNewURLs(natsSrvPool **newPool, natsPool *memPool, natsSrvPool *oldPool,
 const natsUrl *curUrl, const char **urls, int urlCount, const char *tlsName, bool *added)
{
    natsStatus  s       = NATS_OK;
    char        url[256];
    char        *sport;
    int         portPos;
    bool        found;
    bool        isLH;
    natsSrv *srv = NULL;
    natsSrv *newSrv = NULL;
    const char  **infoURLs = NULL;
    int         infoURLCount = urlCount;

    if (newPool == NULL)
        return nats_setDefaultError(NATS_INVALID_ARG);

    // Note about pool randomization: when the pool was first created,
    // it was randomized (if allowed). We keep the order the same (removing
    // implicit servers that are no longer sent to us). New URLs are sent
    // to us in no specific order so don't need extra randomization.

    *added = false;

    // Clone the INFO urls so we can modify the list.
    infoURLs = natsPool_Alloc(memPool, sizeof(const char*) * urlCount);
    if (infoURLs == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);
    for (int i=0; i<urlCount; i++)
        infoURLs[i] = urls[i];

    // Allocate the new server pool
    *newPool = natsPool_Alloc(memPool, sizeof(natsSrvPool));

    // Walk the pool and removed the implicit servers that are no longer in the
    // given array/map
    for (int i=0; i<oldPool->size; i++)
    {
        bool *inInfo ;
        int n;

        srv = oldPool->srvrs[i];
        snprintf(url, sizeof(url), "%s:%d", srv->url->host, srv->url->port);

        // Remove from the temp list so that at the end we are left with only
        // new (or restarted) servers that need to be added to the pool.
        n = nats_strarray_remove((char**) infoURLs, infoURLCount, url);
        inInfo = (n != infoURLCount);
        infoURLCount = n;

        // Keep servers that were set through Options, but also the one that
        // we are currently connected to (even if it is a discovered server).
        if (!(srv->isImplicit) || (srv->url == curUrl))
        {
            newSrv = natsPool_Alloc(memPool, sizeof(natsSrv));
            if (newSrv == NULL)
                return nats_setDefaultError(NATS_NO_MEMORY);
        }
        if (!inInfo)
        {
            // Remove from server pool. Keep current order.

            // Shift left servers past current to the current's position
            for (j = i; j < pool->size - 1; j++)
            {
                pool->srvrs[j] = pool->srvrs[j+1];
            }
            _freeSrv(srv);
            pool->size--;
            i--;
        }
    }

    // If there are any left in the tmp map, these are new (or restarted) servers
    // and need to be added to the pool.
    if (s == NATS_OK)
    {
        natsStrHashIter iter;
        char            *curl = NULL;

        natsStrHashIter_Init(&iter, tmp);
        while ((s == NATS_OK) && natsStrHashIter_Next(&iter, &curl, NULL))
        {
            // Before adding, check if this is a new (as in never seen) URL.
            // This is used to figure out if we invoke the DiscoveredServersCB

            isLH  = false;
            found = false;

            // Consider localhost:<port>, 127.0.0.1:<port> and [::1]:<port>
            // all the same.
            sport = strrchr(curl, ':');
            portPos = (int) (sport - curl);
            if (((nats_strcasestr(curl, "localhost") == curl) && (portPos == 9))
                    || (strncmp(curl, "127.0.0.1", portPos) == 0)
                    || (strncmp(curl, "[::1]", portPos) == 0))
            {
                isLH = ((curl[0] == 'l') || (curl[0] == 'L'));

                snprintf(url, sizeof(url), "localhost%s", sport);
                found = (natsStrHash_Get(pool->urls, url) != NULL);
                if (!found)
                {
                    snprintf(url, sizeof(url), "127.0.0.1%s", sport);
                    found = (natsStrHash_Get(pool->urls, url) != NULL);
                }
                if (!found)
                {
                    snprintf(url, sizeof(url), "[::1]%s", sport);
                    found = (natsStrHash_Get(pool->urls, url) != NULL);
                }
            }
            else
            {
                found = (natsStrHash_Get(pool->urls, curl) != NULL);
            }

            snprintf(url, sizeof(url), "nats://%s", curl);
            if (!found)
            {
                // Make sure that localhost URL is always stored in lower case.
                if (isLH)
                    snprintf(url, sizeof(url), "nats://localhost%s", sport);

                *added = true;
            }
            s = _addURLToPool(pool, url, true, tlsName);
        }
        natsStrHashIter_Done(&iter);
        if ((s == NATS_OK) && added && pool->randomize)
            _shufflePool(pool, 1);
    }

    natsStrHash_Destroy(tmp);

    return NATS_UPDATE_ERR_STACK(s);
}

// Create the server pool using the options given.
// We will place a Url option first, followed by any
// Server Options. We will randomize the server pool unlesss
// the NoRandomize flag is set.
natsStatus
natsSrvPool_Create(natsSrvPool **newPool, natsOptions *opts)
{
    natsStatus  s        = NATS_OK;
    natsSrvPool *pool    = NULL;
    int         poolSize;
    int         i;

    poolSize  = (opts->url != NULL ? 1 : 0);
    poolSize += opts->serversCount;

    // If the pool is going to be empty, we will add the default URL.
    if (poolSize == 0)
        poolSize = 1;

    pool = (natsSrvPool*) NATS_CALLOC(1, sizeof(natsSrvPool));
    if (pool == NULL)
        return nats_setDefaultError(NATS_NO_MEMORY);

    pool->srvrs = (natsSrv**) NATS_CALLOC(poolSize, sizeof(natsSrv*));
    if (pool->srvrs == NULL)
    {
        NATS_FREE(pool);
        return nats_setDefaultError(NATS_NO_MEMORY);
    }
    // Set the current capacity. The array of urls may have to grow in
    // the future.
    pool->cap = poolSize;
    pool->randomize = !opts->noRandomize;

    // Map that helps find out if an URL is already known.
    s = natsStrHash_Create(&(pool->urls), NULL, poolSize);

    // Add URLs from Options' Servers
    for (i=0; (s == NATS_OK) && (i < opts->serversCount); i++)
        s = _addURLToPool(pool, opts->servers[i], false, NULL);

    if (s == NATS_OK)
    {
        // Randomize if allowed to
        if (pool->randomize)
            _shufflePool(pool, 0);
    }

    // Normally, if this one is set, Options.Servers should not be,
    // but we always allowed that, so continue to do so.
    if ((s == NATS_OK) && (opts->url != NULL))
    {
        // Add to the end of the array
        s = _addURLToPool(pool, opts->url, false, NULL);
        if ((s == NATS_OK) && (pool->size > 1))
        {
            // Then swap it with first to guarantee that Options.Url is tried first.
            natsSrv *opstUrl = pool->srvrs[pool->size-1];

            pool->srvrs[pool->size-1] = pool->srvrs[0];
            pool->srvrs[0] = opstUrl;
        }
    }
    else if ((s == NATS_OK) && (pool->size == 0))
    {
        // Place default URL if pool is empty.
        s = _addURLToPool(pool, (char*) NATS_DEFAULT_URL, false, NULL);
    }

    if (s == NATS_OK)
        *newPool = pool;
    else
        natsSrvPool_Destroy(pool);

    return NATS_UPDATE_ERR_STACK(s);
}

