#include "worker.h"
#include "common/time.hpp"
#include "common/string.hpp"
#include "common/hash.hpp"
#include "common/message.hpp"
#include "common/logger.hpp"
#include "server.h"
#include "service.hpp"

worker::worker(server* server, router* router, uint8_t id)
    : server_(server)
    , router_(router)
    , workerid_(id)
{
}

worker::~worker()
{
}

void worker::init()
{
    timer_.set_now_func([this]() {
        return server_->now();
    });

    timer_.set_on_timer([this](timer_id_t timerid, uint32_t serviceid, bool remove) {
        auto s = find_service(serviceid);
        if (s != nullptr)
        {
            s->on_timer(timerid, remove);
        }
        else
        {
            timer_.remove(timerid);
        }
    });

    thread_ = std::thread([this]() {
        state_.store(state::ready, std::memory_order_release);
        CONSOLE_INFO(router_->get_logger(), "WORKER-%u START", workerid_);

        while (true)
        {
            auto s = state_.load(std::memory_order_acquire);
            if (s == state::exited)
            {
                break;
            }

            if (!queue_.empty())
            {
                std::unique_lock<std::mutex> lock(mutex_);
                auto task = queue_.front();
                queue_.pop_front();
                lock.unlock();
                task();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // std::this_thread::yield();
        }

        CONSOLE_INFO(router_->get_logger(), "WORKER-%u STOP", workerid_);
    });

    // wait for thread is created
    while (state_.load(std::memory_order_acquire) != state::ready)
    {
        std::this_thread::yield();
    }
}

void worker::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back([this] {
        auto s = state_.load(std::memory_order_acquire);
        if (s == state::stopping || s == state::exited)
        {
            return;
        }

        if (services_.empty())
        {
            state_.store(state::exited, std::memory_order_release);
            return;
        }

        state_.store(state::stopping, std::memory_order_release);
        for (auto& it : services_)
        {
            it.second->exit();
        }
    });
}

void worker::wait()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
}

bool worker::stoped() const
{
    return state_.load(std::memory_order_acquire) == state::exited;
}

uint32_t worker::uuid()
{
    auto res = uuid_++;
    res %= max_uuid;
    ++res;
    res |= id() << WORKER_ID_SHIFT;
    return res;
}

void worker::add_service(std::string service_type, std::string config, bool unique, uint32_t creatorid, int32_t sessionid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back([this, service_type = std::move(service_type), config = std::move(config), unique, creatorid, sessionid](){
        do
        {
            if (state_.load(std::memory_order_acquire) != state::ready)
            {
                break;
            }

            size_t counter = 0;
            uint32_t serviceid = 0;
            do
            {
                if (counter >= worker::max_uuid)
                {
                    CONSOLE_ERROR(router_->get_logger(), "new service failed: can not get more service id. worker[%d] service num[%u].", id(), services_.size());
                    break;
                }
                serviceid = uuid();
                ++counter;
            } while (services_.find(serviceid) != services_.end());

            auto s = router_->make_service(service_type);
            if (!s)
            {
                CONSOLE_ERROR(router_->get_logger(), "new service failed:service type[%s] was not registered", service_type.data());
                break;
            }

            s->set_id(serviceid);
            s->set_logger(router_->get_logger());
            s->set_unique(unique);
            s->set_server_context(server_, router_, this);

            if (!s->init(config))
            {
                break;
            }

            s->ok(true);

            auto res = services_.emplace(serviceid, std::move(s));
            if (!res.second)
            {
                break;
            }

            count_.fetch_add(1, std::memory_order_release);

            if (server_->get_state() != state::init)
            {
                res.first->second->start();
            }

            if (0 != sessionid)
            {
                router_->response(creatorid, std::string_view{}, std::to_string(serviceid), sessionid);
            }
            return;
        } while (false);

        shared(services_.empty());

        if (0 != sessionid)
        {
            router_->response(creatorid, std::string_view{}, "0", sessionid);
        }
    });
}

void worker::remove_service(uint32_t serviceid, uint32_t sender, uint32_t sessionid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back([this, serviceid, sender, sessionid] {
        if (auto s = find_service(serviceid); nullptr != s)
        {
            count_.fetch_sub(1, std::memory_order_release);

            s->destroy();
            auto content = format(R"({"name":"%s","serviceid":%X,"errmsg":"service destroy"})", s->name().data(), s->id());
            router_->response(sender, "service destroy", content, sessionid);
            services_.erase(serviceid);
            if (services_.empty()) shared(true);

            string_view_t header{ "exit" };
            auto buf = message::create_buffer();
            buf->write_back(content.data(), content.size());
            router_->broadcast(serviceid, buf, header, PTYPE_SYSTEM);
        }
        else
        {
            router_->response(sender, "worker::remove_service ", format("service [%X] not found", serviceid), sessionid, PTYPE_ERROR);
        }

        if (services_.size() == 0 && state_.load(std::memory_order_acquire) == state::stopping)
        {
            state_.store(state::exited, std::memory_order_release);
        }
    });
}

void worker::send(message_ptr_t&& msg)
{
    if (mq_.push_back(std::move(msg)) == 1)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back([this] {
            size_t count = 0;
            if (mq_.size() != 0)
            {
                service* ser = nullptr;
                swapmq_.clear();
                mq_.swap(swapmq_);
                count = swapmq_.size();
                for (auto& msg : swapmq_)
                {
                    handle_one(ser, std::move(msg));
                }
            }

            auto begin_time = server_->now();
            if (begin_time != 0)
            {
                auto difftime = server_->now() - begin_time;
                cpu_time_ += difftime;
                if (difftime > 1000)
                {
                    CONSOLE_WARN(router_->get_logger(), "worker handle cost %" PRId64 "ms queue size %zu", difftime, count);
                }
            }
        });
    }
}

uint8_t worker::id() const
{
    return workerid_;
}

service* worker::find_service(uint32_t serviceid) const
{
    auto iter = services_.find(serviceid);
    if (services_.end() != iter)
    {
        return iter->second.get();
    }
    return nullptr;
}

void worker::shared(bool v)
{
    shared_ = v;
}

bool worker::shared() const
{
    return shared_.load();
}

void worker::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back([this] {
        for (auto& it : services_)
        {
            it.second->start();
        }
    });
}

void worker::update()
{
    //update_state is true
    if (update_state_.test_and_set(std::memory_order_acquire))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace_back([this] {
        timer_.update();

        update_state_.clear(std::memory_order_release);
    });
}

void worker::handle_one(service*& ser, message_ptr_t&& msg)
{
    if (msg->broadcast())
    {
        for (auto& it : services_)
        {
            auto& s = it.second;
            if (s->is_ok() && s->id() != msg->sender())
            {
                s->handle_message(std::forward<message_ptr_t>(msg));
            }
        }
        return;
    }

    if (nullptr == ser || ser->id() != msg->receiver())
    {
        ser = find_service(msg->receiver());
        if (nullptr == ser)
        {
            if (msg->sender() != 0)
            {
                msg->set_sessionid(-msg->sessionid());
                router_->response(msg->sender(), "worker::handle_one ", format("[%X] attempt send to dead service [%X]: %s.", msg->sender(), msg->receiver(), hex_string({ msg->data(),msg->size() }).data()).data(), msg->sessionid(), PTYPE_ERROR);
            }
            return;
        }
    }
    ser->handle_message(std::forward<message_ptr_t>(msg));
    timer_.update();
}
