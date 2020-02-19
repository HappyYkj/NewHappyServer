#pragma  once
#include "common/buffer.hpp"
#include "core/service.hpp"
#include "magic/lua_bind.h"

class lua_service : public service
{
public:
    static constexpr std::string_view LUA_PATH_STR = "/?.lua;";

#if TARGET_PLATFORM == PLATFORM_WINDOWS
    static constexpr std::string_view LUA_CPATH_STR = "/?.dll;";
#else
    static constexpr std::string_view LUA_CPATH_STR = "/?.so;";
#endif
    using lua_state_ptr_t = std::unique_ptr<lua_State, void(*)(lua_State*)>;

    using sol_function_t = sol::function;

    lua_service();

    ~lua_service();

    size_t memory_use();

    void set_callback(char c, sol_function_t f);

private:
    bool init(std::string_view config) override;

    void start() override;

    void exit() override;

    void destroy() override;

    void dispatch(message* msg) override;

    void on_timer(uint32_t timerid, bool remove) override;

    void error(const std::string& msg, bool initialized = true);

    static void* lalloc(void* ud, void* ptr, size_t osize, size_t nsize);

public:
    size_t mem = 0;
    size_t mem_limit = 0;
    size_t mem_report = 8 * 1024 * 1024;

private:
    sol::state lua_;
    sol_function_t start_;
    sol_function_t dispatch_;
    sol_function_t exit_;
    sol_function_t destroy_;
    sol_function_t on_timer_;
};
