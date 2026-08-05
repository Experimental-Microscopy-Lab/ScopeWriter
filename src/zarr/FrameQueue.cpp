// Derived from acquire-zarr 0.8.1 frame.queue with blocking memory backpressure

#include "FrameQueue.h"

#include <utility>

namespace scopewriter::internal::zarr
{
    // Create a bounded frame queue
    FrameQueue::FrameQueue(std::size_t frameCapacity, std::size_t byteCapacity)
        : m_frameCapacity(frameCapacity),
          m_byteCapacity(byteCapacity)
    {
    }

    // Reuse or allocate one frame buffer
    std::vector<std::uint8_t> FrameQueue::acquireBuffer(std::size_t byteCount)
    {
        std::vector<std::uint8_t> buffer;
        {
            std::lock_guard lock(m_mutex);
            if (!m_freeBuffers.empty())
            {
                buffer = std::move(m_freeBuffers.back());
                m_freeBuffers.pop_back();
            }
        }
        buffer.resize(byteCount);
        return buffer;
    }

    // Enqueue one frame with memory backpressure
    bool FrameQueue::push(Frame frame)
    {
        const std::size_t frameBytes = frame.data.size();
        std::unique_lock lock(m_mutex);
        m_queueSpace.wait(lock, [this, frameBytes]
        {
            const bool fitsBytes = m_bytesUsed <= m_byteCapacity
                && frameBytes <= m_byteCapacity - m_bytesUsed;
            return !m_accepting
                || (m_frames.size() + m_processing < m_frameCapacity && fitsBytes);
        });
        if (!m_accepting)
        {
            return false;
        }
        m_bytesUsed += frameBytes;
        m_frames.push_back(std::move(frame));
        lock.unlock();
        m_dataReady.notify_one();
        return true;
    }

    // Wait for and dequeue one frame
    bool FrameQueue::pop(Frame& frame)
    {
        std::unique_lock lock(m_mutex);
        m_dataReady.wait(lock, [this]
        {
            return !m_frames.empty() || !m_accepting;
        });
        if (m_frames.empty())
        {
            return false;
        }
        frame = std::move(m_frames.front());
        m_frames.pop_front();
        ++m_processing;
        return true;
    }

    // Mark one frame complete and recycle its buffer
    void FrameQueue::finish(Frame& frame)
    {
        {
            std::lock_guard lock(m_mutex);
            m_bytesUsed -= frame.data.size();
            if (!m_aborted
                && !frame.data.empty()
                && m_freeBuffers.size() < m_frameCapacity)
            {
                frame.data.clear();
                m_freeBuffers.push_back(std::move(frame.data));
            }
            --m_processing;
            if (m_frames.empty() && m_processing == 0)
            {
                m_idle.notify_all();
            }
        }
        m_queueSpace.notify_one();
    }

    // Wait until every queued frame completes
    bool FrameQueue::waitIdle()
    {
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this]
        {
            return m_aborted || (m_frames.empty() && m_processing == 0);
        });
        return !m_aborted;
    }

    // Stop accepting frames and drain the queue
    void FrameQueue::close()
    {
        {
            std::lock_guard lock(m_mutex);
            m_accepting = false;
        }
        m_dataReady.notify_all();
        m_queueSpace.notify_all();
    }

    // Cancel queued frames and wake all waiters
    void FrameQueue::abort()
    {
        {
            std::lock_guard lock(m_mutex);
            m_aborted = true;
            m_accepting = false;
            for (const auto& frame : m_frames)
            {
                m_bytesUsed -= frame.data.size();
            }
            m_frames.clear();
        }
        m_dataReady.notify_all();
        m_queueSpace.notify_all();
        m_idle.notify_all();
    }

}
