#pragma comment(lib, "lua.lib")

#include <mutex>
#include "common/file.hpp"
#include "common/string.hpp"
#include "common/platform.hpp"
#include "core/server.h"
#include "core/server_config.hpp"
#include "services/lua_service.h"

std::mutex g_objExitMutex;
std::condition_variable g_objExitCond;
static std::weak_ptr<server> wk_server;

#if TARGET_PLATFORM == PLATFORM_WINDOWS
static BOOL WINAPI ConsoleHandlerRoutine(DWORD dwCtrlType)
{
    auto svr = wk_server.lock();
    if (nullptr == svr)
    {
        return TRUE;
    }

    switch (dwCtrlType)
    {
    case CTRL_C_EVENT:
        svr->stop();
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT: // atmost 10 second,will force closed by system
        svr->stop();
        while (svr->get_state() != state::exited)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // std::this_thread::yield();
        }
        return TRUE;
    default:
        break;
    }
    return FALSE;
}
#else
static void signal_handler(int signal)
{
    auto svr = wk_server.lock();
    if (nullptr == svr)
    {
        return;
    }

    switch (signal)
    {
    case SIGTERM:
        CONSOLE_ERROR(svr->logger(), "RECV SIGTERM SIGNAL");
        svr->stop();
        break;
    case SIGINT:
        CONSOLE_ERROR(svr->logger(), "RECV SIGINT SIGNAL");
        svr->stop();
        break;
    default:
        break;
    }
}
#endif

void usage(void)
{
    printf("Usage:\n");
    printf("        happy [-c filename] [-r server-id] [-f lua-filename]\n");
    printf("The options are:\n");
    printf("        -c          set configuration file (default: config.json). will change current working directory to configuration file's path.\n");
    printf("        -r          set server id to run (default: 1).\n");
    printf("        -f          run a lua file. will change current working directory to lua file's path.\n");
    printf("Examples:\n");
    printf("        happy -c config.json\n");
    printf("        happy -c config.json -r 1\n");
    printf("        happy -r 1\n");
    printf("        happy -f aoi_example.lua\n");
}

static void register_signal()
{
#if TARGET_PLATFORM == PLATFORM_WINDOWS
    SetConsoleCtrlHandler(ConsoleHandlerRoutine, TRUE);
#else
    std::signal(SIGHUP,  SIG_IGN);
    std::signal(SIGQUIT, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif
}

int main(int argc, char* argv[])
{
    register_signal();

    directory::working_directory = directory::current_directory();

    int32_t sid = 1;                        // default start server 1
    std::string conf = "config.json";       // default config
    std::string service_file = "main.lua";  // default file

    for (int i = 1; i < argc; ++i)
    {
        bool lastarg = i == (argc - 1);
        std::string_view v{ argv[i] };
        if ((v == "-h" || v == "--help") && !lastarg)
        {
            usage();
            return -1;
        }
        else if ((v == "-c" || v == "--config") && !lastarg)
        {
            conf = argv[++i];
            if (!fs::exists(conf))
            {
                usage();
                return -1;
            }
        }
        else if ((v == "-r" || v == "--run") && !lastarg)
        {
            sid = string_convert<int32_t>(argv[++i]);
        }
        else if ((v == "-f" || v == "--file") && !lastarg)
        {
            service_file = argv[++i];
            if (fs::path(service_file).extension() != ".lua")
            {
                printf("service file must be a lua script.\n");
                usage();
                return -1;
            }
        }
        else
        {
            usage();
            return -1;
        }
    }

    system("chcp 65001");

    server_config_manger& scfg = server_config_manger::instance();
    if (service_file.empty())
    {
        std::string clib_dir = directory::working_directory.append("clib").string();
        replace(clib_dir, "\\", "/");

        std::string lualib_dir = directory::working_directory.append("lualib").string();
        replace(lualib_dir, "\\", "/");

        scfg.parse(format(R"([{"sid":1,"name":"test_#sid","cpath":["../clib"],"path":["../lualib"],"services":[{"name":"%s","file":"%s","cpath":["%s"],"path":["%s"]}]}])",
            fs::path(service_file).stem().string().data(),
            fs::path(service_file).filename().string().data(),
            clib_dir.data(),
            lualib_dir.data()), sid);

        printf("use clib search path: %s\n", clib_dir.data());
        printf("use lualib search path: %s\n", lualib_dir.data());

        fs::current_path(fs::absolute(fs::path(service_file)).parent_path());
        directory::working_directory = fs::current_path();
    }
    else
    {
        if (!directory::exists(conf))
        {
            printf("can not found config file: '%s'.\n", conf.data());
            return 1;
        }

        if (!scfg.parse(file::read_all(conf, std::ios::binary | std::ios::in), sid))
        {
            printf("can not parse config file.\n");
            return 2;
        }

        fs::current_path(fs::absolute(fs::path(conf)).parent_path());
        directory::working_directory = fs::current_path();
    }

    auto c = scfg.find(sid);
    if (c == nullptr)
    {
        printf("config for sid=%d not found.\n", sid);
        return 3;
    }

    std::shared_ptr<server> server_ = std::make_shared<server>();
    wk_server = server_;

    auto router_ = server_->get_router();
    router_->set_env("SID", std::to_string(c->sid));
    router_->set_env("SERVER_NAME", c->name);
    router_->set_env("INNER_HOST", c->inner_host);
    router_->set_env("OUTER_HOST", c->outer_host);
    router_->set_env("THREAD_NUM", std::to_string(c->thread));
    router_->set_env("CONFIG", scfg.config());
    router_->register_service("lua", []()->service_ptr_t {
        return std::make_unique<lua_service>();
    });

    server_->init(c->thread, c->log);
    server_->get_logger()->set_level(c->loglevel);

    for (auto& s : c->services)
    {
        router_->new_service(s.type, s.config, s.unique, s.threadid, 0, 0);
    }

    // wait all configured service is created
    while (server_->get_state() == state::init &&
           server_->service_count() < c->services.size())
    {
        std::this_thread::yield();
    }

    // then call services's start
    server_->run();

    return 0;
}

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
