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

#define MXS_MODULE_NAME "masking"
#include "maskingfilter.hh"

#include <maxscale/json_api.hh>
#include <maxscale/modulecmd.hh>
#include <maxscale/paths.h>
#include <maxscale/utils.h>

#include "maskingrules.hh"

using std::auto_ptr;
using std::string;

namespace
{

char VERSION_STRING[] = "V1.0.0";

/**
 * Implement "call command masking reload ..."
 *
 * @param pArgs  The arguments of the command.
 *
 * @return True, if the command was handled.
 */
bool masking_command_reload(const MODULECMD_ARG* pArgs, json_t** output)
{
    mxb_assert(pArgs->argc == 1);
    mxb_assert(MODULECMD_GET_TYPE(&pArgs->argv[0].type) == MODULECMD_ARG_FILTER);

    const MXS_FILTER_DEF* pFilterDef = pArgs->argv[0].value.filter;
    mxb_assert(pFilterDef);
    MaskingFilter* pFilter = reinterpret_cast<MaskingFilter*>(filter_def_get_instance(pFilterDef));

    bool rv = false;
    MXS_EXCEPTION_GUARD(rv = pFilter->reload());

    if (!rv)
    {
        modulecmd_set_error("Could not reload the rules. Check the log file "
                            "for more detailed information.");
    }

    return rv;
}
}

//
// Global symbols of the Module
//

extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static modulecmd_arg_type_t reload_argv[] =
    {
        {MODULECMD_ARG_FILTER | MODULECMD_ARG_NAME_MATCHES_DOMAIN, "Masking name"}
    };

    modulecmd_register_command(MXS_MODULE_NAME,
                               "reload",
                               MODULECMD_TYPE_ACTIVE,
                               masking_command_reload,
                               MXS_ARRAY_NELEMS(reload_argv),
                               reload_argv,
                               "Reload masking filter rules");

    MXS_NOTICE("Masking module %s initialized.", VERSION_STRING);

    typedef MaskingFilter::Config Config;

    static MXS_MODULE info =
    {
        MXS_MODULE_API_FILTER,
        MXS_MODULE_IN_DEVELOPMENT,
        MXS_FILTER_VERSION,
        "A masking filter that is capable of masking/obfuscating returned column values.",
        "V1.0.0",
        RCAP_TYPE_CONTIGUOUS_INPUT | RCAP_TYPE_CONTIGUOUS_OUTPUT,
        &MaskingFilter::s_object,
        NULL,   /* Process init. */
        NULL,   /* Process finish. */
        NULL,   /* Thread init. */
        NULL,   /* Thread finish. */
        {
            {
                Config::rules_name,
                MXS_MODULE_PARAM_STRING,
                NULL,
                MXS_MODULE_OPT_REQUIRED
            },
            {
                Config::warn_type_mismatch_name,
                MXS_MODULE_PARAM_ENUM,
                Config::warn_type_mismatch_default,
                MXS_MODULE_OPT_NONE,
                Config::warn_type_mismatch_values
            },
            {
                Config::large_payload_name,
                MXS_MODULE_PARAM_ENUM,
                Config::large_payload_default,
                MXS_MODULE_OPT_NONE,
                Config::large_payload_values
            },
            {
                Config::prevent_function_usage_name,
                MXS_MODULE_PARAM_BOOL,
                Config::prevent_function_usage_default,
                MXS_MODULE_OPT_NONE,
            },
            {
                Config::check_user_variables_name,
                MXS_MODULE_PARAM_BOOL,
                Config::check_user_variables_default,
                MXS_MODULE_OPT_NONE,
            },
            {
                Config::check_unions_name,
                MXS_MODULE_PARAM_BOOL,
                Config::check_unions_default,
                MXS_MODULE_OPT_NONE,
            },
            {
                Config::check_subqueries_name,
                MXS_MODULE_PARAM_BOOL,
                Config::check_subqueries_default,
                MXS_MODULE_OPT_NONE,
            },
            {
                Config::require_fully_parsed_name,
                MXS_MODULE_PARAM_BOOL,
                Config::require_fully_parsed_default,
                MXS_MODULE_OPT_NONE,
            },
            {
                Config::treat_string_arg_as_field_name,
                MXS_MODULE_PARAM_BOOL,
                Config::treat_string_arg_as_field_default,
                MXS_MODULE_OPT_NONE
            },
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}

//
// MaskingFilter
//

MaskingFilter::MaskingFilter(const Config& config, auto_ptr<MaskingRules> sRules)
    : m_config(config)
    , m_sRules(sRules.release())
{
    MXS_NOTICE("Masking filter [%s] created.", m_config.name().c_str());
}

MaskingFilter::~MaskingFilter()
{
}

// static
MaskingFilter* MaskingFilter::create(const char* zName, MXS_CONFIG_PARAMETER* pParams)
{
    MaskingFilter* pFilter = NULL;

    Config config(zName, pParams);

    auto_ptr<MaskingRules> sRules = MaskingRules::load(config.rules().c_str());

    if (sRules.get())
    {
        pFilter = new MaskingFilter(config, sRules);

        if (config.treat_string_arg_as_field())
        {
            QC_CACHE_PROPERTIES cache_properties;
            qc_get_cache_properties(&cache_properties);

            if (cache_properties.max_size != 0)
            {
                MXS_NOTICE("The parameter 'treat_string_arg_as_field' is enabled for %s, "
                           "disabling the query classifier cache.",
                           zName);

                cache_properties.max_size = 0;
                qc_set_cache_properties(&cache_properties);
            }
        }
    }

    return pFilter;
}


MaskingFilterSession* MaskingFilter::newSession(MXS_SESSION* pSession, SERVICE* pService)
{
    return MaskingFilterSession::create(pSession, pService, this);
}

// static
void MaskingFilter::diagnostics(DCB* pDcb)
{
}

// static
json_t* MaskingFilter::diagnostics_json() const
{
    return NULL;
}

// static
uint64_t MaskingFilter::getCapabilities()
{
    return RCAP_TYPE_NONE;
}

std::shared_ptr<MaskingRules> MaskingFilter::rules() const
{
    return m_sRules;
}

bool MaskingFilter::reload()
{
    bool rval = false;
    auto_ptr<MaskingRules> sRules = MaskingRules::load(m_config.rules().c_str());

    if (sRules.get())
    {
        MXS_NOTICE("Rules for masking filter '%s' were reloaded from '%s'.",
                   m_config.name().c_str(),
                   m_config.rules().c_str());

        m_sRules.reset(sRules.release());
        rval = true;
    }
    else
    {
        MXS_ERROR("Rules for masking filter '%s' could not be reloaded from '%s'.",
                  m_config.name().c_str(),
                  m_config.rules().c_str());
    }

    return rval;
}
