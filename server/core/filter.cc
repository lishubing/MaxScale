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

/**
 * @file filter.c  - A representation of a filter within MaxScale.
 */

#include "internal/filter.hh"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <maxbase/alloc.h>
#include <maxscale/paths.h>
#include <maxscale/session.hh>
#include <maxscale/service.hh>
#include <maxscale/filter.hh>
#include <maxscale/json_api.hh>

#include "internal/config.hh"
#include "internal/modules.hh"
#include "internal/service.hh"

using std::string;
using std::set;
using Guard = std::lock_guard<std::mutex>;

using namespace maxscale;

static struct
{
    std::mutex              lock;
    std::vector<SFilterDef> filters;
} this_unit;

/**
 * Allocate a new filter
 *
 * @param name   The filter name
 * @param module The module to load
 * @param params Module parameters
 *
 * @return The newly created filter or NULL if an error occurred
 */
SFilterDef filter_alloc(const char* name, const char* module, MXS_CONFIG_PARAMETER* params)
{
    MXS_FILTER_OBJECT* object = (MXS_FILTER_OBJECT*)load_module(module, MODULE_FILTER);

    if (object == NULL)
    {
        MXS_ERROR("Failed to load filter module '%s'.", module);
        return NULL;
    }

    if (!object->clientReply)
    {
        MXS_ERROR("Filter '%s' does not implement the clientReply entry point.", module);
        return NULL;
    }

    MXS_FILTER* instance = object->createInstance(name, params);

    if (instance == NULL)
    {
        MXS_ERROR("Failed to create filter '%s' instance.", name);
        return NULL;
    }

    SFilterDef filter(new(std::nothrow) FilterDef(name, module, object, instance, params));

    if (filter)
    {
        Guard guard(this_unit.lock);
        this_unit.filters.push_back(filter);
    }
    else
    {
        object->destroyInstance(instance);
    }

    return filter;
}

FilterDef::FilterDef(std::string name,
                     std::string module,
                     MXS_FILTER_OBJECT* object,
                     MXS_FILTER* instance,
                     MXS_CONFIG_PARAMETER* params)
    : name(name)
    , module(module)
    , parameters(*params)
    , filter(instance)
    , obj(object)
{
}

FilterDef::~FilterDef()
{
    if (obj->destroyInstance && filter)
    {
        obj->destroyInstance(filter);
    }
}

/**
 * Free the specified filter
 *
 * @param filter        The filter to free
 */
void filter_free(const SFilterDef& filter)
{
    mxb_assert(filter);
    // Removing the filter from the list will trigger deletion once it's no longer in use
    Guard guard(this_unit.lock);
    auto it = std::remove(this_unit.filters.begin(), this_unit.filters.end(), filter);
    mxb_assert(it != this_unit.filters.end());
    this_unit.filters.erase(it);
}

SFilterDef filter_find(const char* name)
{
    Guard guard(this_unit.lock);

    for (const auto& filter : this_unit.filters)
    {
        if (filter->name == name)
        {
            return filter;
        }
    }

    return SFilterDef();
}

bool filter_can_be_destroyed(const SFilterDef& filter)
{
    mxb_assert(filter);
    return !service_filter_in_use(filter);
}

void filter_destroy(const SFilterDef& filter)
{
    mxb_assert(filter);
    mxb_assert(filter_can_be_destroyed(filter));
    filter_free(filter);
}

void filter_destroy_instances()
{
    Guard guard(this_unit.lock);
    this_unit.filters.clear();
}

const char* filter_def_get_name(const MXS_FILTER_DEF* filter_def)
{
    const FilterDef* filter = static_cast<const FilterDef*>(filter_def);
    mxb_assert(filter);
    return filter->name.c_str();
}

const char* filter_def_get_module_name(const MXS_FILTER_DEF* filter_def)
{
    const FilterDef* filter = static_cast<const FilterDef*>(filter_def);
    mxb_assert(filter);
    return filter->module.c_str();
}

MXS_FILTER* filter_def_get_instance(const MXS_FILTER_DEF* filter_def)
{
    const FilterDef* filter = static_cast<const FilterDef*>(filter_def);
    mxb_assert(filter);
    return filter->filter;
}

/**
 * Check a parameter to see if it is a standard filter parameter
 *
 * @param name  Parameter name to check
 */
int filter_standard_parameter(const char* name)
{
    if (strcmp(name, "type") == 0 || strcmp(name, "module") == 0)
    {
        return 1;
    }
    return 0;
}

/**
 * Print all filters to a DCB
 *
 * Designed to be called within a debugger session in order
 * to display all active filters within MaxScale
 */
void dprintAllFilters(DCB* dcb)
{
    Guard guard(this_unit.lock);

    for (const auto& ptr : this_unit.filters)
    {
        dcb_printf(dcb, "FilterDef %p (%s)\n", ptr.get(), ptr->name.c_str());
        dcb_printf(dcb, "\tModule:      %s\n", ptr->module.c_str());
        if (ptr->obj && ptr->filter)
        {
            ptr->obj->diagnostics(ptr->filter, NULL, dcb);
        }
        else
        {
            dcb_printf(dcb, "\tModule not loaded.\n");
        }
    }
}

/**
 * Print filter details to a DCB
 *
 * Designed to be called within a debug CLI in order
 * to display all active filters in MaxScale
 */
void dprintFilter(DCB* dcb, const FilterDef* filter)
{
    mxb_assert(filter);
    dcb_printf(dcb, "FilterDef %p (%s)\n", filter, filter->name.c_str());
    dcb_printf(dcb, "\tModule:      %s\n", filter->module.c_str());
    if (filter->obj && filter->filter)
    {
        filter->obj->diagnostics(filter->filter, NULL, dcb);
    }
}

/**
 * List all filters in a tabular form to a DCB
 *
 */
void dListFilters(DCB* dcb)
{
    Guard guard(this_unit.lock);

    if (!this_unit.filters.empty())
    {
        dcb_printf(dcb, "FilterDefs\n");
        dcb_printf(dcb, "--------------------+-----------------+----------------------------------------\n");
        dcb_printf(dcb,
                   "%-19s | %-15s | Options\n",
                   "FilterDef",
                   "Module");
        dcb_printf(dcb, "--------------------+-----------------+----------------------------------------\n");

        for (const auto& ptr : this_unit.filters)
        {
            dcb_printf(dcb,
                       "%-19s | %-15s | ",
                       ptr->name.c_str(),
                       ptr->module.c_str());
            dcb_printf(dcb, "\n");
        }

        dcb_printf(dcb,
                   "--------------------+-----------------+----------------------------------------\n\n");
    }
}

json_t* filter_parameters_to_json(const SFilterDef& filter)
{
    mxb_assert(filter);
    json_t* rval = json_object();

    /** Add custom module parameters */
    const MXS_MODULE* mod = get_module(filter->module.c_str(), MODULE_FILTER);
    config_add_module_params_json(&filter->parameters,
                                  {CN_TYPE, CN_MODULE},
                                  config_filter_params,
                                  mod->parameters,
                                  rval);

    return rval;
}

json_t* filter_json_data(const SFilterDef& filter, const char* host)
{
    mxb_assert(filter);
    json_t* rval = json_object();

    json_object_set_new(rval, CN_ID, json_string(filter->name.c_str()));
    json_object_set_new(rval, CN_TYPE, json_string(CN_FILTERS));

    json_t* attr = json_object();

    json_object_set_new(attr, CN_MODULE, json_string(filter->module.c_str()));
    json_object_set_new(attr, CN_PARAMETERS, filter_parameters_to_json(filter));

    if (filter->obj && filter->filter && filter->obj->diagnostics_json)
    {
        json_t* diag = filter->obj->diagnostics_json(filter->filter, NULL);

        if (diag)
        {
            json_object_set_new(attr, CN_FILTER_DIAGNOSTICS, diag);
        }
    }

    /** Store relationships to other objects */
    json_t* rel = json_object();
    json_object_set_new(rel, CN_SERVICES, service_relations_to_filter(filter, host));

    json_object_set_new(rval, CN_RELATIONSHIPS, rel);
    json_object_set_new(rval, CN_ATTRIBUTES, attr);
    json_object_set_new(rval, CN_LINKS, mxs_json_self_link(host, CN_FILTERS, filter->name.c_str()));

    return rval;
}

json_t* filter_to_json(const SFilterDef& filter, const char* host)
{
    mxb_assert(filter);
    string self = MXS_JSON_API_FILTERS;
    self += filter->name;
    return mxs_json_resource(host, self.c_str(), filter_json_data(filter, host));
}

json_t* filter_list_to_json(const char* host)
{
    json_t* rval = json_array();

    Guard guard(this_unit.lock);

    for (const auto& f : this_unit.filters)
    {
        json_t* json = filter_json_data(f, host);

        if (json)
        {
            json_array_append_new(rval, json);
        }
    }

    return mxs_json_resource(host, MXS_JSON_API_FILTERS, rval);
}

namespace maxscale
{

//
// FilterSession
//

FilterSession::FilterSession(MXS_SESSION* pSession, SERVICE* pService)
    : m_pSession(pSession)
    , m_pService(pService)
{
}

FilterSession::~FilterSession()
{
}

void FilterSession::close()
{
}

void FilterSession::setDownstream(const Downstream& down)
{
    m_down = down;
}

void FilterSession::setUpstream(const Upstream& up)
{
    m_up = up;
}

int FilterSession::routeQuery(GWBUF* pPacket)
{
    return m_down.routeQuery(pPacket);
}

int FilterSession::clientReply(GWBUF* pPacket, const mxs::ReplyRoute& down, const mxs::Reply& reply)
{
    return m_up.clientReply(pPacket, down, reply);
}

void FilterSession::diagnostics(DCB* pDcb)
{
}

json_t* FilterSession::diagnostics_json() const
{
    return NULL;
}
}

static bool create_filter_config(const SFilterDef& filter, const char* filename)
{
    mxb_assert(filter);
    int file = open(filename, O_EXCL | O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (file == -1)
    {
        MXS_ERROR("Failed to open file '%s' when serializing filter '%s': %d, %s",
                  filename,
                  filter->name.c_str(),
                  errno,
                  mxs_strerror(errno));
        return false;
    }

    Guard guard(filter->lock);
    const MXS_MODULE* mod = get_module(filter->module.c_str(), NULL);
    mxb_assert(mod);

    string config_str = generate_config_string(filter->name, filter->parameters,
                                               config_filter_params, mod->parameters);
    if (dprintf(file, "%s", config_str.c_str()) == -1)
    {
        MXS_ERROR("Could not write serialized configuration to file '%s': %d, %s",
                  filename, errno, mxs_strerror(errno));
    }
    close(file);
    return true;
}

bool filter_serialize(const SFilterDef& filter)
{
    mxb_assert(filter);
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename,
             sizeof(filename),
             "%s/%s.cnf.tmp",
             get_config_persistdir(),
             filter->name.c_str());

    if (unlink(filename) == -1 && errno != ENOENT)
    {
        MXS_ERROR("Failed to remove temporary filter configuration at '%s': %d, %s",
                  filename,
                  errno,
                  mxs_strerror(errno));
    }
    else if (create_filter_config(filter, filename))
    {
        char final_filename[PATH_MAX];
        strcpy(final_filename, filename);

        char* dot = strrchr(final_filename, '.');
        mxb_assert(dot);
        *dot = '\0';

        if (rename(filename, final_filename) == 0)
        {
            rval = true;
        }
        else
        {
            MXS_ERROR("Failed to rename temporary filter configuration at '%s': %d, %s",
                      filename,
                      errno,
                      mxs_strerror(errno));
        }
    }

    return rval;
}
