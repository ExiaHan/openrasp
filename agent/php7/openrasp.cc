/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "openrasp.h"
#include "openrasp_ini.h"

extern "C"
{
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
}
#include "openrasp_log.h"
#include "openrasp_v8.h"
#include "openrasp_hook.h"
#include "openrasp_inject.h"
#include "openrasp_security_policy.h"
#include "openrasp_fswatch.h"
#include <new>
#include "openrasp_shared_alloc.h"
#include "agent/shared_config_manager.h"
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
#include "agent/openrasp_agent_manager.h"
#endif

using openrasp::OpenraspConfig;

ZEND_DECLARE_MODULE_GLOBALS(openrasp);

bool is_initialized = false;
bool remote_active = false;
static bool make_openrasp_root_dir();
static bool current_sapi_supported();
static std::string get_config_abs_path(OpenraspConfig::FromType type);
static bool update_config(openrasp::ConfigHolder *config, OpenraspConfig::FromType type = OpenraspConfig::FromType::kIni);

PHP_INI_BEGIN()
PHP_INI_ENTRY1("openrasp.root_dir", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.root_dir)
#ifdef HAVE_GETTEXT
PHP_INI_ENTRY1("openrasp.locale", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.locale)
#endif
PHP_INI_ENTRY1("openrasp.backend_url", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.backend_url)
PHP_INI_ENTRY1("openrasp.app_id", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.app_id)
PHP_INI_ENTRY1("openrasp.app_secret", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.app_secret)
PHP_INI_ENTRY1("openrasp.plugin_update_enable", "on", PHP_INI_SYSTEM, OnUpdateOpenraspBool, &openrasp_ini.plugin_update_enable)
PHP_INI_ENTRY1("openrasp.remote_management_enable", "off", PHP_INI_SYSTEM, OnUpdateOpenraspBool, &openrasp_ini.remote_management_enable)
PHP_INI_END()

PHP_GINIT_FUNCTION(openrasp)
{
#if defined(COMPILE_DL_JSON) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
#ifdef ZTS
    new (openrasp_globals) _zend_openrasp_globals;
#endif
}

PHP_GSHUTDOWN_FUNCTION(openrasp)
{
#ifdef ZTS
    openrasp_globals->~_zend_openrasp_globals();
#endif
}

PHP_MINIT_FUNCTION(openrasp)
{
    ZEND_INIT_MODULE_GLOBALS(openrasp, PHP_GINIT(openrasp), PHP_GSHUTDOWN(openrasp));
    REGISTER_INI_ENTRIES();
    if (!current_sapi_supported())
    {
        return SUCCESS;
    }
    if (!make_openrasp_root_dir())
    {
        return SUCCESS;
    }
    openrasp::scm.reset(new openrasp::SharedConfigManager());
    if (!openrasp::scm->startup())
    {
        openrasp_error(E_WARNING, RUNTIME_ERROR, _("Fail to startup SharedConfigManager."));
        return SUCCESS;
    }

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (check_sapi_need_alloc_shm() && openrasp_ini.remote_management_enable)
    {
        openrasp::oam.reset(new openrasp::OpenraspAgentManager());
        if (!openrasp::oam->verify_ini_correct())
        {
            return SUCCESS;
        }
        remote_active = true;
    }
#endif
    if (!remote_active)
    {
        update_config(&OPENRASP_G(config));
    }

    if (PHP_MINIT(openrasp_log)(INIT_FUNC_ARGS_PASSTHRU) == FAILURE)
    {
        return SUCCESS;
    }
    if (PHP_MINIT(openrasp_v8)(INIT_FUNC_ARGS_PASSTHRU) == FAILURE)
    {
        return SUCCESS;
    }
    int result;
    result = PHP_MINIT(openrasp_hook)(INIT_FUNC_ARGS_PASSTHRU);
    result = PHP_MINIT(openrasp_inject)(INIT_FUNC_ARGS_PASSTHRU);

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (openrasp::oam)
    {
        openrasp::oam->startup();
    }
#endif
    if (!remote_active)
    {
        std::string config_file_path = get_config_abs_path(OpenraspConfig::FromType::kIni);
        std::string conf_contents;
        if (get_entire_file_content(config_file_path.c_str(), conf_contents))
        {
            openrasp::OpenraspConfig openrasp_config(conf_contents, OpenraspConfig::FromType::kIni);
            openrasp::scm->build_check_type_white_array(openrasp_config);
            OpenRASPActionType callable_action = string_to_action(openrasp_config.Get<std::string>("callable.action"));
            OpenRASPActionType webshell_eval_action = string_to_action(openrasp_config.Get<std::string>("webshell_eval.action"));
            OpenRASPActionType webshell_command_action = string_to_action(openrasp_config.Get<std::string>("webshell_command.action"));
            OpenRASPActionType webshell_file_put_contents_action = string_to_action(openrasp_config.Get<std::string>("webshell_file_put_contents.action"));
            openrasp::scm->set_buildin_check_action(callable_action,
                                                    webshell_eval_action,
                                                    webshell_command_action,
                                                    webshell_file_put_contents_action);
            result = PHP_MINIT(openrasp_fswatch)(INIT_FUNC_ARGS_PASSTHRU);
        }
    }

    result = PHP_MINIT(openrasp_security_policy)(INIT_FUNC_ARGS_PASSTHRU);
    is_initialized = true;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        if (!remote_active)
        {
            result = PHP_MSHUTDOWN(openrasp_fswatch)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        }
        result = PHP_MSHUTDOWN(openrasp_inject)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_hook)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_v8)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_log)(SHUTDOWN_FUNC_ARGS_PASSTHRU);

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
        if (openrasp::oam)
        {
            openrasp::oam->shutdown();
        }
#endif
        openrasp::scm->shutdown();
        remote_active = false;
        is_initialized = false;
    }
    UNREGISTER_INI_ENTRIES();
    ZEND_SHUTDOWN_MODULE_GLOBALS(openrasp, PHP_GSHUTDOWN(openrasp));
    return SUCCESS;
}

PHP_RINIT_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        long config_last_update = openrasp::scm->get_config_last_update();
        if (config_last_update && config_last_update > OPENRASP_G(config).GetLatestUpdateTime())
        {
            if (update_config(&OPENRASP_G(config), OpenraspConfig::FromType::kJson))
            {
                OPENRASP_G(config).SetLatestUpdateTime(config_last_update);
            }
            else
            {
                openrasp_error(E_WARNING, CONFIG_ERROR, _("Fail to load new config."));
            }
        }
        // openrasp_inject must be called before openrasp_log cuz of request_id
        result = PHP_RINIT(openrasp_inject)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_log)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_hook)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_v8)(INIT_FUNC_ARGS_PASSTHRU);
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        result = PHP_RSHUTDOWN(openrasp_log)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_RSHUTDOWN(openrasp_inject)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
    }
    return SUCCESS;
}

PHP_MINFO_FUNCTION(openrasp)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Status", is_initialized ? "Protected" : "Unprotected, Initialization Failed");
#ifdef OPENRASP_COMMIT_ID
    php_info_print_table_row(2, "Version", OPENRASP_COMMIT_ID);
#else
    php_info_print_table_row(2, "Version", "");
#endif
    php_info_print_table_row(2, "V8 Version", ZEND_TOSTR(V8_MAJOR_VERSION) "." ZEND_TOSTR(V8_MINOR_VERSION));
    php_info_print_table_row(2, "Antlr Version", "4.7.1 (JavaScript Runtime)");
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (openrasp::oam)
    {
        php_info_print_table_row(2, "Plugin Version",
                                 openrasp::oam->agent_ctrl_block
                                     ? openrasp::oam->agent_ctrl_block->get_plugin_version()
                                     : "");
    }
#endif
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

/**  module depends
 */
#if ZEND_MODULE_API_NO >= 20050922
zend_module_dep openrasp_deps[] = {
    ZEND_MOD_REQUIRED("standard")
        ZEND_MOD_REQUIRED("json")
            ZEND_MOD_CONFLICTS("xdebug")
                ZEND_MOD_END};
#endif

zend_module_entry openrasp_module_entry = {
#if ZEND_MODULE_API_NO >= 20050922
    STANDARD_MODULE_HEADER_EX,
    NULL,
    openrasp_deps,
#else
    STANDARD_MODULE_HEADER,
#endif
    "openrasp",
    NULL,
    PHP_MINIT(openrasp),
    PHP_MSHUTDOWN(openrasp),
    PHP_RINIT(openrasp),
    PHP_RSHUTDOWN(openrasp),
    PHP_MINFO(openrasp),
    PHP_OPENRASP_VERSION,
    STANDARD_MODULE_PROPERTIES};

#ifdef COMPILE_DL_OPENRASP
ZEND_GET_MODULE(openrasp)
#endif

static bool make_openrasp_root_dir()
{
    char *path = openrasp_ini.root_dir;
    if (!path)
    {
        openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.root_dir must not be an empty path"));
        return false;
    }
    if (!IS_ABSOLUTE_PATH(path, strlen(path)))
    {
        openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.root_dir must not be a relative path"));
        return false;
    }
    path = expand_filepath(path, nullptr);
    if (!path || strnlen(path, 2) == 1)
    {
        openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.root_dir must not be a root path"));
        efree(path);
        return false;
    }
    std::string root_dir(path);
    efree(path);
    std::vector<std::string> sub_dir_list{"assets", "conf", "logs", "plugins", "locale"};
    for (auto dir : sub_dir_list)
    {
        std::string path(root_dir + DEFAULT_SLASH + dir);
        if (!recursive_mkdir(path.c_str(), path.length(), 0777))
        {
            openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.root_dir must be a writable path"));
            return false;
        }
    }
#ifdef HAVE_GETTEXT
    if (nullptr != setlocale(LC_ALL, (nullptr == openrasp_ini.locale || strcmp(openrasp_ini.locale, "") == 0) ? "C" : openrasp_ini.locale))
    {
        std::string locale_path(root_dir + DEFAULT_SLASH + "locale" + DEFAULT_SLASH);
        if (!bindtextdomain(GETTEXT_PACKAGE, locale_path.c_str()))
        {
            openrasp_error(E_WARNING, CONFIG_ERROR, _("Fail to bindtextdomain - %s"), strerror(errno));
        }
        if (!textdomain(GETTEXT_PACKAGE))
        {
            openrasp_error(E_WARNING, CONFIG_ERROR, _("Fail to textdomain - %s"), strerror(errno));
        }
    }
    else
    {
        openrasp_error(E_WARNING, CONFIG_ERROR, _("Unable to set OpenRASP locale to '%s'"), openrasp_ini.locale);
    }
#endif
    return true;
}

static std::string get_config_abs_path(OpenraspConfig::FromType type)
{
    std::string filename;
    switch (type)
    {
    case OpenraspConfig::FromType::kIni:
        filename = "openrasp.ini";
        break;
    case OpenraspConfig::FromType::kJson:
        filename = "cloud-config.json";
        break;
    default:
        break;
    }
    return std::string(openrasp_ini.root_dir) +
           DEFAULT_SLASH +
           "conf" +
           DEFAULT_SLASH + filename;
}

static bool update_config(openrasp::ConfigHolder *config, OpenraspConfig::FromType type)
{
    if (nullptr != openrasp_ini.root_dir && strcmp(openrasp_ini.root_dir, "") != 0)
    {
        std::string config_file_path = get_config_abs_path(type);
        std::string conf_contents;
        if (get_entire_file_content(config_file_path.c_str(), conf_contents))
        {
            openrasp::OpenraspConfig openrasp_config(conf_contents, type);
            return config->update(&openrasp_config);
        }
    }
    return false;
}

static bool current_sapi_supported()
{
    const static std::set<std::string> supported_sapis =
        {
            "cli",
            "cli-server",
            "cgi-fcgi",
            "fpm-fcgi",
            "apache2handler"};
    auto iter = supported_sapis.find(std::string(sapi_module.name));
    if (iter == supported_sapis.end())
    {
        openrasp_error(E_WARNING, CONFIG_ERROR, _("Unsupported SAPI: %s."), sapi_module.name);
        return false;
    }
    return true;
}