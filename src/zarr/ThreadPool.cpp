// Derived from acquire-zarr 0.8.1 thread.pool with a bounded task queue

#include "ThreadPool.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace scopewriter::internal::zarr
{
    ThreadPool::ThreadPool(unsigned int threadCount,
                           std::size_t queueCapacity,
                           ErrorCallback errorCallback)
        : m_errorCallback(std::move(errorCallback)),
          m_queueCapacity((std::max)(queueCapacity, std::size_t{1}))
    {
        const unsigned int available = (std::max)(std::thread::hardware_concurrency(), 1u);
        threadCount = (std::clamp)(threadCount, 1u, available);
        m_threads.reserve(threadCount);
        try
        {
            for (unsigned int index = 0; index < threadCount; ++index)
            {
                m_threads.emplace_back([this]
                {
                    run();
                });
            }
        }
        catch (...)
        {
            {
                std::lock_guard lock(m_mutex);
                m_accepting = false;
            }
            m_taskReady.notify_all();
            for (auto& thread : m_threads)
            {
                thread.join();
            }
            throw;
        }
    }

    ThreadPool::~ThreadPool()
    {
        awaitStop();
    }

    bool ThreadPool::push(Task task)
    {
        std::unique_lock lock(m_mutex);
        m_queueSpace.wait(lock, [this]
        {
            return !m_accepting || m_failed || m_tasks.size() < m_queueCapacity;
        });
        if (!m_accepting || m_failed)
        {
            return false;
        }
        m_tasks.push(std::move(task));
        lock.unlock();
        m_taskReady.notify_one();
        return true;
    }

    void ThreadPool::awaitStop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_accepting = false;
        }
        m_taskReady.notify_all();
        m_queueSpace.notify_all();
        for (auto& thread : m_threads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        m_threads.clear();
    }

    void ThreadPool::run()
    {
        while (true)
        {
            Task task;
            {
                std::unique_lock lock(m_mutex);
                m_taskReady.wait(lock, [this]
                {
                    return !m_tasks.empty() || !m_accepting;
                });
                if (m_tasks.empty())
                {
                    return;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop();
                m_queueSpace.notify_one();
            }

            std::string error;
            bool success = false;
            try
            {
                success = task(error);
            }
            catch (const std::exception& exception)
            {
                error = exception.what();
            }
            catch (...)
            {
                error = "Unknown OME-Zarr worker failure";
            }
            if (success)
            {
                continue;
            }

            {
                std::lock_guard lock(m_mutex);
                if (m_failed)
                {
                    continue;
                }
                m_failed = true;
                m_accepting = false;
                while (!m_tasks.empty())
                {
                    m_tasks.pop();
                }
            }
            m_taskReady.notify_all();
            m_queueSpace.notify_all();
            m_errorCallback(error.empty() ? "OME-Zarr worker failed" : std::move(error));
        }
    }
}
