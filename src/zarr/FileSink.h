// Derived from acquire-zarr 0.8.1 filesystem sink interfaces
#pragma once

#include "FileHandle.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace scopewriter::internal::zarr
{
    class FileSink
    {
    public:
        FileSink(std::filesystem::path path, std::shared_ptr<FileHandlePool> handlePool);
        ~FileSink();

        FileSink(const FileSink&) = delete;
        FileSink& operator=(const FileSink&) = delete;

        bool write(std::uint64_t offset,
                   const std::uint8_t* data,
                   std::size_t byteCount,
                   std::string& error);
        bool finalize(std::string& error);

    private:
        std::filesystem::path m_path;
        std::shared_ptr<FileHandlePool> m_handlePool;
        std::mutex m_mutex;
        bool m_finalized{false};
        bool m_finalizeResult{false};
    };
}
