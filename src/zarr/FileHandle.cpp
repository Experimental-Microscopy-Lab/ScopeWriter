// Derived from acquire-zarr 0.8.1 file.handle and platform implementations

#include "FileHandle.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace scopewriter::internal::zarr
{
    namespace
    {
#if defined(_WIN32)
        // Format one Windows error code
        std::string windowsError(DWORD code)
        {
            LPSTR buffer = nullptr;
            const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                                  | FORMAT_MESSAGE_FROM_SYSTEM
                                                  | FORMAT_MESSAGE_IGNORE_INSERTS,
                                              nullptr,
                                              code,
                                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                              reinterpret_cast<LPSTR>(&buffer),
                                              0,
                                              nullptr);
            const std::string message = size == 0 ? "Windows error " + std::to_string(code)
                                                   : std::string(buffer, size);
            if (buffer != nullptr)
            {
                LocalFree(buffer);
            }
            return message;
        }

        struct ThreadEvent
        {
            ThreadEvent()
                : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))
            {
            }

            ~ThreadEvent()
            {
                if (handle != nullptr)
                {
                    CloseHandle(handle);
                }
            }

            HANDLE handle{nullptr};
        };
#endif
    }

    // Open one shard file for positioned writes
    FileHandle::FileHandle(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        const HANDLE handle = CreateFileW(path.c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr,
                                          OPEN_ALWAYS,
                                          FILE_FLAG_OVERLAPPED,
                                          nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Failed to open OME-Zarr file: "
                                     + windowsError(GetLastError()));
        }
        m_handle = handle;
#else
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (descriptor < 0)
        {
            throw std::runtime_error("Failed to open OME-Zarr file: "
                                     + std::string(std::strerror(errno)));
        }
        m_handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor + 1));
#endif
    }

    // Close the native file handle
    FileHandle::~FileHandle()
    {
#if defined(_WIN32)
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
        }
#else
        if (m_handle != nullptr)
        {
            const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(m_handle)) - 1;
            ::close(descriptor);
        }
#endif
    }

    // Return the native file handle
    void* FileHandle::native() const noexcept
    {
        return m_handle;
    }

    // Track one borrowed pooled handle
    BorrowedHandle::BorrowedHandle(FileHandle* handle,
                                   std::filesystem::path path,
                                   FileHandlePool* pool)
        : m_handle(handle), m_path(std::move(path)), m_pool(pool)
    {
    }

    BorrowedHandle::~BorrowedHandle()
    {
        release();
    }

    BorrowedHandle::BorrowedHandle(BorrowedHandle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_path(std::move(other.m_path)),
          m_pool(std::exchange(other.m_pool, nullptr))
    {
    }

    BorrowedHandle& BorrowedHandle::operator=(BorrowedHandle&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_handle = std::exchange(other.m_handle, nullptr);
            m_path = std::move(other.m_path);
            m_pool = std::exchange(other.m_pool, nullptr);
        }
        return *this;
    }

    FileHandle* BorrowedHandle::get() const noexcept
    {
        return m_handle;
    }

    // Return this handle to its pool
    void BorrowedHandle::release()
    {
        if (m_handle != nullptr && m_pool != nullptr)
        {
            m_pool->giveBack(m_path);
        }
        m_handle = nullptr;
        m_pool = nullptr;
    }

    // Set a bounded platform handle limit
    FileHandlePool::FileHandlePool()
    {
#if defined(_WIN32)
        m_maxHandles = 8192;
#else
        rlimit limit{};
        m_maxHandles = getrlimit(RLIMIT_NOFILE, &limit) == 0
            ? static_cast<std::size_t>((std::min)(limit.rlim_cur, rlim_t{8192}))
            : 1024;
#endif
        m_maxHandles = (std::max)(m_maxHandles, std::size_t{1});
    }

    // Borrow or create one cached file handle
    BorrowedHandle FileHandlePool::borrow(const std::filesystem::path& path)
    {
        std::unique_lock lock(m_mutex);
        m_spaceAvailable.wait(lock, [this, &path]
        {
            return m_handles.contains(path) || m_handles.size() < m_maxHandles;
        });

        auto iterator = m_handles.find(path);
        if (iterator == m_handles.end())
        {
            constexpr int attempts = 3;
            std::shared_ptr<FileHandle> handle;
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                try
                {
                    handle = std::make_shared<FileHandle>(path);
                    break;
                }
                catch (...)
                {
                    if (attempt + 1 == attempts)
                    {
                        throw;
                    }
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    lock.lock();
                    iterator = m_handles.find(path);
                    if (iterator != m_handles.end())
                    {
                        break;
                    }
                }
            }
            if (iterator == m_handles.end())
            {
                m_lru.push_front(path);
                iterator = m_handles.emplace(path,
                                             Entry{std::move(handle), m_lru.begin(), 0}).first;
            }
        }
        else
        {
            m_lru.splice(m_lru.begin(), m_lru, iterator->second.lru);
            iterator->second.lru = m_lru.begin();
        }
        ++iterator->second.references;
        return {iterator->second.handle.get(), path, this};
    }

    // Release one reference to a cached handle
    void FileHandlePool::giveBack(const std::filesystem::path& path)
    {
        std::lock_guard lock(m_mutex);
        const auto iterator = m_handles.find(path);
        if (iterator != m_handles.end() && iterator->second.references > 0)
        {
            --iterator->second.references;
        }
        if (m_handles.size() >= m_maxHandles)
        {
            evictIdle();
        }
        m_spaceAvailable.notify_all();
    }

    // Remove one idle handle from the cache
    void FileHandlePool::close(const std::filesystem::path& path)
    {
        std::lock_guard lock(m_mutex);
        const auto iterator = m_handles.find(path);
        if (iterator != m_handles.end() && iterator->second.references == 0)
        {
            m_lru.erase(iterator->second.lru);
            m_handles.erase(iterator);
        }
        m_spaceAvailable.notify_all();
    }

    // Evict the least recently used idle handle
    void FileHandlePool::evictIdle()
    {
        for (auto iterator = m_lru.rbegin(); iterator != m_lru.rend(); ++iterator)
        {
            const auto handle = m_handles.find(*iterator);
            if (handle != m_handles.end() && handle->second.references == 0)
            {
                m_handles.erase(handle);
                m_lru.erase(std::next(iterator).base());
                return;
            }
        }
    }

    // Write a complete byte range at one file offset
    bool writeAt(FileHandle& handle,
                 std::uint64_t offset,
                 const std::uint8_t* data,
                 std::size_t byteCount,
                 std::string& error)
    {
        if (byteCount == 0)
        {
            return true;
        }
#if defined(_WIN32)
        thread_local ThreadEvent event;
        if (event.handle == nullptr)
        {
            error = "Failed to create OME-Zarr write event";
            return false;
        }
        std::size_t writtenTotal = 0;
        while (writtenTotal < byteCount)
        {
            const DWORD request = static_cast<DWORD>((std::min)(
                byteCount - writtenTotal,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            ResetEvent(event.handle);
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32u);
            overlapped.hEvent = event.handle;
            if (!WriteFile(static_cast<HANDLE>(handle.native()),
                           data + writtenTotal,
                           request,
                           nullptr,
                           &overlapped)
                && GetLastError() != ERROR_IO_PENDING)
            {
                error = "Failed to write OME-Zarr shard: " + windowsError(GetLastError());
                return false;
            }
            DWORD written = 0;
            if (!GetOverlappedResult(static_cast<HANDLE>(handle.native()),
                                     &overlapped,
                                     &written,
                                     TRUE)
                || written == 0)
            {
                error = "Failed to complete OME-Zarr shard write: "
                    + windowsError(GetLastError());
                return false;
            }
            writtenTotal += written;
            offset += written;
        }
        return true;
#else
        const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle.native())) - 1;
        std::size_t writtenTotal = 0;
        while (writtenTotal < byteCount)
        {
            const ssize_t written = pwrite(descriptor,
                                           data + writtenTotal,
                                           byteCount - writtenTotal,
                                           static_cast<off_t>(offset));
            if (written <= 0)
            {
                error = "Failed to write OME-Zarr shard: " + std::string(std::strerror(errno));
                return false;
            }
            writtenTotal += static_cast<std::size_t>(written);
            offset += static_cast<std::uint64_t>(written);
        }
        return true;
#endif
    }

    // Flush one native file handle to storage
    bool flush(FileHandle& handle, std::string& error)
    {
#if defined(_WIN32)
        if (!FlushFileBuffers(static_cast<HANDLE>(handle.native())))
        {
            error = "Failed to flush OME-Zarr shard: " + windowsError(GetLastError());
            return false;
        }
#else
        const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle.native())) - 1;
        if (fsync(descriptor) != 0)
        {
            error = "Failed to flush OME-Zarr shard: " + std::string(std::strerror(errno));
            return false;
        }
#endif
        return true;
    }
}
