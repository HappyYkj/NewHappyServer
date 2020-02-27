#pragma once

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <string>
#include "common/noncopyable.hpp"
#include "sol.hpp"

class logger;
class server;
class lua_service;

class lua_bind :public noncopyable
{
public:
    explicit lua_bind(sol::table& lua_);
    ~lua_bind();

    const lua_bind& bind_timer(lua_service* services) const;

    const lua_bind& bind_util() const;

    const lua_bind& bind_log(logger* log, uint32_t serviceid = 0) const;

    const lua_bind& bind_message() const;

    const lua_bind& bind_service(lua_service* service) const;

    static void registerlib(lua_State* state, const char *name, lua_CFunction function);

    static void registerlib(lua_State* state, const char *name, const sol::table& module);

private:
    sol::table& lua;
};

const char* lua_traceback(lua_State* _state);

extern "C"
{
    int luaopen_lfs(lua_State* L);
    int luaopen_cjson(lua_State* L);
    int luaopen_lluv(lua_State* L);
    int luaopen_serialize(lua_State* L);

    int luaopen_pb(lua_State* L);
    int luaopen_pb_io(lua_State* L);
    int luaopen_pb_buffer(lua_State* L);
    int luaopen_pb_slice(lua_State* L);
    int luaopen_pb_conv(lua_State* L);
    int luaopen_pb_unsafe(lua_State* L);
}
