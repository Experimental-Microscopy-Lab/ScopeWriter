// Derived from acquire-zarr 0.8.1 chunk and Zstd compression implementations

#include "Chunk.h"

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace scopewriter::internal::zarr
{
    namespace
    {
        struct ZstdContext
        {
            ZstdContext()
                : value(ZSTD_createCCtx())
            {
            }

            ~ZstdContext()
            {
                ZSTD_freeCCtx(value);
            }

            ZSTD_CCtx* value{nullptr};
        };
    }

    // Allocate a zero filled chunk buffer
    Chunk::Chunk(std::size_t byteCount, std::size_t bytesPerSample)
        : m_bytesPerSample(bytesPerSample), m_buffer(byteCount, 0)
    {
        if (byteCount == 0)
        {
            throw std::invalid_argument("OME-Zarr chunk cannot be empty");
        }
    }

    // Copy image rows into this chunk
    void Chunk::writeRows(std::size_t destinationOffset,
                          const std::uint8_t* source,
                          std::size_t sourceStride,
                          std::size_t copyBytes,
                          std::size_t destinationStride,
                          std::size_t rowCount)
    {
        if (rowCount == 0 || copyBytes == 0)
        {
            return;
        }
        const std::size_t span = (rowCount - 1) * destinationStride + copyBytes;
        if (destinationOffset > m_buffer.size() || span > m_buffer.size() - destinationOffset)
        {
            throw std::out_of_range("OME-Zarr tile exceeds chunk buffer");
        }

        bool anyData = m_hasData;
        for (std::size_t row = 0; row < rowCount; ++row)
        {
            const auto* sourceRow = source + row * sourceStride;
            auto* destinationRow = m_buffer.data() + destinationOffset
                + row * destinationStride;
            std::memcpy(destinationRow, sourceRow, copyBytes);
            if (!anyData)
            {
                anyData = std::any_of(sourceRow,
                                      sourceRow + copyBytes,
                                      [](std::uint8_t value)
                                      {
                                          return value != 0;
                                      });
            }
        }
        m_hasData = anyData;
    }

    // Report whether the chunk contains any nonzero byte
    bool Chunk::hasData() const noexcept
    {
        return m_hasData;
    }

    // Finalize byte order and transfer encoded chunk data
    bool Chunk::compressAndTake(bool enableCompression,
                                int compressionLevel,
                                std::vector<std::uint8_t>& output,
                                std::string& error)
    {
        if (m_buffer.empty())
        {
            error = "OME-Zarr chunk was already consumed";
            return false;
        }
        if constexpr (std::endian::native == std::endian::big)
        {
            if (m_bytesPerSample == 2)
            {
                for (std::size_t index = 0; index < m_buffer.size(); index += 2)
                {
                    std::swap(m_buffer[index], m_buffer[index + 1]);
                }
            }
        }
        if (!enableCompression)
        {
            output = std::move(m_buffer);
            return true;
        }

        thread_local ZstdContext context;
        if (context.value == nullptr)
        {
            error = "Failed to create OME-Zarr compression context";
            return false;
        }
        output.resize(ZSTD_compressBound(m_buffer.size()));
        const std::size_t compressedSize = ZSTD_compressCCtx(context.value,
                                                             output.data(),
                                                             output.size(),
                                                             m_buffer.data(),
                                                             m_buffer.size(),
                                                             compressionLevel);
        if (ZSTD_isError(compressedSize))
        {
            error = "Failed to compress OME-Zarr chunk: "
                + std::string(ZSTD_getErrorName(compressedSize));
            output.clear();
            return false;
        }
        output.resize(compressedSize);
        m_buffer.clear();
        return true;
    }
}
