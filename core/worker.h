#pragma once
#include "config.hpp"
#include "common/concurrent_queue.hpp"
#include "common/spinlock.hpp"
#include "common/timer.hpp"
#include "worker_timer.hpp"

class server;
class router;

class worker
{
    using queue_t = concurrent_queue<message_ptr_t, spin_lock, std::vector>;

    using command_hander_t = std::function<std::string(const std::vector<std::string>&)>;

    using task_t = std::function<void()>;

public:
    static constexpr uint16_t max_uuid = 0xFFFF;

    friend class server;

    friend class socket;

    explicit worker(server* srv, router* r, uint32_t id);

    ~worker();

    worker(const worker&) = delete;

    worker& operator=(const worker&) = delete;

    void remove_service(uint32_t serviceid, uint32_t sender, uint32_t sessionid);

    uint32_t id() const;

    uint32_t uuid();

    void add_service(std::string service_type, std::string config, bool unique, uint32_t creatorid, int32_t sessionid);

    void send(message_ptr_t&& msg);

    void shared(bool v);

    bool shared() const;

    void runcmd(uint32_t sender, const std::string& cmd, int32_t sessionid);

    uint32_t make_prefab(const buffer_ptr_t& buf);

    void send_prefab(uint32_t sender, uint32_t receiver, uint32_t prefabid, string_view_t header, int32_t sessionid, uint8_t type) const;

    worker_timer& timer() { return timer_; }

private:
    void init();

    void stop();

    void wait();

    bool stoped() const;

private:
    void start();

    void update();

    void handle_one(service*& ser, message_ptr_t&& msg);

    service* find_service(uint32_t serviceid) const;

private:
    std::atomic<state> state_ = state::init;
    std::atomic_bool shared_ = true;
    //to prevent post too many update event
    std::atomic_flag update_state_ = ATOMIC_FLAG_INIT;
    std::atomic_uint32_t count_ = 0;
    uint32_t uuid_ = 0;
    int64_t cpu_time_ = 0;
    uint32_t workerid_;
    router* router_;
    server* server_;
    std::deque<task_t> queue_;
    std::thread thread_;
    queue_t mq_;
    queue_t::container_type swapmq_;
    worker_timer timer_;
    std::unordered_map<uint32_t, service_ptr_t> services_;
    std::unordered_map<uint32_t, buffer_ptr_t> prefabs_;
    std::unordered_map<std::string, command_hander_t> commands_;
};