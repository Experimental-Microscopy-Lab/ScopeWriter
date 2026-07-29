// Derived from acquire-zarr 0.8.1 frame.queue with blocking memory backpressure

#include "FrameQueue.h"

#include <algorithm>
#include <utility>

namespace scopewriter::internal::zarr
{
    FrameQueue::FrameQueue(std::size_t frameCapacity, std::size_t byteCapacity)
        : m_frameCapacity((std::max)(frameCapacity, std::size_t{1})),
          m_byteCapacity((std::max)(byteCapacity, std::size_t{1}))
    {
    }

    bool FrameQueue::push(Frame frame)
    {
        const std::size_t frameBytes = frame.data.size();
        std::unique_lock lock(m_mutex);
        m_queueSpace.wait(lock, [this, frameBytes]
        {
            const bool fitsBytes = m_frames.empty() || m_bytesUsed + frameBytes <= m_byteCapacity;
            return !m_accepting || m_aborted
                || (m_frames.size() < m_frameCapacity && fitsBytes);
        });
        if (!m_accepting || m_aborted)
        {
            return false;
        }
        m_bytesUsed += frameBytes;
        m_frames.push_back(std::move(frame));
        lock.unlock();
        m_dataReady.notify_one();
        return true;
    }

    bool FrameQueue::pop(Frame& frame)
    {
        std::unique_lock lock(m_mutex);
        m_dataReady.wait(lock, [this]
        {
            return !m_frames.empty() || !m_accepting || m_aborted;
        });
        if (m_frames.empty())
        {
            return false;
        }
        frame = std::move(m_frames.front());
        m_frames.pop_front();
        m_bytesUsed -= frame.data.size();
        lock.unlock();
        m_queueSpace.notify_one();
        return true;
    }

    void FrameQueue::close()
    {
        {
            std::lock_guard lock(m_mutex);
            m_accepting = false;
        }
        m_dataReady.notify_all();
        m_queueSpace.notify_all();
    }

    void FrameQueue::abort()
    {
        {
            std::lock_guard lock(m_mutex);
            m_aborted = true;
            m_accepting = false;
            m_frames.clear();
            m_bytesUsed = 0;
        }
        m_dataReady.notify_all();
        m_queueSpace.notify_all();
    }

}
