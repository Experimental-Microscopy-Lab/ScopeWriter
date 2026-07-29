// Provides single frame access across supported dataset formats

#include "scopewriter/ScopeWriter.h"

#include <crc32c/crc32c.h>
#include <tiffio.h>
#include <zstd.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>

namespace scopewriter
{
    namespace
    {
        constexpr std::uint64_t kUnwrittenChunk =
            (std::numeric_limits<std::uint64_t>::max)();

        struct TiffCloser
        {
            void operator()(TIFF* tiff) const
            {
                TIFFClose(tiff);
            }
        };

        // Open a TIFF path with native platform encoding
        TIFF* openTiff(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            return TIFFOpenW(path.c_str(), "r");
#else
            return TIFFOpen(path.string().c_str(), "r");
#endif
        }

        // Load one complete dataset file into memory
        bool readFile(const std::filesystem::path& path,
                      std::vector<std::uint8_t>& bytes,
                      std::string& error)
        {
            std::error_code filesystemError;
            const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
            if (filesystemError
                || size > (std::numeric_limits<std::size_t>::max)()
                || size > static_cast<std::uintmax_t>(
                    (std::numeric_limits<std::streamsize>::max)()))
            {
                error = "Failed to inspect dataset file";
                return false;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                error = "Failed to open dataset file";
                return false;
            }
            bytes.resize(static_cast<std::size_t>(size));
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()))
            {
                error = "Failed to load dataset file";
                return false;
            }
            return true;
        }

        // Load one complete metadata file as text
        bool readText(const std::filesystem::path& path,
                      std::string& text,
                      std::string& error)
        {
            std::vector<std::uint8_t> bytes;
            if (!readFile(path, bytes, error))
            {
                return false;
            }
            text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        }

        // Locate a JSON value used by ScopeWriter metadata
        std::size_t jsonValuePosition(const std::string& json,
                                      std::string_view key,
                                      std::size_t begin = 0)
        {
            const std::string token = '"' + std::string(key) + '"';
            const std::size_t name = json.find(token, begin);
            if (name == std::string::npos)
            {
                return name;
            }
            std::size_t position = json.find(':', name + token.size());
            if (position == std::string::npos)
            {
                return position;
            }
            ++position;
            while (position < json.size()
                   && std::isspace(static_cast<unsigned char>(json[position])))
            {
                ++position;
            }
            return position;
        }

        // Parse one JSON string value
        bool jsonString(const std::string& json,
                        std::string_view key,
                        std::string& value,
                        std::size_t begin = 0)
        {
            std::size_t position = jsonValuePosition(json, key, begin);
            if (position == std::string::npos || position >= json.size()
                || json[position] != '"')
            {
                return false;
            }
            value.clear();
            for (++position; position < json.size(); ++position)
            {
                const char character = json[position];
                if (character == '"')
                {
                    return true;
                }
                if (character != '\\')
                {
                    value.push_back(character);
                    continue;
                }
                if (++position >= json.size())
                {
                    return false;
                }
                switch (json[position])
                {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: return false;
                }
            }
            return false;
        }

        // Parse one complete integer token
        template<typename Integer>
        bool parseInteger(std::string_view text, Integer& value)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        // Parse one JSON integer value
        template<typename Integer>
        bool jsonInteger(const std::string& json,
                         std::string_view key,
                         Integer& value,
                         std::size_t begin = 0)
        {
            std::size_t position = jsonValuePosition(json, key, begin);
            if (position == std::string::npos || position >= json.size())
            {
                return false;
            }
            bool quoted = false;
            if (json[position] == '"')
            {
                quoted = true;
                ++position;
            }
            const std::size_t numberBegin = position;
            if (position < json.size() && json[position] == '-')
            {
                ++position;
            }
            while (position < json.size()
                   && std::isdigit(static_cast<unsigned char>(json[position])))
            {
                ++position;
            }
            if (position == numberBegin || (quoted && (position >= json.size()
                                                        || json[position] != '"')))
            {
                return false;
            }
            return parseInteger(std::string_view(json).substr(numberBegin,
                                                               position - numberBegin),
                                value);
        }

        // Parse one XML integer attribute
        template<typename Integer>
        bool xmlInteger(const std::string& xml,
                        std::string_view name,
                        Integer& value)
        {
            const std::string token = std::string(name) + "=\"";
            const std::size_t attribute = xml.find(token);
            if (attribute == std::string::npos)
            {
                return false;
            }
            const std::size_t begin = attribute + token.size();
            const std::size_t end = xml.find('"', begin);
            return end != std::string::npos
                && parseInteger(std::string_view(xml).substr(begin, end - begin), value);
        }

        // Parse one JSON integer array
        bool integerArray(const std::string& json,
                          std::string_view key,
                          std::vector<std::int64_t>& values,
                          std::size_t begin = 0)
        {
            std::size_t position = jsonValuePosition(json, key, begin);
            if (position == std::string::npos || position >= json.size()
                || json[position] != '[')
            {
                return false;
            }
            values.clear();
            ++position;
            while (position < json.size())
            {
                while (position < json.size()
                       && std::isspace(static_cast<unsigned char>(json[position])))
                {
                    ++position;
                }
                if (position < json.size() && json[position] == ']')
                {
                    return true;
                }
                const std::size_t numberBegin = position;
                if (position < json.size() && json[position] == '-')
                {
                    ++position;
                }
                while (position < json.size()
                       && std::isdigit(static_cast<unsigned char>(json[position])))
                {
                    ++position;
                }
                std::int64_t value = 0;
                if (position == numberBegin
                    || !parseInteger(std::string_view(json).substr(numberBegin,
                                                                   position - numberBegin),
                                     value))
                {
                    return false;
                }
                values.push_back(value);
                while (position < json.size()
                       && std::isspace(static_cast<unsigned char>(json[position])))
                {
                    ++position;
                }
                if (position >= json.size() || (json[position] != ',' && json[position] != ']'))
                {
                    return false;
                }
                if (json[position] == ']')
                {
                    return true;
                }
                ++position;
            }
            return false;
        }

        // Decode one little endian 64 bit value
        std::uint64_t littleEndian64(const std::uint8_t* data)
        {
            std::uint64_t value = 0;
            for (int byte = 0; byte < 8; ++byte)
            {
                value |= static_cast<std::uint64_t>(data[byte]) << (byte * 8);
            }
            return value;
        }

        // Decode one little endian 32 bit value
        std::uint32_t littleEndian32(const std::uint8_t* data)
        {
            std::uint32_t value = 0;
            for (int byte = 0; byte < 4; ++byte)
            {
                value |= static_cast<std::uint32_t>(data[byte]) << (byte * 8);
            }
            return value;
        }

        // Apply ScopeWriter metadata stored in a TIFF description
        void applyPlainTiffMetadata(const std::string& description, DatasetFrame& frame)
        {
            std::string schema;
            if (!jsonString(description, "schema", schema)
                || schema != kFrameMetadataProtocol)
            {
                return;
            }
            jsonString(description, "camera_id", frame.metadata.cameraId);
            jsonInteger(description, "frame_index", frame.metadata.frameIndex);
            jsonInteger(description, "timestamp_ns", frame.metadata.timestampNs);
            jsonInteger(description, "bits_per_sample", frame.significantBits);
            jsonInteger(description, "source_roi_x", frame.metadata.sourceRoiX);
            jsonInteger(description, "source_roi_y", frame.metadata.sourceRoiY);
            jsonInteger(description, "source_roi_width", frame.metadata.sourceRoiWidth);
            jsonInteger(description, "source_roi_height", frame.metadata.sourceRoiHeight);
        }

        // Load one TIFF directory and its frame metadata
        bool tiffFrame(const DatasetFrameLocation& location,
                       DatasetFrame& frame,
                       std::string& error)
        {
            if (location.frameIndex > (std::numeric_limits<tdir_t>::max)())
            {
                error = "TIFF frame index is out of range";
                return false;
            }
            std::unique_ptr<TIFF, TiffCloser> tiff(openTiff(location.dataPath));
            if (!tiff
                || !TIFFSetDirectory(tiff.get(), static_cast<tdir_t>(location.frameIndex)))
            {
                error = "Failed to select the TIFF frame";
                return false;
            }

            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint16_t bits = 0;
            std::uint16_t samples = 1;
            std::uint16_t planar = PLANARCONFIG_CONTIG;
            std::uint16_t sampleFormat = SAMPLEFORMAT_UINT;
            if (!TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width)
                || !TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height)
                || !TIFFGetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits))
            {
                error = "TIFF frame geometry is incomplete";
                return false;
            }
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planar);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat);
            if (width == 0 || height == 0 || samples != 1
                || planar != PLANARCONFIG_CONTIG || sampleFormat != SAMPLEFORMAT_UINT
                || bits == 0 || bits > 16
                || width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
                || height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
            {
                error = "TIFF frame layout is unsupported";
                return false;
            }

            frame.width = static_cast<int>(width);
            frame.height = static_cast<int>(height);
            frame.pixelType = bits <= 8 ? PixelType::UInt8 : PixelType::UInt16;
            const int storageBits = bits <= 8 ? 8 : 16;
            frame.significantBits = storageBits;
            const std::size_t sampleBytes = frame.pixelType == PixelType::UInt8 ? 1u : 2u;
            const std::size_t stride = static_cast<std::size_t>(width) * sampleBytes;
            if (stride > (std::numeric_limits<std::size_t>::max)() / height)
            {
                error = "TIFF frame is too large";
                return false;
            }
            frame.bytes.resize(stride * height);
            for (std::uint32_t row = 0; row < height; ++row)
            {
                if (TIFFReadScanline(tiff.get(), frame.bytes.data() + row * stride, row, 0) < 0)
                {
                    error = "Failed to load TIFF pixels";
                    return false;
                }
            }

            frame.metadata.frameIndex = location.frameIndex;
            frame.metadata.stride = stride;
            frame.metadata.sourceRoiWidth = frame.width;
            frame.metadata.sourceRoiHeight = frame.height;
            if (location.format == Format::OmeTiff)
            {
                if (!TIFFSetDirectory(tiff.get(), 0))
                {
                    error = "Failed to select OME-TIFF metadata";
                    return false;
                }
                char* description = nullptr;
                int significantBits = 0;
                if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, &description)
                    && description
                    && xmlInteger(description, "SignificantBits", significantBits)
                    && significantBits > 0 && significantBits <= storageBits)
                {
                    frame.significantBits = significantBits;
                }
            }
            else
            {
                char* description = nullptr;
                if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, &description)
                    && description)
                {
                    applyPlainTiffMetadata(description, frame);
                }
            }
            return true;
        }

        // Split one CSV row while preserving quoted fields
        bool csvFields(std::string_view line, std::vector<std::string>& fields)
        {
            fields.clear();
            std::string field;
            bool quoted = false;
            for (std::size_t index = 0; index < line.size(); ++index)
            {
                const char character = line[index];
                if (quoted)
                {
                    if (character == '"')
                    {
                        if (index + 1 < line.size() && line[index + 1] == '"')
                        {
                            field.push_back('"');
                            ++index;
                        }
                        else
                        {
                            quoted = false;
                        }
                    }
                    else
                    {
                        field.push_back(character);
                    }
                }
                else if (character == ',')
                {
                    fields.push_back(std::move(field));
                    field.clear();
                }
                else if (character == '"' && field.empty())
                {
                    quoted = true;
                }
                else
                {
                    field.push_back(character);
                }
            }
            if (quoted)
            {
                return false;
            }
            fields.push_back(std::move(field));
            return true;
        }

        // Load one binary frame using its metadata row
        bool binaryFrame(const DatasetFrameLocation& location,
                         DatasetFrame& frame,
                         std::string& error)
        {
            std::ifstream metadata(location.frameMetadataPath, std::ios::binary);
            if (!metadata)
            {
                error = "Failed to open binary frame metadata";
                return false;
            }
            std::string line;
            if (!std::getline(metadata, line))
            {
                error = "Binary frame metadata is empty";
                return false;
            }
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line != kBinaryFrameMetadataHeader)
            {
                error = "Binary frame metadata format is unsupported";
                return false;
            }

            std::uint64_t offset = 0;
            std::uint64_t current = 0;
            std::vector<std::string> fields;
            while (std::getline(metadata, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (line.empty())
                {
                    continue;
                }
                if (!csvFields(line, fields) || fields.size() != 14)
                {
                    error = "Binary frame metadata row is invalid";
                    return false;
                }
                std::uint64_t payloadBytes = 0;
                if (!parseInteger(fields[9], payloadBytes) || payloadBytes == 0)
                {
                    error = "Binary frame payload size is invalid";
                    return false;
                }
                if (current++ != location.frameIndex)
                {
                    if (offset > (std::numeric_limits<std::uint64_t>::max)() - payloadBytes)
                    {
                        error = "Binary frame offset exceeds the supported range";
                        return false;
                    }
                    offset += payloadBytes;
                    continue;
                }

                int pixelFormatId = -1;
                if (!parseInteger(fields[1], frame.metadata.frameIndex)
                    || !parseInteger(fields[2], frame.metadata.timestampNs)
                    || !parseInteger(fields[3], frame.width)
                    || !parseInteger(fields[4], frame.height)
                    || !parseInteger(fields[5], frame.significantBits)
                    || !parseInteger(fields[6], frame.metadata.stride)
                    || !parseInteger(fields[8], pixelFormatId)
                    || !parseInteger(fields[10], frame.metadata.sourceRoiX)
                    || !parseInteger(fields[11], frame.metadata.sourceRoiY)
                    || !parseInteger(fields[12], frame.metadata.sourceRoiWidth)
                    || !parseInteger(fields[13], frame.metadata.sourceRoiHeight))
                {
                    error = "Binary frame metadata values are invalid";
                    return false;
                }
                frame.metadata.cameraId = fields[0];
                if (pixelFormatId == 0 && fields[7] == "Mono8")
                {
                    frame.pixelType = PixelType::UInt8;
                }
                else if (pixelFormatId == 1 && fields[7] == "Mono16")
                {
                    frame.pixelType = PixelType::UInt16;
                }
                else
                {
                    error = "Binary frame pixel format is unsupported";
                    return false;
                }
                if (frame.width <= 0 || frame.height <= 0 || frame.metadata.stride == 0
                    || frame.metadata.stride
                        > (std::numeric_limits<std::size_t>::max)()
                            / static_cast<std::size_t>(frame.height)
                    || frame.metadata.stride * static_cast<std::size_t>(frame.height)
                        != payloadBytes
                    || payloadBytes > (std::numeric_limits<std::size_t>::max)()
                    || payloadBytes > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::streamsize>::max)()))
                {
                    error = "Binary frame layout is invalid";
                    return false;
                }
                std::ifstream input(location.dataPath, std::ios::binary);
                if (!input || offset > static_cast<std::uint64_t>(
                                          (std::numeric_limits<std::streamoff>::max)()))
                {
                    error = "Failed to open binary frame payload";
                    return false;
                }
                input.seekg(static_cast<std::streamoff>(offset));
                frame.bytes.resize(static_cast<std::size_t>(payloadBytes));
                input.read(reinterpret_cast<char*>(frame.bytes.data()),
                           static_cast<std::streamsize>(frame.bytes.size()));
                if (!input || input.gcount() != static_cast<std::streamsize>(frame.bytes.size()))
                {
                    error = "Failed to load binary frame payload";
                    return false;
                }
                return true;
            }
            error = "Binary frame index is out of range";
            return false;
        }

        // Assemble one OME Zarr plane from its shards
        bool zarrFrame(const DatasetFrameLocation& location,
                       DatasetFrame& frame,
                       std::string& error)
        {
            std::string metadata;
            if (!readText(location.dataPath / "zarr.json", metadata, error))
            {
                return false;
            }
            const std::size_t chunkGrid = metadata.find("\"chunk_grid\"");
            const std::size_t sharding = metadata.find("\"sharding_indexed\"");
            std::vector<std::int64_t> shape;
            std::vector<std::int64_t> shardShape;
            std::vector<std::int64_t> chunkShape;
            std::string dataType;
            if (chunkGrid == std::string::npos || sharding == std::string::npos
                || !integerArray(metadata, "shape", shape)
                || !integerArray(metadata, "chunk_shape", shardShape, chunkGrid)
                || !integerArray(metadata, "chunk_shape", chunkShape, sharding)
                || !jsonString(metadata, "data_type", dataType)
                || shape.size() != 5 || shardShape.size() != 5 || chunkShape.size() != 5
                || shardShape[0] != 1 || shardShape[1] != 1 || shardShape[2] != 1
                || chunkShape[0] != 1 || chunkShape[1] != 1 || chunkShape[2] != 1)
            {
                error = "OME-Zarr array metadata is unsupported";
                return false;
            }
            if (dataType == "uint8")
            {
                frame.pixelType = PixelType::UInt8;
                frame.significantBits = 8;
            }
            else if (dataType == "uint16")
            {
                frame.pixelType = PixelType::UInt16;
                frame.significantBits = 16;
            }
            else
            {
                error = "OME-Zarr pixel type is unsupported";
                return false;
            }
            std::string groupMetadata;
            if (!readText(location.dataPath.parent_path() / "zarr.json",
                          groupMetadata,
                          error))
            {
                return false;
            }
            int significantBits = 0;
            if (jsonInteger(groupMetadata, "significantBits", significantBits)
                && significantBits > 0 && significantBits <= frame.significantBits)
            {
                frame.significantBits = significantBits;
            }
            if (location.t < 0 || location.c < 0 || location.z < 0
                || location.t >= shape[0] || location.c >= shape[1] || location.z >= shape[2]
                || shape[3] <= 0 || shape[4] <= 0
                || shape[3] > (std::numeric_limits<int>::max)()
                || shape[4] > (std::numeric_limits<int>::max)()
                || chunkShape[3] <= 0 || chunkShape[4] <= 0
                || chunkShape[3] > (std::numeric_limits<int>::max)()
                || chunkShape[4] > (std::numeric_limits<int>::max)()
                || shardShape[3] < chunkShape[3] || shardShape[4] < chunkShape[4]
                || shardShape[3] % chunkShape[3] != 0
                || shardShape[4] % chunkShape[4] != 0
                || shardShape[3] / chunkShape[3] > (std::numeric_limits<int>::max)()
                || shardShape[4] / chunkShape[4] > (std::numeric_limits<int>::max)())
            {
                error = "OME-Zarr frame coordinates or layout are invalid";
                return false;
            }

            frame.width = static_cast<int>(shape[4]);
            frame.height = static_cast<int>(shape[3]);
            const std::size_t sampleBytes = frame.pixelType == PixelType::UInt8 ? 1u : 2u;
            const std::size_t stride = static_cast<std::size_t>(frame.width) * sampleBytes;
            if (stride > (std::numeric_limits<std::size_t>::max)()
                             / static_cast<std::size_t>(frame.height))
            {
                error = "OME-Zarr frame is too large";
                return false;
            }
            frame.bytes.assign(stride * static_cast<std::size_t>(frame.height), 0);
            frame.metadata.frameIndex = location.frameIndex;
            frame.metadata.stride = stride;
            frame.metadata.sourceRoiWidth = frame.width;
            frame.metadata.sourceRoiHeight = frame.height;
            frame.metadata.t = location.t;
            frame.metadata.c = location.c;
            frame.metadata.z = location.z;

            const int chunkHeight = static_cast<int>(chunkShape[3]);
            const int chunkWidth = static_cast<int>(chunkShape[4]);
            const int shardChunksY = static_cast<int>(shardShape[3] / chunkShape[3]);
            const int shardChunksX = static_cast<int>(shardShape[4] / chunkShape[4]);
            const int chunksY = (frame.height - 1) / chunkHeight + 1;
            const int chunksX = (frame.width - 1) / chunkWidth + 1;
            const int shardsY = (chunksY - 1) / shardChunksY + 1;
            const int shardsX = (chunksX - 1) / shardChunksX + 1;
            if (static_cast<std::size_t>(shardChunksY)
                > (std::numeric_limits<std::size_t>::max)()
                    / static_cast<std::size_t>(shardChunksX))
            {
                error = "OME-Zarr shard index is too large";
                return false;
            }
            const std::size_t chunksPerShard = static_cast<std::size_t>(shardChunksY)
                * static_cast<std::size_t>(shardChunksX);
            if (chunksPerShard > ((std::numeric_limits<std::size_t>::max)() - 4) / 16)
            {
                error = "OME-Zarr shard index is too large";
                return false;
            }
            const std::size_t indexBytes = chunksPerShard * 16 + 4;
            if (static_cast<std::size_t>(chunkHeight)
                > (std::numeric_limits<std::size_t>::max)()
                    / static_cast<std::size_t>(chunkWidth) / sampleBytes)
            {
                error = "OME-Zarr chunk is too large";
                return false;
            }
            const std::size_t chunkBytes = static_cast<std::size_t>(chunkHeight)
                * static_cast<std::size_t>(chunkWidth) * sampleBytes;
            const bool compressed = metadata.find("\"name\":\"zstd\"", sharding)
                != std::string::npos;
            std::vector<std::uint8_t> decoded(chunkBytes);
            std::vector<std::uint8_t> encoded;

            for (int shardY = 0; shardY < shardsY; ++shardY)
            {
                for (int shardX = 0; shardX < shardsX; ++shardX)
                {
                    const std::filesystem::path shardPath = location.dataPath / "c"
                        / std::to_string(location.t) / std::to_string(location.c)
                        / std::to_string(location.z) / std::to_string(shardY)
                        / std::to_string(shardX);
                    std::error_code filesystemError;
                    const bool shardExists = std::filesystem::exists(shardPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to inspect OME-Zarr shard";
                        return false;
                    }
                    if (!shardExists)
                    {
                        continue;
                    }
                    const std::uintmax_t shardSize = std::filesystem::file_size(shardPath,
                                                                                filesystemError);
                    if (filesystemError
                        || shardSize < indexBytes
                        || shardSize > static_cast<std::uintmax_t>(
                            (std::numeric_limits<std::streamoff>::max)())
                        || indexBytes > static_cast<std::size_t>(
                            (std::numeric_limits<std::streamsize>::max)()))
                    {
                        error = "OME-Zarr shard is incomplete";
                        return false;
                    }
                    const std::uint64_t payloadBytes = static_cast<std::uint64_t>(
                        shardSize - indexBytes);
                    std::ifstream shard(shardPath, std::ios::binary);
                    std::vector<std::uint8_t> index(indexBytes);
                    if (!shard)
                    {
                        error = "Failed to open OME-Zarr shard";
                        return false;
                    }
                    shard.seekg(static_cast<std::streamoff>(payloadBytes));
                    shard.read(reinterpret_cast<char*>(index.data()),
                               static_cast<std::streamsize>(index.size()));
                    if (!shard
                        || shard.gcount() != static_cast<std::streamsize>(index.size()))
                    {
                        error = "Failed to load OME-Zarr shard index";
                        return false;
                    }
                    if (littleEndian32(index.data() + chunksPerShard * 16)
                        != crc32c::Crc32c(index.data(), chunksPerShard * 16))
                    {
                        error = "OME-Zarr shard index checksum is invalid";
                        return false;
                    }
                    for (int localY = 0; localY < shardChunksY; ++localY)
                    {
                        for (int localX = 0; localX < shardChunksX; ++localX)
                        {
                            const int chunkY = shardY * shardChunksY + localY;
                            const int chunkX = shardX * shardChunksX + localX;
                            if (chunkY >= chunksY || chunkX >= chunksX)
                            {
                                continue;
                            }
                            const std::size_t chunkIndex = static_cast<std::size_t>(localY)
                                * static_cast<std::size_t>(shardChunksX)
                                + static_cast<std::size_t>(localX);
                            const std::uint64_t offset = littleEndian64(index.data()
                                                                       + chunkIndex * 16);
                            const std::uint64_t extent = littleEndian64(index.data()
                                                                       + chunkIndex * 16 + 8);
                            if (offset == kUnwrittenChunk && extent == kUnwrittenChunk)
                            {
                                continue;
                            }
                            if (offset == kUnwrittenChunk || extent == kUnwrittenChunk
                                || offset > payloadBytes || extent > payloadBytes - offset
                                || offset > static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::streamoff>::max)())
                                || extent > static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::streamsize>::max)())
                                || extent > static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::size_t>::max)()))
                            {
                                error = "OME-Zarr shard index entry is invalid";
                                return false;
                            }
                            shard.seekg(static_cast<std::streamoff>(offset));
                            if (compressed)
                            {
                                encoded.resize(static_cast<std::size_t>(extent));
                                shard.read(reinterpret_cast<char*>(encoded.data()),
                                           static_cast<std::streamsize>(encoded.size()));
                                if (!shard
                                    || shard.gcount()
                                        != static_cast<std::streamsize>(encoded.size()))
                                {
                                    error = "Failed to load OME-Zarr chunk";
                                    return false;
                                }
                                const std::size_t result = ZSTD_decompress(
                                    decoded.data(), decoded.size(), encoded.data(), encoded.size());
                                if (ZSTD_isError(result) || result != decoded.size())
                                {
                                    error = "Failed to decompress OME-Zarr chunk";
                                    return false;
                                }
                            }
                            else
                            {
                                if (extent != chunkBytes)
                                {
                                    error = "OME-Zarr chunk size is invalid";
                                    return false;
                                }
                                shard.read(reinterpret_cast<char*>(decoded.data()),
                                           static_cast<std::streamsize>(decoded.size()));
                                if (!shard
                                    || shard.gcount()
                                        != static_cast<std::streamsize>(decoded.size()))
                                {
                                    error = "Failed to load OME-Zarr chunk";
                                    return false;
                                }
                            }
                            if constexpr (std::endian::native == std::endian::big)
                            {
                                if (sampleBytes == 2)
                                {
                                    for (std::size_t byte = 0; byte < decoded.size(); byte += 2)
                                    {
                                        std::swap(decoded[byte], decoded[byte + 1]);
                                    }
                                }
                            }
                            const int destinationX = chunkX * chunkWidth;
                            const int destinationY = chunkY * chunkHeight;
                            const int copyWidth = (std::min)(chunkWidth,
                                                            frame.width - destinationX);
                            const int copyHeight = (std::min)(chunkHeight,
                                                             frame.height - destinationY);
                            for (int row = 0; row < copyHeight; ++row)
                            {
                                std::memcpy(frame.bytes.data()
                                                + static_cast<std::size_t>(destinationY + row)
                                                    * stride
                                                + static_cast<std::size_t>(destinationX)
                                                    * sampleBytes,
                                            decoded.data()
                                                + static_cast<std::size_t>(row)
                                                    * static_cast<std::size_t>(chunkWidth)
                                                    * sampleBytes,
                                            static_cast<std::size_t>(copyWidth) * sampleBytes);
                            }
                        }
                    }
                }
            }
            return true;
        }
    }

    // Dispatch frame access to the selected dataset format
    bool datasetFrame(const DatasetFrameLocation& location,
                      DatasetFrame& frame,
                      std::string& error)
    {
        frame = DatasetFrame{};
        error.clear();
        if (location.dataPath.empty())
        {
            error = "Dataset path is required";
            return false;
        }
        if (location.format == Format::OmeZarr)
        {
            return zarrFrame(location, frame, error);
        }
        if (location.format == Format::Binary)
        {
            if (location.frameMetadataPath.empty())
            {
                error = "Binary frame metadata path is required";
                return false;
            }
            return binaryFrame(location, frame, error);
        }
        return tiffFrame(location, frame, error);
    }
}
