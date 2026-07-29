// Derived from acquire-zarr 0.8.1 file.sink filesystem backend

#include "FileSink.h"

#include <stdexcept>
#include <system_error>
#include <utility>

namespace scopewriter::internal::zarr
{
    FileSink::FileSink(std::filesystem::path path,
                       std::shared_ptr<FileHandlePool> handlePool)
        : m_path(std::move(path)), m_handlePool(std::move(handlePool))
    {
        std::error_code filesystemError;
        std::filesystem::create_directories(m_path.parent_path(), filesystemError);
        if (filesystemError)
        {
            throw std::runtime_error("Failed to create OME-Zarr directory: "
                                     + filesystemError.message());
        }
    }

    FileSink::~FileSink()
    {
        m_handlePool->close(m_path);
    }

    bool FileSink::write(std::uint64_t offset,
                         const std::uint8_t* data,
                         std::size_t byteCount,
                         std::string& error)
    {
        try
        {
            auto handle = m_handlePool->borrow(m_path);
            if (handle.get() == nullptr)
            {
                error = "Failed to borrow OME-Zarr file handle";
                return false;
            }
            return writeAt(*handle.get(), offset, data, byteCount, error);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }

    bool FileSink::finalize(std::string& error)
    {
        std::lock_guard lock(m_mutex);
        if (m_finalized)
        {
            return m_finalizeResult;
        }
        m_finalized = true;
        try
        {
            auto handle = m_handlePool->borrow(m_path);
            if (handle.get() == nullptr)
            {
                error = "Failed to borrow OME-Zarr file handle for flush";
                return m_finalizeResult = false;
            }
            m_finalizeResult = flush(*handle.get(), error);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            m_finalizeResult = false;
        }
        return m_finalizeResult;
    }
}
