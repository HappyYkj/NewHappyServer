#include "lua_service.h"
#include "lua_service_config.hpp"
#include "common/message.hpp"
#include "common/hash.hpp"
#include "core/server.h"
#include "core/worker.h"
#include "core/server_config.hpp"

void* lua_service::lalloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    lua_service* l = reinterpret_cast<lua_service*>(ud);
    size_t mem = l->mem;

    l->mem += nsize;

    if (ptr)
    {
        l->mem -= osize;
    }

    if (l->mem_limit != 0 && l->mem > l->mem_limit)
    {
        if (ptr == nullptr || nsize > osize)
        {
            CONSOLE_ERROR(l->get_logger(), "%s Memory error current %.2f M, limit %.2f M", l->name().data(), (float)(l->mem) / (1024 * 1024), (float)l->mem_limit / (1024 * 1024));
            l->mem = mem;
            return nullptr;
        }
    }

    if (l->mem > l->mem_report)
    {
        l->mem_report *= 2;
        CONSOLE_WARN(l->get_logger(), "%s Memory warning %.2f M", l->name().data(), (float)l->mem / (1024 * 1024));
    }

    if (nsize == 0)
    {
        free(ptr);
        return nullptr;
    }
    else
    {
        return realloc(ptr, nsize);
    }
}

lua_service::lua_service()
    : lua_(sol::default_at_panic, lalloc, this)
{
}

lua_service::~lua_service()
{
}

bool lua_service::init(std::string_view config)
{
    service_config_parser<lua_service> conf;
    if (!conf.parse(this, config))
    {
        CONSOLE_ERROR(get_logger(), "lua service init failed: parse config failed.");
        return false;
    }

    auto luafile = conf.get_value<std::string>("file");
    if (luafile.empty())
    {
        CONSOLE_ERROR(get_logger(), "lua service init failed: config does not provide lua file.");
        return false;
    }

    mem_limit = static_cast<size_t>(conf.get_value<int64_t>("memlimit"));

    lua_.open_libraries();
    sol::table module = lua_.create_table();
    lua_bind lua_bind(module);
    lua_bind.bind_service(this)
            .bind_log(get_logger(), id())
            .bind_util()
            .bind_message()
            .bind_timer(this);
    lua_bind::registerlib(lua_.lua_state(), "core", module);
    lua_bind::registerlib(lua_.lua_state(), "lfs",  luaopen_lfs);
    lua_bind::registerlib(lua_.lua_state(), "lluv", luaopen_lluv);
    lua_bind::registerlib(lua_.lua_state(), "json", luaopen_cjson);
    lua_bind::registerlib(lua_.lua_state(), "seri", luaopen_serialize);

    lua_bind::registerlib(lua_.lua_state(), "pb",        luaopen_pb);
    lua_bind::registerlib(lua_.lua_state(), "pb.io",     luaopen_pb_io);
    lua_bind::registerlib(lua_.lua_state(), "pb.buffer", luaopen_pb_buffer);
    lua_bind::registerlib(lua_.lua_state(), "pb.slice",  luaopen_pb_slice);
    lua_bind::registerlib(lua_.lua_state(), "pb.conv",   luaopen_pb_conv);
    lua_bind::registerlib(lua_.lua_state(), "pb.unsafe", luaopen_pb_unsafe);

    lua_bind::registerlib(lua_.lua_state(), "msgpack",      luaopen_cmsgpack);
    lua_bind::registerlib(lua_.lua_state(), "msgpack.safe", luaopen_cmsgpack_safe);
    lua_bind::registerlib(lua_.lua_state(), "luasql.mysql", luaopen_luasql_mysql);

    auto server_cfg = server_config_manger::instance().get_server_config();
    if (server_cfg != nullptr)
    {
        std::string path;
        path.append(".").append(LUA_PATH_STR);
        for (auto& v : server_cfg->path)
        {
            std::string strpath;
            strpath.append(v.data(), v.size());
            strpath.append(LUA_PATH_STR);
            path.append(strpath);
        }
        path.append(lua_["package"]["path"]);
        lua_["package"]["path"] = path;

        std::string cpath;
        cpath.append(".").append(LUA_CPATH_STR);
        for (auto& v : server_cfg->cpath)
        {
            std::string strpath;
            strpath.append(v.data(), v.size());
            strpath.append(LUA_CPATH_STR);
            cpath.append(strpath);
        }
        cpath.append(lua_["package"]["cpath"]);
        lua_["package"]["cpath"] = cpath;
    }

    // sol::protected_function_result call_result = lua_.script_file(luafile, sol::script_pass_on_error);
    sol::load_result load_result = lua_.load_file(luafile);
    if (!load_result.valid())
    {
        auto errmsg = sol::stack::get<std::string>(load_result.lua_state(), -1);
        CONSOLE_ERROR(get_logger(), "lua service init failed: %s.", errmsg.data());
        return false;
    }

    sol::object json = lua_.require("json", luaopen_cjson, false);
    sol::table tconfig = json.as<sol::table>().get<sol::function>("decode").call(config).get<sol::table>();
    sol::protected_function_result call_result = load_result.call(tconfig);
    if (!call_result.valid())
    {
        sol::error err = call_result;
        CONSOLE_ERROR(get_logger(), "lua service init failed: %s.", err.what());
        return false;
    }

    if (unique())
    {
        if (!router_->set_unique_service(name(), id()))
        {
            CONSOLE_ERROR(get_logger(), "lua service init failed: unique service name %s repeated.", name().data());
            return false;
        }
    }

    CONSOLE_INFO(get_logger(), "[WORKER %u] new service [%s:%X]", worker_->id(), name().data(), id());
    ok_ = true;
    return ok_;
}

void lua_service::start()
{
    if (is_start() || !is_ok())
    {
        return;
    }

    service::start();

    if (start_.valid())
    {
        auto result = start_();
        if (!result.valid())
        {
            sol::error err = result;
            CONSOLE_ERROR(get_logger(), "%s", err.what());
        }
    }
}

void lua_service::dispatch(message* msg)
{
    if (!is_ok())
    {
        return;
    }

    if (!dispatch_.valid())
    {
        CONSOLE_ERROR(get_logger(), "should initialize callbacks first.");
        return;
    }

    auto result = dispatch_(msg, msg->type());
    if (!result.valid())
    {
        sol::error err = result;
        if (msg->sessionid() >= 0 || msg->receiver() == 0) // socket mesage receiver==0
        {
            CONSOLE_ERROR(get_logger(), "%s dispatch:\n%s", name().data(), err.what());
            return;
        }

        msg->set_sessionid(-msg->sessionid());
        router_->response(msg->sender(), "lua_service::dispatch "sv, err.what(), msg->sessionid(), PTYPE_ERROR);
    }
}

void lua_service::on_timer(uint32_t timerid, bool remove)
{
    if (!is_ok())
    {
        return;
    }

    auto result = on_timer_(timerid, remove);
    if (!result.valid())
    {
        sol::error err = result;
        CONSOLE_ERROR(get_logger(), "%s", err.what());
    }
}

void lua_service::exit()
{
    if (!is_ok())
    {
        return;
    }

    if (exit_.valid())
    {
        auto result = exit_();
        if (!result.valid())
        {
            sol::error err = result;
            CONSOLE_ERROR(get_logger(), "%s", err.what());
        }
        return;
    }

    service::exit();
}

void lua_service::destroy()
{
    CONSOLE_INFO(get_logger(), "[WORKER %u] destroy service [%s:%X] ", worker_->id(), name().data(), id());

    if (!is_ok())
    {
        return;
    }

    if (destroy_.valid())
    {
        auto result = destroy_();
        if (!result.valid())
        {
            sol::error err = result;
            CONSOLE_ERROR(get_logger(), "%s", err.what());
        }
    }

    service::destroy();
}

void lua_service::error(const std::string & msg, bool initialized)
{
    std::string backtrace = lua_traceback(lua_.lua_state());
    CONSOLE_ERROR(get_logger(), "%s %s", name().data(), (msg + backtrace).data());
    
    if (initialized)
    {
        destroy();
        quit();
    }
    
    if (unique())
    {
        CONSOLE_ERROR(get_logger(), "unique service %s crashed, server will abort.", name().data());
        server_->stop();
    }
}

size_t lua_service::memory_use()
{
    return mem;
}

void lua_service::set_callback(char c, sol_function_t f)
{
    if (!f.valid())
    {
        CONSOLE_ERROR(get_logger(), "set initialize callbacks error.");
        return;
    }

    switch (c)
    {
        case 's':
        {
            start_ = f;
            break;
        }
        case 'm':
        {
            dispatch_ = f;
            break;
        }
        case 'e':
        {
            exit_ = f;
            break;
        }
        case 'd':
        {
            destroy_ = f;
            break;
        }
        case 't':
        {
            on_timer_ = f;
            break;
        }
    }
}
