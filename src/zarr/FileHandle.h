// Derived from acquire-zarr 0.8.1 file handle interfaces
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace scopewriter::internal::zarr
{
    class FileHandle
    {
    public:
        explicit FileHandle(const std::filesystem::path& path);
        ~FileHandle();

        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        void* native() const noexcept;

    private:
        void* m_handle{nullptr};
    };

    class FileHandlePool;

    class BorrowedHandle
    {
    public:
        BorrowedHandle() = default;
        BorrowedHandle(FileHandle* handle,
                       std::filesystem::path path,
                       FileHandlePool* pool);
        ~BorrowedHandle();

        BorrowedHandle(const BorrowedHandle&) = delete;
        BorrowedHandle& operator=(const BorrowedHandle&) = delete;
        BorrowedHandle(BorrowedHandle&& other) noexcept;
        BorrowedHandle& operator=(BorrowedHandle&& other) noexcept;

        FileHandle* get() const noexcept;

    private:
        void release();

        FileHandle* m_handle{nullptr};
        std::filesystem::path m_path;
        FileHandlePool* m_pool{nullptr};
    };

    class FileHandlePool
    {
    public:
        FileHandlePool();

        BorrowedHandle borrow(const std::filesystem::path& path);
        void giveBack(const std::filesystem::path& path);
        void close(const std::filesystem::path& path);

    private:
        struct Entry
        {
            std::shared_ptr<FileHandle> handle;
            std::list<std::filesystem::path>::iterator lru;
            std::uint32_t references{0};
        };

        void evictIdle();

        std::size_t m_maxHandles;
        std::list<std::filesystem::path> m_lru;
        std::unordered_map<std::filesystem::path, Entry> m_handles;
        std::mutex m_mutex;
        std::condition_variable m_spaceAvailable;
    };

    bool writeAt(FileHandle& handle,
                 std::uint64_t offset,
                 const std::uint8_t* data,
                 std::size_t byteCount,
                 std::string& error);
    bool flush(FileHandle& handle, std::string& error);
}
