#pragma once
#include "router.h"

class server final
{
public:
    server();

    ~server();

    server(const server&) = delete;

    server& operator=(const server&) = delete;

    server(server&&) = delete;

    void init(uint8_t worker_num, const std::string& logpath);

    void run();

    void stop();

    logger* get_logger();

    router* get_router();

    state get_state();

    int64_t now();

    uint32_t service_count();

private:
    void wait();

private:
    std::atomic<state> state_;
    int64_t now_;
    std::vector<std::unique_ptr<worker>> workers_;
    logger logger_;
    router router_;
};
