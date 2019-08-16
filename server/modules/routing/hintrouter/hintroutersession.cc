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

#define MXS_MODULE_NAME "hintrouter"
#include "hintroutersession.hh"

#include <algorithm>
#include <functional>

#include <maxbase/alloc.h>
#include "hintrouter.hh"

namespace
{
/**
 * Writer is a function object that writes a clone of a provided GWBUF,
 * to each dcb it is called with.
 */
class Writer : std::unary_function<HintRouterSession::MapElement, bool>
{
public:
    Writer(GWBUF* pPacket)
        : m_pPacket(pPacket)
    {
    }

    bool operator()(HintRouterSession::MapElement& elem)
    {
        bool rv = false;
        auto endpoint = elem.second;
        GWBUF* pPacket = gwbuf_clone(m_pPacket);

        if (pPacket)
        {
            HR_DEBUG("Writing packet to %p %s.", endpoint, endpoint->target()->name());
            rv = endpoint->routeQuery(pPacket);
        }
        return rv;
    }

private:
    GWBUF* m_pPacket;
};
}

HintRouterSession::HintRouterSession(MXS_SESSION* pSession,
                                     HintRouter* pRouter,
                                     const BackendMap& backends)
    : maxscale::RouterSession(pSession)
    , m_router(pRouter)
    , m_backends(backends)
    , m_master(NULL)
    , m_n_routed_to_slave(0)
    , m_surplus_replies(0)
{
    HR_ENTRY();
    update_connections();
}


HintRouterSession::~HintRouterSession()
{
    HR_ENTRY();
}


void HintRouterSession::close()
{
    HR_ENTRY();
    m_master = nullptr;
    m_slaves.clear();
    m_backends.clear();
}

int32_t HintRouterSession::routeQuery(GWBUF* pPacket)
{
    HR_ENTRY();

    bool success = false;

    if (pPacket->hint)
    {
        /* At least one hint => look for match. Only use the later hints if the
         * first is unsuccessful. */
        HINT* current_hint = pPacket->hint;
        HR_DEBUG("Hint, looking for match.");
        while (!success && current_hint)
        {
            success = route_by_hint(pPacket, current_hint, false);
            if (!success)
            {
                current_hint = current_hint->next;
            }
        }
    }

    if (!success)
    {
        HR_DEBUG("No hints or hint-based routing failed, falling back to default action.");
        HINT default_hint = {};
        default_hint.type = m_router->get_default_action();
        if (default_hint.type == HINT_ROUTE_TO_NAMED_SERVER)
        {
            default_hint.data = MXS_STRDUP(m_router->get_default_server().c_str());
            // Ignore allocation error, it will just result in an error later on
        }
        success = route_by_hint(pPacket, &default_hint, true);
        if (default_hint.type == HINT_ROUTE_TO_NAMED_SERVER)
        {
            MXS_FREE(default_hint.data);
        }
    }

    if (!success)
    {
        gwbuf_free(pPacket);
    }
    return success;
}


void HintRouterSession::clientReply(GWBUF* pPacket, const mxs::ReplyRoute& down, const mxs::Reply& reply)
{
    HR_ENTRY();

    mxs::Target* pTarget = down.back()->target();

    if (m_surplus_replies == 0)
    {
        HR_DEBUG("Returning packet from %s.", pTarget->name());

        RouterSession::clientReply(pPacket, down, reply);
    }
    else
    {
        HR_DEBUG("Ignoring reply packet from %s.", pTarget->name());

        --m_surplus_replies;
        gwbuf_free(pPacket);
    }
}

bool HintRouterSession::handleError(GWBUF* pMessage, mxs::Endpoint* pProblem, const mxs::Reply& pReply)
{
    HR_ENTRY();
    return false;
}

bool HintRouterSession::route_by_hint(GWBUF* pPacket, HINT* hint, bool print_errors)
{
    bool success = false;
    switch (hint->type)
    {
    case HINT_ROUTE_TO_MASTER:
        {
            bool master_ok = false;
            // The master server should be already known, but may have changed
            if (m_master && m_master->target()->is_master())
            {
                master_ok = true;
            }
            else
            {
                update_connections();
                if (m_master)
                {
                    master_ok = true;
                }
            }

            if (master_ok)
            {
                HR_DEBUG("Writing packet to master: '%s'.", m_master->target()->name());
                success = m_master->routeQuery(pPacket);
                if (success)
                {
                    m_router->m_routed_to_master++;
                }
                else
                {
                    HR_DEBUG("Write to master failed.");
                }
            }
            else if (print_errors)
            {
                MXS_ERROR("Hint suggests routing to master when no master connected.");
            }
        }
        break;

    case HINT_ROUTE_TO_SLAVE:
        success = route_to_slave(pPacket, print_errors);
        break;

    case HINT_ROUTE_TO_NAMED_SERVER:
        {
            string backend_name((hint->data) ? (const char*)(hint->data) : "");
            BackendMap::const_iterator iter = m_backends.find(backend_name);
            if (iter != m_backends.end())
            {
                HR_DEBUG("Writing packet to %s.", iter->second.server()->name());
                success = iter->second->routeQuery(pPacket);
                if (success)
                {
                    m_router->m_routed_to_named++;
                }
                else
                {
                    HR_DEBUG("Write failed.");
                }
            }
            else if (print_errors)
            {
                /* This shouldn't be possible with current setup as server names are
                 * checked on startup. With a different filter and the 'print_errors'
                 * on for the first call this is possible. */
                MXS_ERROR("Hint suggests routing to backend '%s' when no such backend connected.",
                          backend_name.c_str());
            }
        }
        break;

    case HINT_ROUTE_TO_ALL:
        {
            HR_DEBUG("Writing packet to %lu backends.", m_backends.size());
            BackendMap::size_type n_writes =
                std::count_if(m_backends.begin(), m_backends.end(), Writer(pPacket));
            if (n_writes != 0)
            {
                m_surplus_replies = n_writes - 1;
            }
            BackendMap::size_type size = m_backends.size();
            success = (n_writes == size);
            if (success)
            {
                gwbuf_free(pPacket);
                m_router->m_routed_to_all++;
            }
            else
            {
                HR_DEBUG("Write to all failed.");
                if (print_errors)
                {
                    MXS_ERROR("Write failed for '%lu' out of '%lu' backends.",
                              (size - n_writes),
                              size);
                }
            }
        }
        break;

    default:
        MXS_ERROR("Unsupported hint type '%d'", hint->type);
        break;
    }
    return success;
}

bool HintRouterSession::route_to_slave(GWBUF* pPacket, bool print_errors)
{
    bool success = false;
    // Find a valid slave
    size_type size = m_slaves.size();
    if (size)
    {
        size_type begin = m_n_routed_to_slave % size;
        size_type limit = begin + size;
        for (size_type curr = begin; curr != limit; curr++)
        {
            auto candidate = m_slaves.at(curr % size);
            if (candidate->target()->is_slave())
            {
                HR_DEBUG("Writing packet to slave: '%s'.", candidate->target()->name());
                success = candidate->routeQuery(pPacket);
                if (success)
                {
                    break;
                }
                else
                {
                    HR_DEBUG("Write to slave failed.");
                }
            }
        }
    }

    /* It is (in theory) possible, that none of the slaves in the slave-array are
     * working (or have been promoted to master) and the previous master is now
     * a slave. In this situation, re-arranging the dcb:s will help. */
    if (!success)
    {
        update_connections();
        size = m_slaves.size();
        if (size)
        {
            size_type begin = m_n_routed_to_slave % size;
            size_type limit = begin + size;
            for (size_type curr = begin; curr != limit; curr++)
            {
                auto candidate = m_slaves.at(curr % size);
                HR_DEBUG("Writing packet to slave: '%s'.", candidate->target()->name());
                success = candidate->routeQuery(pPacket);
                if (success)
                {
                    break;
                }
                else
                {
                    HR_DEBUG("Write to slave failed.");
                }
            }
        }
    }

    if (success)
    {
        m_router->m_routed_to_slave++;
        m_n_routed_to_slave++;
    }
    else if (print_errors)
    {
        if (!size)
        {
            MXS_ERROR("Hint suggests routing to slave when no slaves found.");
        }
        else
        {
            MXS_ERROR("Could not write to any of '%lu' slaves.", size);
        }
    }
    return success;
}

void HintRouterSession::update_connections()
{
    /* Attempt to rearrange the dcb:s in the session such that the master and
     * slave containers are correct again. Do not try to make new connections,
     * since those would not have the correct session state anyways. */
    m_master = nullptr;
    m_slaves.clear();

    for (BackendMap::const_iterator iter = m_backends.begin();
         iter != m_backends.end(); iter++)
    {
        auto server = iter->second->target();
        if (server->is_master())
        {
            if (!m_master)
            {
                m_master = iter->second;
            }
            else
            {
                MXS_WARNING("Found multiple master servers when updating connections.");
            }
        }
        else if (server->is_slave())
        {
            m_slaves.push_back(iter->second);
        }
    }
}
