// Derived from acquire-zarr 0.8.1 thread pool interfaces
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace scopewriter::internal::zarr
{
    class ThreadPool
    {
    public:
        using Task = std::function<bool(std::string&)>;
        using ErrorCallback = std::function<void(std::string)>;

        ThreadPool(unsigned int threadCount,
                   std::size_t queueCapacity,
                   ErrorCallback errorCallback);
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        bool push(Task task);
        void awaitStop();

    private:
        void run();

        ErrorCallback m_errorCallback;
        std::vector<std::thread> m_threads;
        std::queue<Task> m_tasks;
        std::size_t m_queueCapacity;
        std::mutex m_mutex;
        std::condition_variable m_taskReady;
        std::condition_variable m_queueSpace;
        bool m_accepting{true};
        bool m_failed{false};
    };
}
