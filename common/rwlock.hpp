#pragma once
#include <thread>
#include <atomic>
#include "noncopyable.hpp"

/* read write lock */
class rwlock : public noncopyable
{
public:
    rwlock()
        : read_(0)
        , write_(false)
    {
    }

    void lock_shared()
    {
        for (;;)
        {
            int count = 0;
            while (write_.load(std::memory_order_acquire))
            {
                ++count;
                if (count > 1000)
                {
                    std::this_thread::yield();
                }
            }
            read_.fetch_add(1, std::memory_order_release);
            if (write_.load(std::memory_order_acquire))
            {
                read_.fetch_sub(1, std::memory_order_release);
            }
            else
            {
                break;
            }
        }
    }

    void lock()
    {
        int count = 0;
        while (write_.exchange(true, std::memory_order_acquire))
        {
            ++count;
            if (count > 1000)
            {
                std::this_thread::yield();
            }
        }

        count = 0;
        while (read_.load(std::memory_order_acquire))
        {
            ++count;
            if (count > 1000)
            {
                std::this_thread::yield();
            }
        }
    }

    void unlock()
    {
        write_.store(false, std::memory_order_release);
    }

    void unlock_shared()
    {
        read_.fetch_sub(1, std::memory_order_release);
    }

private:
    std::atomic<int32_t> read_;
    std::atomic_bool     write_;
};
