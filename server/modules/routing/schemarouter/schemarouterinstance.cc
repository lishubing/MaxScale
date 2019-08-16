/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "schemarouter.hh"
#include "schemarouterinstance.hh"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <maxbase/alloc.h>
#include <maxscale/buffer.hh>
#include <maxscale/modinfo.hh>
#include <maxscale/modutil.hh>
#include <maxscale/poll.hh>
#include <maxscale/query_classifier.hh>
#include <maxscale/router.hh>
#include <maxscale/secrets.h>

using std::string;

namespace schemarouter
{

#define DEFAULT_REFRESH_INTERVAL "300s"

/**
 * @file schemarouter.c The entry points for the simple sharding router module.
 */

SchemaRouter::SchemaRouter(SERVICE* service, SConfig config)
    : mxs::Router<SchemaRouter, SchemaRouterSession>(service)
    , m_config(config)
    , m_service(service)
{
}

SchemaRouter::~SchemaRouter()
{
}

SchemaRouter* SchemaRouter::create(SERVICE* pService, MXS_CONFIG_PARAMETER* params)
{
    // TODO: This is wrong: a router shouldn't modify core parameters. This should be expressed in some other
    // form that the service would know to behave differently if a router requires authentication from all
    // servers.

    // if (!params->contains("auth_all_servers"))
    // {
    //     MXS_NOTICE("Authentication data is fetched from all servers. To disable this "
    //                "add 'auth_all_servers=0' to the service.");
    //     pService->users_from_all = true;
    // }

    SConfig config(new Config(params));
    return new SchemaRouter(pService, config);
}

bool SchemaRouter::configure(MXS_CONFIG_PARAMETER* params)
{
    SConfig config(new Config(params));
    m_config = config;
    return true;
}

/**
 * @node Search all RUNNING backend servers and connect
 *
 * Parameters:
 * @param backend_ref - in, use, out
 *      Pointer to backend server reference object array.
 *      NULL is not allowed.
 *
 * @param router_nservers - in, use
 *      Number of backend server pointers pointed to by b.
 *
 * @param session - in, use
 *      MaxScale session pointer used when connection to backend is established.
 *
 * @param  router - in, use
 *      Pointer to router instance. Used when server states are qualified.
 *
 * @return true, if at least one master and one slave was found.
 *
 *
 * @details It is assumed that there is only one available server.
 *      There will be exactly as many backend references than there are
 *      connections because all servers are supposed to be operational. It is,
 *      however, possible that there are less available servers than expected.
 */
bool connect_backend_servers(SRBackendList& backends, MXS_SESSION* session)
{
    bool succp = false;
    int servers_found = 0;
    int servers_connected = 0;
    int slaves_connected = 0;

    /**
     * Scan server list and connect each of them. None should fail or session
     * can't be established.
     */
    for (const auto& b : backends)
    {
        if (b->target()->is_connectable())
        {
            servers_found += 1;

            /** Server is already connected */
            if (b->in_use())
            {
                slaves_connected += 1;
            }
            /** New server connection */
            else
            {
                if (b->connect())
                {
                    servers_connected += 1;
                }
                else
                {
                    succp = false;
                    MXS_ERROR("Unable to establish "
                              "connection with slave '%s'",
                              b->name());
                    /* handle connect error */
                    break;
                }
            }
        }
    }

    if (servers_connected > 0)
    {
        succp = true;

        if (mxs_log_is_priority_enabled(LOG_INFO))
        {
            for (const auto& b : backends)
            {
                if (b->in_use())
                {
                    MXS_INFO("Connected %s in \t'%s'",
                             b->target()->status_string().c_str(),
                             b->name());
                }
            }
        }
    }

    return succp;
}

SchemaRouterSession* SchemaRouter::newSession(MXS_SESSION* pSession, const Endpoints& endpoints)
{
    SRBackendList backends;

    for (auto e : endpoints)
    {
        backends.emplace_back(new SRBackend(e));
    }

    SchemaRouterSession* rval = NULL;

    if (connect_backend_servers(backends, pSession))
    {
        rval = new SchemaRouterSession(pSession, this, std::move(backends));
    }

    return rval;
}

void SchemaRouter::diagnostics(DCB* dcb)
{
    double sescmd_pct = m_stats.n_sescmd != 0 ?
        100.0 * ((double)m_stats.n_sescmd / (double)m_stats.n_queries) :
        0.0;

    /** Session command statistics */
    dcb_printf(dcb, "\n\33[1;4mSession Commands\33[0m\n");
    dcb_printf(dcb,
               "Total number of queries: %d\n",
               m_stats.n_queries);
    dcb_printf(dcb,
               "Percentage of session commands: %.2f\n",
               sescmd_pct);
    dcb_printf(dcb,
               "Longest chain of stored session commands: %d\n",
               m_stats.longest_sescmd);
    dcb_printf(dcb,
               "Session command history limit exceeded: %d times\n",
               m_stats.n_hist_exceeded);

    /** Session time statistics */

    if (m_stats.sessions > 0)
    {
        dcb_printf(dcb, "\n\33[1;4mSession Time Statistics\33[0m\n");
        dcb_printf(dcb, "Longest session: %.2lf seconds\n", m_stats.ses_longest);
        dcb_printf(dcb, "Shortest session: %.2lf seconds\n", m_stats.ses_shortest);
        dcb_printf(dcb, "Average session length: %.2lf seconds\n", m_stats.ses_average);
    }
    dcb_printf(dcb, "Shard map cache hits: %d\n", m_stats.shmap_cache_hit);
    dcb_printf(dcb, "Shard map cache misses: %d\n", m_stats.shmap_cache_miss);
    dcb_printf(dcb, "\n");
}

json_t* SchemaRouter::diagnostics_json() const
{
    double sescmd_pct = m_stats.n_sescmd != 0 ?
        100.0 * ((double)m_stats.n_sescmd / (double)m_stats.n_queries) :
        0.0;

    json_t* rval = json_object();
    json_object_set_new(rval, "queries", json_integer(m_stats.n_queries));
    json_object_set_new(rval, "sescmd_percentage", json_real(sescmd_pct));
    json_object_set_new(rval, "longest_sescmd_chain", json_integer(m_stats.longest_sescmd));
    json_object_set_new(rval, "times_sescmd_limit_exceeded", json_integer(m_stats.n_hist_exceeded));

    /** Session time statistics */
    if (m_stats.sessions > 0)
    {
        json_object_set_new(rval, "longest_session", json_real(m_stats.ses_longest));
        json_object_set_new(rval, "shortest_session", json_real(m_stats.ses_shortest));
        json_object_set_new(rval, "average_session", json_real(m_stats.ses_average));
    }

    json_object_set_new(rval, "shard_map_hits", json_integer(m_stats.shmap_cache_hit));
    json_object_set_new(rval, "shard_map_misses", json_integer(m_stats.shmap_cache_miss));

    return rval;
}

static const uint64_t CAPABILITIES = RCAP_TYPE_CONTIGUOUS_INPUT | RCAP_TYPE_PACKET_OUTPUT
    | RCAP_TYPE_RUNTIME_CONFIG | RCAP_TYPE_REQUEST_TRACKING;

uint64_t SchemaRouter::getCapabilities()
{
    return schemarouter::CAPABILITIES;
}
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static auto desc = "A database sharding router for simple sharding";
    static MXS_MODULE info =
    {
        MXS_MODULE_API_ROUTER,
        MXS_MODULE_BETA_RELEASE,
        MXS_ROUTER_VERSION,
        desc,
        "V1.0.0",
        schemarouter::CAPABILITIES,
        &schemarouter::SchemaRouter::s_object,
        NULL,
        NULL,
        NULL,
        NULL,
        {
            {CN_IGNORE_TABLES,                MXS_MODULE_PARAM_STRING },
            {CN_IGNORE_TABLES_REGEX,          MXS_MODULE_PARAM_STRING },
            {CN_IGNORE_DATABASES,             MXS_MODULE_PARAM_STRING },
            {CN_IGNORE_DATABASES_REGEX,       MXS_MODULE_PARAM_STRING },
            {"max_sescmd_history",            MXS_MODULE_PARAM_COUNT, "0"},
            {"disable_sescmd_history",        MXS_MODULE_PARAM_BOOL, "false"},
            {"refresh_databases",             MXS_MODULE_PARAM_BOOL, "true"},
            {
                "refresh_interval",
                MXS_MODULE_PARAM_DURATION,
                DEFAULT_REFRESH_INTERVAL,
                MXS_MODULE_OPT_DURATION_S
            },
            {"debug",                         MXS_MODULE_PARAM_BOOL, "false"},
            {"preferred_server",              MXS_MODULE_PARAM_SERVER },
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}
