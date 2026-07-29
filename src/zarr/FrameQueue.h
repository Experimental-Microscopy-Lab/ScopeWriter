// Derived from acquire-zarr 0.8.1 frame queue interfaces
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <vector>

namespace scopewriter::internal::zarr
{
    class FrameQueue
    {
    public:
        struct Frame
        {
            std::filesystem::path groupPath;
            std::vector<std::uint8_t> data;
            std::int64_t t{0};
            int c{0};
            int z{0};
            bool zeroFill{false};
        };

        FrameQueue(std::size_t frameCapacity, std::size_t byteCapacity);

        bool push(Frame frame);
        bool pop(Frame& frame);
        void close();
        void abort();

    private:
        std::deque<Frame> m_frames;
        std::size_t m_frameCapacity;
        std::size_t m_byteCapacity;
        std::size_t m_bytesUsed{0};
        std::mutex m_mutex;
        std::condition_variable m_dataReady;
        std::condition_variable m_queueSpace;
        bool m_accepting{true};
        bool m_aborted{false};
    };
}
