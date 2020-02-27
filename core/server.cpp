#include "server.h"
#include "worker.h"

server::server()
    : state_(state::unknown)
    , now_(0)
    , workers_()
    , logger_()
    , router_(workers_, &logger_)
{
}

server::~server()
{
    stop();
    wait();
}

void server::init(uint8_t worker_num, const std::string& logpath)
{
    worker_num = (worker_num <= 0) ? 1 : worker_num;

    get_logger()->init(logpath);

    router_.set_server(this);

    CONSOLE_INFO(get_logger(), "INIT with %ud workers.", worker_num);

    for (uint8_t idx = 0; idx < worker_num; idx++)
    {
        workers_.emplace_back(std::make_unique<worker>(this, &router_, idx + 1));
    }

    for (auto& worker : workers_)
    {
        worker->init();
    }

    state_.store(state::init, std::memory_order_release);
}

void server::run()
{
    if (0 == workers_.size())
    {
        printf("should run server::init first!\r\n");
        return;
    }

    state_.store(state::ready, std::memory_order_release);

    for (auto& worker : workers_)
    {
        worker->start();
    }

    int64_t previous_tick = time::now();
    int64_t sleep_duration = 0;
    while (true)
    {
        now_ = time::now();
        auto diff = now_ - previous_tick;
        diff = (diff < 0) ? 0 : diff;
        previous_tick = now_;

        size_t stoped_worker_num = 0;

        for (const auto& worker : workers_)
        {
            if (worker->stoped())
            {
                stoped_worker_num++;
            }
            worker->update();
        }

        if (stoped_worker_num == workers_.size())
        {
            break;
        }

        if (diff <= UPDATE_INTERVAL + sleep_duration)
        {
            sleep_duration = UPDATE_INTERVAL + sleep_duration - diff;
            thread_sleep(sleep_duration);
        }
        else
        {
            sleep_duration = 0;
        }
    }
    wait();
}

void server::stop()
{
    auto s = state_.exchange(state::stopping, std::memory_order_acquire);
    if (s > state::ready)
    {
        return;
    }

    for (const auto& worker : workers_)
    {
        worker->stop();
    }
}

logger* server::get_logger()
{
    return &logger_;
}

router* server::get_router()
{
    return &router_;
}

void server::wait()
{
    for (auto iter = workers_.rbegin(); iter != workers_.rend(); ++iter)
    {
        (*iter)->wait();
    }
    CONSOLE_INFO(get_logger(), "STOP");
    logger_.wait();
    state_.store(state::exited, std::memory_order_release);
}

state server::get_state()
{
    return state_.load();
}

int64_t server::now()
{
    return now_;
}

uint32_t server::service_count()
{
    uint32_t count = 0;
    for (const auto& worker : workers_)
    {
        count += worker->count_.load(std::memory_order_acquire);
    }
    return count;
}
