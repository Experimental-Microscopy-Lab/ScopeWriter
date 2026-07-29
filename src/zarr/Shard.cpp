// Derived from acquire-zarr 0.8.1 filesystem shard implementation

#include "Shard.h"

#include <crc32c/crc32c.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace scopewriter::internal::zarr
{
    namespace
    {
        constexpr std::uint64_t kUnwritten = (std::numeric_limits<std::uint64_t>::max)();

        // Append one little endian 64 bit value
        void appendLittleEndian64(std::vector<std::uint8_t>& output, std::uint64_t value)
        {
            for (int byte = 0; byte < 8; ++byte)
            {
                output.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
            }
        }

        // Append one little endian 32 bit value
        void appendLittleEndian32(std::vector<std::uint8_t>& output, std::uint32_t value)
        {
            for (int byte = 0; byte < 4; ++byte)
            {
                output.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
            }
        }
    }

    // Create a shard with unwritten index entries
    Shard::Shard(std::filesystem::path path,
                 std::size_t chunkCount,
                 std::shared_ptr<FileHandlePool> handlePool)
        : m_offsets(chunkCount, kUnwritten),
          m_extents(chunkCount, kUnwritten),
          m_unwrittenChunks(chunkCount),
          m_sink(std::move(path), std::move(handlePool))
    {
        if (chunkCount == 0)
        {
            throw std::invalid_argument("OME-Zarr shard cannot be empty");
        }
    }

    // Finalize any remaining shard index
    Shard::~Shard()
    {
        std::string ignored;
        finalize(ignored);
    }

    // Store one encoded chunk in this shard
    bool Shard::writeChunk(std::size_t index,
                           const std::vector<std::uint8_t>& data,
                           std::string& error)
    {
        if (index >= m_offsets.size())
        {
            error = "OME-Zarr chunk index is out of range";
            return false;
        }
        std::uint64_t offset = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_finalized)
            {
                error = m_finalizeError;
                return m_finalizeResult;
            }
            if (m_offsets[index] != kUnwritten)
            {
                error = "OME-Zarr chunk was written more than once";
                return false;
            }
            offset = m_fileOffset;
            m_fileOffset += data.size();
            m_offsets[index] = offset;
            m_extents[index] = data.size();
        }
        if (!m_sink.write(offset, data.data(), data.size(), error))
        {
            return false;
        }
        return finishChunk(error);
    }

    // Mark one zero filled chunk as complete
    bool Shard::skipChunk(std::size_t index, std::string& error)
    {
        if (index >= m_offsets.size())
        {
            error = "OME-Zarr chunk index is out of range";
            return false;
        }
        return finishChunk(error);
    }

    // Finalize the shard when its last chunk completes
    bool Shard::finishChunk(std::string& error)
    {
        const std::size_t previous = m_unwrittenChunks.fetch_sub(1);
        if (previous == 0)
        {
            error = "OME-Zarr shard completion underflow";
            return false;
        }
        if (previous != 1)
        {
            return true;
        }
        std::lock_guard lock(m_mutex);
        return finalizeLocked(error);
    }

    // Finalize this shard under synchronization
    bool Shard::finalize(std::string& error)
    {
        std::lock_guard lock(m_mutex);
        return finalizeLocked(error);
    }

    // Write the index and flush the shard
    bool Shard::finalizeLocked(std::string& error)
    {
        if (m_finalized)
        {
            error = m_finalizeError;
            return m_finalizeResult;
        }
        m_finalized = true;
        if (!writeIndex(error) || !m_sink.finalize(error))
        {
            m_finalizeError = error;
            return m_finalizeResult = false;
        }
        return m_finalizeResult = true;
    }

    // Append the indexed sharding table and checksum
    bool Shard::writeIndex(std::string& error)
    {
        std::vector<std::uint8_t> index;
        index.reserve(m_offsets.size() * 16 + 4);
        for (std::size_t chunk = 0; chunk < m_offsets.size(); ++chunk)
        {
            appendLittleEndian64(index, m_offsets[chunk]);
            appendLittleEndian64(index, m_extents[chunk]);
        }
        appendLittleEndian32(index, crc32c::Crc32c(index.data(), index.size()));
        return m_sink.write(m_fileOffset, index.data(), index.size(), error);
    }
}
