// Derived from acquire-zarr 0.8.1 chunk interfaces
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scopewriter::internal::zarr
{
    class Chunk
    {
    public:
        Chunk(std::size_t byteCount, std::size_t bytesPerSample);

        void writeRows(std::size_t destinationOffset,
                       const std::uint8_t* source,
                       std::size_t sourceStride,
                       std::size_t copyBytes,
                       std::size_t destinationStride,
                       std::size_t rowCount);
        bool hasData() const noexcept;
        bool compressAndTake(bool enableCompression,
                             int compressionLevel,
                             std::vector<std::uint8_t>& output,
                             std::string& error);

    private:
        std::size_t m_bytesPerSample;
        std::vector<std::uint8_t> m_buffer;
        bool m_hasData{false};
    };
}
