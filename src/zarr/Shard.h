// Derived from acquire-zarr 0.8.1 shard interfaces
#pragma once

#include "FileSink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scopewriter::internal::zarr
{
    class Shard
    {
    public:
        Shard(std::filesystem::path path,
              std::size_t chunkCount,
              std::shared_ptr<FileHandlePool> handlePool);
        ~Shard();

        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;

        bool writeChunk(std::size_t index,
                        const std::vector<std::uint8_t>& data,
                        std::string& error);
        bool skipChunk(std::size_t index, std::string& error);
        bool finalize(std::string& error);

    private:
        bool finishChunk(std::string& error);
        bool finalizeLocked(std::string& error);
        bool writeIndex(std::string& error);

        std::vector<std::uint64_t> m_offsets;
        std::vector<std::uint64_t> m_extents;
        std::atomic<std::size_t> m_unwrittenChunks;
        std::uint64_t m_fileOffset{0};
        std::mutex m_mutex;
        bool m_finalized{false};
        bool m_finalizeResult{false};
        std::string m_finalizeError;
        FileSink m_sink;
    };
}
