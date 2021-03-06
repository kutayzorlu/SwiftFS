/*  
 * Copyright 2012-2013 Paul Ionkin <paul.ionkin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/
#include "global.h"
#include "client_pool.h"

struct _ClientPool {
    Application *app;
    ConfData *conf;

    struct event_base *evbase;
    struct evdns_base *dns_base;
    GList *l_clients; // the list of PoolClient (HTTPClient or HTTPConnection)
    GQueue *q_requests; // the queue of awaiting requests
};

typedef struct {
    ClientPool *pool;
    ClientPool_client_check_rediness client_check_rediness; // is client ready for a new request
    ClientPool_client_destroy client_destroy;
    ClientPool_client_get_info client_get_info;
    gpointer client;
} PoolClient;

typedef struct {
    ClientPool_on_client_ready on_client_ready;
    gpointer ctx;
} RequestData;

#define POOL "pool"

static void client_pool_on_client_released (gpointer client, gpointer ctx);

// creates connection pool object
// create client_count clients
// return NULL if error
ClientPool *client_pool_create (Application *app, 
    gint client_count,
    ClientPool_client_create client_create, 
    ClientPool_client_destroy client_destroy, 
    ClientPool_client_set_on_released_cb client_set_on_released_cb,
    ClientPool_client_check_rediness client_check_rediness,
    ClientPool_client_get_info client_get_info
)
{
    ClientPool *pool;
    gint i;
    PoolClient *pc;


    pool = g_new0 (ClientPool, 1);
    pool->app = app;
    pool->conf = application_get_conf (app);
    pool->evbase = application_get_evbase (app);
    pool->dns_base = application_get_dnsbase (app);
    pool->l_clients = NULL;
    pool->q_requests = g_queue_new ();
   
    for (i = 0; i < client_count; i++) {
        pc = g_new0 (PoolClient, 1);
        pc->pool = pool;
        pc->client = client_create (app);
        pc->client_check_rediness = client_check_rediness;
        pc->client_destroy = client_destroy;
        pc->client_get_info = client_get_info;
        // add to the list
        pool->l_clients = g_list_append (pool->l_clients, pc);
        // add callback
        client_set_on_released_cb (pc->client, client_pool_on_client_released, pc);
    }

    return pool;
}

void client_pool_destroy (ClientPool *pool)
{
    GList *l;
    PoolClient *pc;
    
    if (pool->q_requests)
        g_queue_free_full (pool->q_requests, g_free);
    for (l = g_list_first (pool->l_clients); l; l = g_list_next (l)) {
        pc = (PoolClient *) l->data;
        pc->client_destroy (pc->client);
        g_free (pc);
    }
    g_list_free (pool->l_clients);

    g_free (pool);
}

// callback executed when a client done with a request
static void client_pool_on_client_released (gpointer client, gpointer ctx)
{
    PoolClient *pc = (PoolClient *) ctx;
    RequestData *data;

    // if we have a request pending
    data = g_queue_pop_head (pc->pool->q_requests);
    if (data) {
        LOG_debug (POOL, "Retrieving client from the Pool: %p", data->ctx);
        data->on_client_ready (client, data->ctx);
        g_free (data);
    }
}

// add client's callback to the awaiting queue
// return TRUE if added, FALSE if list is full
gboolean client_pool_get_client (ClientPool *pool, ClientPool_on_client_ready on_client_ready, gpointer ctx)
{
    GList *l;
    RequestData *data;
    PoolClient *pc;
    
    // check if the awaiting queue is full
    if (g_queue_get_length (pool->q_requests) >= conf_get_uint (pool->conf, "pool.max_requests_per_pool")) {
        LOG_debug (POOL, "Pool's client awaiting queue is full !");
        return FALSE;
    }

    // check if there is a client which is ready to execute a new request
    for (l = g_list_first (pool->l_clients); l; l = g_list_next (l)) {
        pc = (PoolClient *) l->data;
        
        // check if client is ready
        if (pc->client_check_rediness (pc->client)) {
            on_client_ready (pc->client, ctx);
            return TRUE;
        }
    }

    LOG_debug (POOL, "all Pool's clients are busy, putting into queue: %p", ctx);
    
    // add client to the end of queue
    data = g_new0 (RequestData, 1);
    data->on_client_ready = on_client_ready;
    data->ctx = ctx;
    g_queue_push_tail (pool->q_requests, data);

    return TRUE;
}

GList *client_pool_get_task_list (ClientPool *pool, GList *l_tasks, const gchar *pool_name)
{
    GList *l;
    
    for (l = g_list_first (pool->l_clients); l; l = g_list_next (l)) {
        PoolClient *pc = (PoolClient *) l->data;
        ClientInfo *client_info = pc->client_get_info (pc->client);
        client_info->pool_name = g_strdup (pool_name);
        l_tasks = g_list_append (l_tasks, client_info);
    }

    return l_tasks;
}
