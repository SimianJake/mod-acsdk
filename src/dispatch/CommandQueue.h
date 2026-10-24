#ifndef ACSDK_COMMAND_QUEUE_H
#define ACSDK_COMMAND_QUEUE_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace acsdk
{
    // Multi-producer (gRPC threads) / single-consumer (world thread) task queue (PLAN.md §4.1 #2-#4).
    // Holds no game types so it can be unit tested on its own.
    class CommandQueue
    {
    public:
        using Task = std::function<void()>;
        using Clock = std::chrono::steady_clock;

        explicit CommandQueue(std::size_t capacity) : _capacity(capacity) { }

        // Producer side. Returns false when the queue is full or closed; the caller fails the RPC
        // with RESOURCE_EXHAUSTED / UNAVAILABLE instead of buffering without bound.
        bool TryPush(Task task)
        {
            std::lock_guard lock(_mutex);
            if (_closed || _tasks.size() >= _capacity)
                return false;

            _tasks.push_back(std::move(task));
            return true;
        }

        // Consumer side (world thread). Runs tasks in FIFO order until maxTasks have run or the
        // budget is used up; whatever is left stays queued for the next tick. Returns tasks run.
        std::size_t Drain(std::size_t maxTasks, Clock::duration budget)
        {
            Clock::time_point const deadline = Clock::now() + budget;
            std::size_t ran = 0;

            while (ran < maxTasks)
            {
                Task task;
                {
                    std::lock_guard lock(_mutex);
                    if (_tasks.empty())
                        break;

                    task = std::move(_tasks.front());
                    _tasks.pop_front();
                }

                task();
                ++ran;

                if (Clock::now() >= deadline)
                    break;
            }

            return ran;
        }

        // Stop accepting tasks (shutdown). Already-queued tasks can still be drained.
        void Close()
        {
            std::lock_guard lock(_mutex);
            _closed = true;
        }

        std::size_t Size() const
        {
            std::lock_guard lock(_mutex);
            return _tasks.size();
        }

        void SetCapacity(std::size_t capacity)
        {
            std::lock_guard lock(_mutex);
            _capacity = capacity;
        }

    private:
        mutable std::mutex _mutex;
        std::deque<Task> _tasks;
        std::size_t _capacity;
        bool _closed = false;
    };
}

#endif
