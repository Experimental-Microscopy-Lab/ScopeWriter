// Implements the public writer and its TIFF and binary backends

#include "scopewriter/ScopeWriter.h"

#include "ZarrWriter.h"

#include <tiffio.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace scopewriter
{
    // Return the bundled libtiff version
    std::string libTiffVersion()
    {
        std::string version = TIFFGetVersion();
        version = version.substr(0, version.find('\n'));
        constexpr char prefix[] = "LIBTIFF, Version ";
        if (version.starts_with(prefix))
        {
            version.erase(0, sizeof(prefix) - 1);
        }
        return version;
    }

    // Return the bundled zlib version
    std::string zlibVersion()
    {
        return ::zlibVersion();
    }

    namespace
    {
        std::string number(double value)
        {
            std::ostringstream stream;
            stream << std::setprecision(17) << value;
            return stream.str();
        }

        std::string xmlEscape(const std::string& value)
        {
            std::string output;
            output.reserve(value.size());
            for (const char character : value)
            {
                switch (character)
                {
                case '&': output += "&amp;"; break;
                case '<': output += "&lt;"; break;
                case '>': output += "&gt;"; break;
                case '\"': output += "&quot;"; break;
                case '\'': output += "&apos;"; break;
                default: output += character; break;
                }
            }
            return output;
        }

        std::string jsonEscape(const std::string& value)
        {
            std::ostringstream output;
            output << '\"';
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '\"': output << "\\\""; break;
                case '\\': output << "\\\\"; break;
                case '\b': output << "\\b"; break;
                case '\f': output << "\\f"; break;
                case '\n': output << "\\n"; break;
                case '\r': output << "\\r"; break;
                case '\t': output << "\\t"; break;
                default:
                    if (character < 0x20)
                    {
                        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<int>(character) << std::dec;
                    }
                    else
                    {
                        output << static_cast<char>(character);
                    }
                    break;
                }
            }
            output << '\"';
            return output.str();
        }

        std::string csvField(const std::string& value)
        {
            if (value.find_first_of(",\"\n\r") == std::string::npos)
            {
                return value;
            }
            std::string escaped;
            escaped.reserve(value.size() + 2);
            escaped += '"';
            for (const char character : value)
            {
                if (character == '"') escaped += '"';
                escaped += character;
            }
            escaped += '"';
            return escaped;
        }

        std::string pathUtf8(const std::filesystem::path& path)
        {
            const auto encoded = path.generic_u8string();
            return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
        }

        std::string isoTimestamp(std::uint64_t timestampNs)
        {
            if (timestampNs == 0)
            {
                return {};
            }
            const auto milliseconds = static_cast<std::int64_t>(timestampNs / 1000000ull);
            const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
            std::tm utc{};
#if defined(_WIN32)
            if (gmtime_s(&utc, &seconds) != 0)
            {
                return {};
            }
#else
            if (gmtime_r(&seconds, &utc) == nullptr)
            {
                return {};
            }
#endif
            std::ostringstream output;
            output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
                   << std::setw(3) << std::setfill('0') << milliseconds % 1000 << 'Z';
            return output.str();
        }

        std::string createUuid()
        {
            std::array<unsigned char, 16> bytes{};
            std::random_device device;
            for (auto& byte : bytes)
            {
                byte = static_cast<unsigned char>(device());
            }
            bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
            bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

            std::ostringstream output;
            output << "urn:uuid:" << std::hex << std::setfill('0');
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    output << '-';
                }
                output << std::setw(2) << static_cast<int>(bytes[index]);
            }
            return output.str();
        }

        bool validUuid(const std::string& value)
        {
            if (value.size() != 45 || value.compare(0, 9, "urn:uuid:") != 0)
            {
                return false;
            }
            for (std::size_t index = 9; index < value.size(); ++index)
            {
                const std::size_t uuidIndex = index - 9;
                const bool separator = uuidIndex == 8 || uuidIndex == 13
                    || uuidIndex == 18 || uuidIndex == 23;
                const unsigned char character = static_cast<unsigned char>(value[index]);
                if ((separator && character != '-')
                    || (!separator && !std::isxdigit(character)))
                {
                    return false;
                }
            }
            return true;
        }

        std::size_t bytesPerPixel(PixelType type)
        {
            return type == PixelType::UInt8 ? 1u : 2u;
        }

        int storageBits(PixelType type)
        {
            return type == PixelType::UInt8 ? 8 : 16;
        }

        const char* pixelFormatName(PixelType type)
        {
            return type == PixelType::UInt8 ? "Mono8" : "Mono16";
        }

        unsigned int pixelFormatId(PixelType type)
        {
            return type == PixelType::UInt8 ? 0u : 1u;
        }

        int axisCount(const WriterSettings& settings, char axis)
        {
            switch (axis)
            {
            case 'T': return settings.timeCount;
            case 'C': return settings.channelCount;
            case 'Z': return settings.zCount;
            default: return 0;
            }
        }

        bool sequenceIndex(const WriterSettings& settings,
                           const FrameMetadata& metadata,
                           std::int64_t& index)
        {
            index = 0;
            for (const char axis : settings.acquisitionOrder)
            {
                const std::int64_t coordinate = axis == 'T'
                    ? metadata.t
                    : axis == 'C' ? metadata.c : metadata.z;
                const int count = axisCount(settings, axis);
                if (count > 0)
                {
                    if (index > ((std::numeric_limits<std::int64_t>::max)() - coordinate)
                                    / count)
                    {
                        return false;
                    }
                    index *= count;
                }
                index += coordinate;
            }
            return true;
        }

        bool planeCapacity(const WriterSettings& settings, std::int64_t& capacity)
        {
            if (settings.timeCount == 0)
            {
                capacity = -1;
                return true;
            }
            capacity = 1;
            for (const char axis : settings.acquisitionOrder)
            {
                const int count = axisCount(settings, axis);
                if (capacity > (std::numeric_limits<std::int64_t>::max)() / count)
                {
                    return false;
                }
                capacity *= count;
            }
            return true;
        }

        FrameMetadata coordinatesForSequenceIndex(const WriterSettings& settings,
                                                   std::int64_t index,
                                                   int positionIndex)
        {
            FrameMetadata metadata;
            metadata.positionIndex = positionIndex;
            for (auto iterator = settings.acquisitionOrder.rbegin();
                 iterator != settings.acquisitionOrder.rend(); ++iterator)
            {
                const int count = axisCount(settings, *iterator);
                const std::int64_t coordinate = count > 0 ? index % count : index;
                if (count > 0)
                {
                    index /= count;
                }
                if (*iterator == 'T')
                    metadata.t = coordinate;
                else if (*iterator == 'C')
                    metadata.c = static_cast<int>(coordinate);
                else
                    metadata.z = static_cast<int>(coordinate);
            }
            return metadata;
        }

        std::string seriesName(const WriterSettings& settings, int positionIndex)
        {
            if (settings.positionCount == 1)
            {
                return {};
            }
            if (settings.positions.empty())
            {
                return std::to_string(positionIndex);
            }
            const auto& position = settings.positions[static_cast<std::size_t>(positionIndex)];
            std::string name = position.name;
            if (position.gridRow && position.gridColumn)
            {
                name += '_' + std::to_string(*position.gridRow)
                    + '_' + std::to_string(*position.gridColumn);
            }
            return name;
        }

        std::filesystem::path multiTiffRoot(const std::filesystem::path& outputPath)
        {
            std::string name = outputPath.filename().string();
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            const std::array<std::string, 2> suffixes{".ome.tiff", ".ome.tif"};
            for (const auto& suffix : suffixes)
            {
                if (lower.size() >= suffix.size()
                    && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    name.erase(name.size() - suffix.size());
                    return outputPath.parent_path() / name;
                }
            }
            return outputPath;
        }

        std::filesystem::path positionTiffPath(const std::filesystem::path& root,
                                               int positionIndex)
        {
            std::ostringstream name;
            name << root.filename().string() << "_p" << std::setw(3) << std::setfill('0')
                 << positionIndex << ".ome.tiff";
            return root / name.str();
        }

        // Validate shared settings before selecting a backend
        bool validSettings(const WriterSettings& settings, std::string& error)
        {
            if (settings.outputPath.empty())
            {
                error = "Output path is required";
                return false;
            }
            if (settings.format == Format::Binary
                && (settings.frameMetadataPath.empty()
                    || settings.frameMetadataPath == settings.outputPath))
            {
                error = "Binary output requires a distinct frame metadata path";
                return false;
            }
            if (!settings.uuid.empty() && !validUuid(settings.uuid))
            {
                error = "UUID must be a canonical urn:uuid value";
                return false;
            }
            if (settings.width <= 0 || settings.height <= 0)
            {
                error = "Image dimensions must be positive";
                return false;
            }
            const auto width = static_cast<std::size_t>(settings.width);
            const auto height = static_cast<std::size_t>(settings.height);
            const std::size_t sampleBytes = bytesPerPixel(settings.pixelType);
            if (width > (std::numeric_limits<std::size_t>::max)() / height
                || width * height > (std::numeric_limits<std::size_t>::max)() / sampleBytes)
            {
                error = "Image dimensions exceed the supported frame size";
                return false;
            }
            const std::size_t frameBytes = width * height * sampleBytes;
            const int bits = storageBits(settings.pixelType);
            if (settings.significantBits <= 0 || settings.significantBits > bits)
            {
                error = "Significant bits must fit the selected pixel type";
                return false;
            }
            if (settings.positionCount <= 0 || settings.timeCount < 0
                || settings.channelCount <= 0
                || settings.zCount <= 0)
            {
                error = "Position, channel and Z counts must be positive and time count cannot be negative";
                return false;
            }
            std::string sortedOrder = settings.acquisitionOrder;
            std::sort(sortedOrder.begin(), sortedOrder.end());
            if (sortedOrder != "CTZ"
                || (settings.timeCount == 0 && settings.acquisitionOrder.front() != 'T'))
            {
                error = "Acquisition order must contain T, C and Z once and an unbounded T must be first";
                return false;
            }
            if ((!settings.channels.empty()
                 && settings.channels.size()
                     != static_cast<std::size_t>(settings.channelCount))
                || (!settings.positions.empty()
                    && settings.positions.size()
                        != static_cast<std::size_t>(settings.positionCount)))
            {
                error = "Channel and position metadata must match the configured counts";
                return false;
            }
            for (const auto& channel : settings.channels)
            {
                if ((channel.excitationWavelengthNm
                     && (!std::isfinite(*channel.excitationWavelengthNm)
                         || *channel.excitationWavelengthNm <= 0.0))
                    || (channel.emissionWavelengthNm
                        && (!std::isfinite(*channel.emissionWavelengthNm)
                            || *channel.emissionWavelengthNm <= 0.0)))
                {
                    error = "Channel wavelengths must be finite positive values";
                    return false;
                }
            }
            std::vector<std::string> channelNames;
            channelNames.reserve(settings.channels.size());
            for (const auto& channel : settings.channels)
            {
                if (channel.name.empty()
                    || std::find(channelNames.begin(), channelNames.end(), channel.name)
                        != channelNames.end())
                {
                    error = "Channel names must be non-empty and unique";
                    return false;
                }
                channelNames.push_back(channel.name);
            }
            const std::array<double, 5> nonNegativeValues{
                settings.physicalSizeXUm,
                settings.physicalSizeYUm,
                settings.physicalSizeZUm,
                settings.timeIncrementMs,
                settings.defaultExposureMs
            };
            if (std::any_of(nonNegativeValues.begin(), nonNegativeValues.end(),
                            [](double value)
                            {
                                return !std::isfinite(value) || value < 0.0;
                            })
                || (settings.detector.offset && !std::isfinite(*settings.detector.offset)))
            {
                error = "Physical sizes, timing and detector metadata contain an invalid value";
                return false;
            }
            std::vector<std::string> seriesNames;
            seriesNames.reserve(static_cast<std::size_t>(settings.positionCount));
            for (int index = 0; index < settings.positionCount; ++index)
            {
                if (!settings.positions.empty())
                {
                    const auto& position = settings.positions[static_cast<std::size_t>(index)];
                    if (position.name.empty()
                        || position.gridRow.has_value() != position.gridColumn.has_value()
                        || (position.xUm && !std::isfinite(*position.xUm))
                        || (position.yUm && !std::isfinite(*position.yUm))
                        || (position.zUm && !std::isfinite(*position.zUm)))
                    {
                        error = "Position metadata contains an invalid value";
                        return false;
                    }
                }
                const std::string name = seriesName(settings, index);
                if (settings.positionCount > 1
                    && (name.empty() || name == "." || name == ".."
                        || name.find('/') != std::string::npos
                        || name.find('\\') != std::string::npos
                        || std::find(seriesNames.begin(), seriesNames.end(), name)
                            != seriesNames.end()))
                {
                    error = "Position series names must be unique path components";
                    return false;
                }
                seriesNames.push_back(name);
            }
            for (const auto& [nameSpace, values] : settings.metadata)
            {
                if (nameSpace.empty()
                    || std::any_of(values.begin(), values.end(), [](const auto& entry)
                    {
                        return entry.first.empty();
                    }))
                {
                    error = "Metadata namespaces and keys cannot be empty";
                    return false;
                }
            }
            if (settings.compressionLevel < 0 || settings.compressionLevel > 9)
            {
                error = "Compression level must be between 0 and 9";
                return false;
            }
            if (settings.format == Format::OmeZarr
                && (settings.zarrChunkWidth <= 0 || settings.zarrChunkHeight <= 0
                    || settings.zarrShardWidthChunks < 0
                    || settings.zarrShardHeightChunks < 0
                    || (settings.zarrShardWidthChunks > 0
                        && settings.zarrShardWidthChunks
                            > (std::numeric_limits<int>::max)() / settings.zarrChunkWidth)
                    || (settings.zarrShardHeightChunks > 0
                        && settings.zarrShardHeightChunks
                            > (std::numeric_limits<int>::max)() / settings.zarrChunkHeight)))
            {
                error = "OME-Zarr chunk and shard dimensions are invalid";
                return false;
            }
            if (settings.format == Format::OmeZarr
                && settings.zarrMaxQueuedFrameBytes > 0
                && settings.zarrMaxQueuedFrameBytes < frameBytes)
            {
                error = "OME-Zarr queued frame byte limit is smaller than one frame";
                return false;
            }
            if (settings.format == Format::OmeZarr
                && settings.zarrWorkerCount
                    > (std::max)(1u, std::thread::hardware_concurrency()))
            {
                error = "OME-Zarr worker count exceeds the available hardware threads";
                return false;
            }
            std::int64_t capacity = 0;
            if (!planeCapacity(settings, capacity))
            {
                error = "Configured dimensions exceed the supported plane count";
                return false;
            }
            return true;
        }

        // Validate frame coordinates and metadata
        bool validFrame(const WriterSettings& settings,
                        const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& metadata,
                        std::string& error)
        {
            const auto width = static_cast<std::size_t>(settings.width);
            const auto height = static_cast<std::size_t>(settings.height);
            const std::size_t sampleBytes = bytesPerPixel(settings.pixelType);
            if (width > (std::numeric_limits<std::size_t>::max)() / height
                || width * height > (std::numeric_limits<std::size_t>::max)() / sampleBytes)
            {
                error = "Image dimensions exceed the supported frame size";
                return false;
            }
            const std::size_t expected = width * height * sampleBytes;
            if (data == nullptr || byteCount != expected)
            {
                error = "Frame byte count does not match the configured image";
                return false;
            }
            if (metadata.positionIndex < 0 || metadata.positionIndex >= settings.positionCount
                || metadata.c < 0 || metadata.c >= settings.channelCount
                || metadata.z < 0 || metadata.z >= settings.zCount || metadata.t < -1
                || (settings.timeCount > 0 && metadata.t >= settings.timeCount))
            {
                error = "Frame coordinates are outside the configured dataset";
                return false;
            }
            if (!std::isfinite(metadata.exposureMs) || metadata.exposureMs < 0.0
                || (metadata.positionXUm && !std::isfinite(*metadata.positionXUm))
                || (metadata.positionYUm && !std::isfinite(*metadata.positionYUm))
                || (metadata.positionZUm && !std::isfinite(*metadata.positionZUm))
                || std::any_of(metadata.metadata.begin(), metadata.metadata.end(),
                               [](const auto& entry)
                               {
                                   return entry.first.empty();
                               }))
            {
                error = "Frame metadata contains an invalid value";
                return false;
            }
            return true;
        }

        std::uint32_t tiffRowsPerStrip(const WriterSettings& settings)
        {
            constexpr std::size_t targetBytes = 64u * 1024u;
            const std::size_t rowBytes = static_cast<std::size_t>(settings.width)
                * bytesPerPixel(settings.pixelType);
            const std::size_t rows = (std::clamp)(
                targetBytes / rowBytes,
                std::size_t{1},
                static_cast<std::size_t>(settings.height));
            return static_cast<std::uint32_t>(rows);
        }

        // Configure one TIFF image directory
        void configureTiffDirectory(TIFF* tiff,
                                    const WriterSettings& settings,
                                    const char* description)
        {
            TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, settings.width);
            TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, settings.height);
            TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, storageBits(settings.pixelType));
            TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
            TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
            TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, tiffRowsPerStrip(settings));
            if (description != nullptr)
            {
                TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION, description);
            }
            if (settings.enableCompression)
            {
                TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
                TIFFSetField(tiff, TIFFTAG_ZIPQUALITY, settings.compressionLevel);
                TIFFSetField(tiff, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
            }
            else
            {
                TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            }
        }

        // Write one image using bounded TIFF strips
        bool writeTiffStrips(TIFF* tiff,
                             const WriterSettings& settings,
                             const void* data,
                             std::string& error)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(settings.width)
                * bytesPerPixel(settings.pixelType);
            const std::size_t rowsPerStrip = tiffRowsPerStrip(settings);
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            tstrip_t strip = 0;
            for (std::size_t row = 0; row < static_cast<std::size_t>(settings.height);
                 row += rowsPerStrip, ++strip)
            {
                const std::size_t rows = (std::min)(
                    rowsPerStrip,
                    static_cast<std::size_t>(settings.height) - row);
                if (TIFFWriteEncodedStrip(tiff,
                                          strip,
                                          const_cast<std::uint8_t*>(bytes + row * rowBytes),
                                          static_cast<tmsize_t>(rows * rowBytes)) == -1)
                {
                    error = "Failed to write a TIFF strip";
                    return false;
                }
            }
            return true;
        }

        // Validate binary frame layout before writing
        bool validBinaryFrame(const WriterSettings& settings,
                              const void* data,
                              std::size_t byteCount,
                              const FrameMetadata& metadata,
                              std::string& error)
        {
            const std::size_t packedRowBytes = static_cast<std::size_t>(settings.width)
                * bytesPerPixel(settings.pixelType);
            if (data == nullptr || byteCount == 0 || metadata.stride < packedRowBytes
                || metadata.stride > (std::numeric_limits<std::size_t>::max)()
                    / static_cast<std::size_t>(settings.height)
                || byteCount != metadata.stride * static_cast<std::size_t>(settings.height))
            {
                error = "Binary frame payload does not match its stride and dimensions";
                return false;
            }
            return true;
        }

        struct Plane
        {
            int ifd{0};
            FrameMetadata metadata;
        };

        struct TiffFileMetadata
        {
            int position{0};
            std::filesystem::path path;
            std::string uuid;
            const std::vector<Plane>* planes{nullptr};
        };

        // Build OME XML for the current set of planes
        std::string buildOmeXml(const WriterSettings& settings,
                                const std::vector<TiffFileMetadata>& files,
                                const std::string& rootUuid)
        {
            struct Series
            {
                int position{0};
                int sizeZ{1};
                std::int64_t sizeT{1};
                const TiffFileMetadata* file{nullptr};
                std::vector<const Plane*> planes;
            };

            std::vector<Series> series;
            series.reserve(files.size());
            for (const auto& file : files)
            {
                Series current;
                current.position = file.position;
                current.sizeZ = settings.zCount;
                current.sizeT = settings.timeCount > 0 ? settings.timeCount : 1;
                current.file = &file;
                if (file.planes != nullptr)
                {
                    for (const auto& plane : *file.planes)
                    {
                        current.sizeT = (std::max)(current.sizeT, plane.metadata.t + 1);
                        current.planes.push_back(&plane);
                    }
                }
                series.push_back(std::move(current));
            }

            const bool hasDetector = !settings.detector.manufacturer.empty()
                || !settings.detector.model.empty()
                || !settings.detector.serialNumber.empty()
                || settings.detector.offset.has_value();
            const bool hasLinkedMetadata = !settings.linkedMetadataFile.empty();
            const std::string acquisitionDate = isoTimestamp(
                settings.acquisitionStartTimestampNs);

            std::ostringstream xml;
            xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                << "<!-- Warning: this comment is an OME-XML metadata block, which contains crucial dimensional parameters and other important metadata. Please edit cautiously. -->\n"
                << "<OME xmlns=\"http://www.openmicroscopy.org/Schemas/OME/2016-06\""
                << " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
                << " xsi:schemaLocation=\"http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd\""
                << " Creator=\"" << xmlEscape(settings.creator) << "\""
                << " UUID=\"" << xmlEscape(rootUuid) << "\">\n";

            if (hasDetector)
            {
                xml << "  <Instrument ID=\"Instrument:0\">\n"
                    << "    <Detector ID=\"Detector:0\"";
                if (!settings.detector.manufacturer.empty())
                    xml << " Manufacturer=\"" << xmlEscape(settings.detector.manufacturer) << "\"";
                if (!settings.detector.model.empty())
                    xml << " Model=\"" << xmlEscape(settings.detector.model) << "\"";
                if (!settings.detector.serialNumber.empty())
                    xml << " SerialNumber=\"" << xmlEscape(settings.detector.serialNumber) << "\"";
                if (settings.detector.offset)
                    xml << " Offset=\"" << number(*settings.detector.offset) << "\"";
                xml << "/>\n  </Instrument>\n";
            }

            for (std::size_t index = 0; index < series.size(); ++index)
            {
                const auto& current = series[index];
                const PositionMetadata* position = settings.positions.empty()
                    ? nullptr
                    : &settings.positions[static_cast<std::size_t>(current.position)];
                const std::string positionName = position != nullptr && !position->name.empty()
                    ? position->name
                    : "Position " + std::to_string(current.position + 1);
                const std::string name = settings.positionCount == 1
                    ? settings.imageName
                    : settings.imageName + " " + positionName;
                xml << "  <Image ID=\"Image:" << current.position << "\" Name=\""
                    << xmlEscape(name) << "\">\n";
                if (!acquisitionDate.empty())
                    xml << "    <AcquisitionDate>" << acquisitionDate << "</AcquisitionDate>\n";
                if (hasDetector)
                    xml << "    <InstrumentRef ID=\"Instrument:0\"/>\n";

                xml << "    <StageLabel Name=\"" << xmlEscape(positionName) << "\"";
                const auto stage = std::find_if(current.planes.begin(), current.planes.end(),
                    [](const Plane* plane)
                    {
                        return plane->metadata.positionXUm || plane->metadata.positionYUm
                            || plane->metadata.positionZUm;
                    });
                const auto positionX = position != nullptr && position->xUm
                    ? position->xUm
                    : stage != current.planes.end() ? (*stage)->metadata.positionXUm : std::nullopt;
                const auto positionY = position != nullptr && position->yUm
                    ? position->yUm
                    : stage != current.planes.end() ? (*stage)->metadata.positionYUm : std::nullopt;
                const auto positionZ = position != nullptr && position->zUm
                    ? position->zUm
                    : stage != current.planes.end() ? (*stage)->metadata.positionZUm : std::nullopt;
                if (positionX)
                {
                    xml << " X=\"" << number(*positionX) << "\" XUnit=\"µm\"";
                }
                if (positionY)
                    xml << " Y=\"" << number(*positionY) << "\" YUnit=\"µm\"";
                if (positionZ)
                    xml << " Z=\"" << number(*positionZ) << "\" ZUnit=\"µm\"";
                xml << "/>\n";

                xml << "    <Pixels ID=\"Pixels:" << current.position
                    << "\" DimensionOrder=\"XYZCT\" Type=\""
                    << (settings.pixelType == PixelType::UInt8 ? "uint8" : "uint16")
                    << "\" SizeX=\"" << settings.width
                    << "\" SizeY=\"" << settings.height
                    << "\" SizeZ=\"" << current.sizeZ
                    << "\" SizeC=\"" << settings.channelCount
                    << "\" SizeT=\"" << current.sizeT
                    << "\" BigEndian=\"false\" Interleaved=\"false\"";
                if (settings.physicalSizeXUm > 0.0)
                    xml << " PhysicalSizeX=\"" << number(settings.physicalSizeXUm)
                        << "\" PhysicalSizeXUnit=\"µm\"";
                if (settings.physicalSizeYUm > 0.0)
                    xml << " PhysicalSizeY=\"" << number(settings.physicalSizeYUm)
                        << "\" PhysicalSizeYUnit=\"µm\"";
                if (settings.physicalSizeZUm > 0.0)
                    xml << " PhysicalSizeZ=\"" << number(settings.physicalSizeZUm)
                        << "\" PhysicalSizeZUnit=\"µm\"";
                if (settings.timeIncrementMs > 0.0)
                    xml << " TimeIncrement=\"" << number(settings.timeIncrementMs)
                        << "\" TimeIncrementUnit=\"ms\"";
                xml << " SignificantBits=\"" << settings.significantBits << "\">\n";
                for (int channelIndex = 0; channelIndex < settings.channelCount; ++channelIndex)
                {
                    const ChannelMetadata* channel = settings.channels.empty()
                        ? nullptr
                        : &settings.channels[static_cast<std::size_t>(channelIndex)];
                    const std::string channelName = channel != nullptr && !channel->name.empty()
                        ? channel->name
                        : "Channel " + std::to_string(channelIndex + 1);
                    xml << "      <Channel ID=\"Channel:" << current.position << ':'
                        << channelIndex
                        << "\" Name=\"" << xmlEscape(channelName) << "\"";
                    if (channel != nullptr && !channel->fluorophore.empty())
                        xml << " Fluor=\"" << xmlEscape(channel->fluorophore) << "\"";
                    if (channel != nullptr && channel->excitationWavelengthNm)
                        xml << " ExcitationWavelength=\""
                            << number(*channel->excitationWavelengthNm)
                            << "\" ExcitationWavelengthUnit=\"nm\"";
                    if (channel != nullptr && channel->emissionWavelengthNm)
                        xml << " EmissionWavelength=\""
                            << number(*channel->emissionWavelengthNm)
                            << "\" EmissionWavelengthUnit=\"nm\"";
                    if (channel != nullptr && channel->colorRGB)
                    {
                        const std::uint32_t rgba = ((*channel->colorRGB & 0x00ffffffu) << 8u)
                            | 0xffu;
                        xml << " Color=\"" << static_cast<std::int32_t>(rgba) << "\"";
                    }
                    xml << " SamplesPerPixel=\"1\"/>\n";
                }

                for (const Plane* plane : current.planes)
                {
                    xml << "      <TiffData IFD=\"" << plane->ifd
                        << "\" FirstZ=\"" << plane->metadata.z
                        << "\" FirstC=\"" << plane->metadata.c
                        << "\" FirstT=\"" << plane->metadata.t
                        << "\" PlaneCount=\"1\"";
                    if (files.size() == 1)
                    {
                        xml << "/>\n";
                    }
                    else
                    {
                        xml << "><UUID FileName=\""
                            << xmlEscape(pathUtf8(current.file->path.filename())) << "\">"
                            << xmlEscape(current.file->uuid) << "</UUID></TiffData>\n";
                    }
                }
                for (const Plane* plane : current.planes)
                {
                    const auto& metadata = plane->metadata;
                    xml << "      <Plane TheZ=\"" << metadata.z
                        << "\" TheC=\"" << metadata.c
                        << "\" TheT=\"" << metadata.t << "\"";
                    if (settings.acquisitionStartTimestampNs != 0
                        && metadata.timestampNs >= settings.acquisitionStartTimestampNs)
                    {
                        const double deltaMs = static_cast<double>(
                            metadata.timestampNs - settings.acquisitionStartTimestampNs) / 1000000.0;
                        xml << " DeltaT=\"" << number(deltaMs) << "\" DeltaTUnit=\"ms\"";
                    }
                    const double exposure = metadata.exposureMs > 0.0
                                                ? metadata.exposureMs
                                                : settings.defaultExposureMs;
                    if (exposure > 0.0)
                        xml << " ExposureTime=\"" << number(exposure)
                            << "\" ExposureTimeUnit=\"ms\"";
                    if (metadata.positionXUm)
                        xml << " PositionX=\"" << number(*metadata.positionXUm)
                            << "\" PositionXUnit=\"µm\"";
                    if (metadata.positionYUm)
                        xml << " PositionY=\"" << number(*metadata.positionYUm)
                            << "\" PositionYUnit=\"µm\"";
                    if (metadata.positionZUm)
                        xml << " PositionZ=\"" << number(*metadata.positionZUm)
                            << "\" PositionZUnit=\"µm\"";
                    if (metadata.metadata.empty())
                    {
                        xml << "/>\n";
                    }
                    else
                    {
                        xml << ">\n        <AnnotationRef ID=\"Annotation:Plane:"
                            << current.position << ':' << plane->ifd
                            << "\"/>\n      </Plane>\n";
                    }
                }
                xml << "    </Pixels>\n";
                for (std::size_t annotationIndex = 0;
                     annotationIndex < settings.metadata.size();
                     ++annotationIndex)
                {
                    xml << "    <AnnotationRef ID=\"Annotation:Global:"
                        << annotationIndex << "\"/>\n";
                }
                if (hasLinkedMetadata)
                    xml << "    <AnnotationRef ID=\"Annotation:LinkedMetadata\"/>\n";
                xml << "  </Image>\n";
            }
            const bool hasFrameAnnotations = std::any_of(
                series.begin(), series.end(), [](const Series& current)
                {
                    return std::any_of(current.planes.begin(), current.planes.end(),
                        [](const Plane* plane)
                        {
                            return !plane->metadata.metadata.empty();
                        });
                });
            if (!settings.metadata.empty() || hasLinkedMetadata || hasFrameAnnotations)
            {
                xml << "  <StructuredAnnotations>\n";
                std::size_t annotationIndex = 0;
                for (const auto& [nameSpace, values] : settings.metadata)
                {
                    xml << "    <MapAnnotation ID=\"Annotation:Global:"
                        << annotationIndex++ << "\" Namespace=\""
                        << xmlEscape(nameSpace) << "\"><Value>";
                    for (const auto& [key, value] : values)
                    {
                        xml << "<M K=\"" << xmlEscape(key) << "\">"
                            << xmlEscape(value) << "</M>";
                    }
                    xml << "</Value></MapAnnotation>\n";
                }
                if (hasLinkedMetadata)
                {
                    xml << "    <MapAnnotation ID=\"Annotation:LinkedMetadata\""
                        << " Namespace=\"urn:scopewriter:linked-metadata\"><Value>"
                        << "<M K=\"fileName\">"
                        << xmlEscape(settings.linkedMetadataFile)
                        << "</M><M K=\"format\">json</M>"
                        << "</Value></MapAnnotation>\n";
                }
                for (const auto& current : series)
                {
                    for (const Plane* plane : current.planes)
                    {
                        if (plane->metadata.metadata.empty())
                            continue;
                        xml << "    <MapAnnotation ID=\"Annotation:Plane:"
                            << current.position << ':' << plane->ifd
                            << "\" Namespace=\"urn:scopewriter:frame-metadata\"><Value>";
                        for (const auto& [key, value] : plane->metadata.metadata)
                        {
                            xml << "<M K=\"" << xmlEscape(key) << "\">"
                                << xmlEscape(value) << "</M>";
                        }
                        xml << "</Value></MapAnnotation>\n";
                    }
                }
                xml << "  </StructuredAnnotations>\n";
            }
            xml << "</OME>\n";
            return xml.str();
        }

        // Define the common backend lifecycle
        class Backend
        {
        public:
            virtual ~Backend() = default;
            virtual bool open(const WriterSettings& settings, std::string& error) = 0;
            virtual bool append(const void* data,
                                std::size_t byteCount,
                                const FrameMetadata& metadata,
                                std::string& error) = 0;
            virtual bool flush(std::string& error) = 0;
            virtual bool close(std::string& error) = 0;
            virtual bool isOpen() const noexcept = 0;
        };

        // Store plain TIFF frames and embedded metadata
        class TiffBackend final : public Backend
        {
        public:
            ~TiffBackend() override
            {
                std::string ignored;
                close(ignored);
            }

            bool open(const WriterSettings& settings, std::string& error) override
            {
                std::error_code filesystemError;
                if (std::filesystem::exists(settings.outputPath, filesystemError))
                {
                    if (!settings.overwrite)
                    {
                        error = "TIFF output already exists";
                        return false;
                    }
                    std::filesystem::remove(settings.outputPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to replace the existing TIFF output";
                        return false;
                    }
                }
                const auto parent = settings.outputPath.parent_path();
                if (!parent.empty())
                {
                    std::filesystem::create_directories(parent, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to create the TIFF output directory";
                        return false;
                    }
                }
#if defined(_WIN32)
                m_tiff = TIFFOpenW(settings.outputPath.c_str(), "w8");
#else
                const std::string path = pathUtf8(settings.outputPath);
                m_tiff = TIFFOpen(path.c_str(), "w8");
#endif
                if (m_tiff == nullptr)
                {
                    error = "Failed to open the TIFF output";
                    return false;
                }
                m_settings = settings;
                m_framesWritten = 0;
                return true;
            }

            bool append(const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& metadata,
                        std::string& error) override
            {
                if (m_tiff == nullptr)
                {
                    error = "TIFF output is not open";
                    return false;
                }
                if (!validFrame(m_settings, data, byteCount, metadata, error))
                {
                    return false;
                }

                std::ostringstream description;
                description << '{'
                    << "\"schema\":" << jsonEscape(kFrameMetadataProtocol)
                    << ",\"linked_metadata_file\":"
                    << jsonEscape(m_settings.linkedMetadataFile)
                    << ",\"camera_id\":" << jsonEscape(metadata.cameraId)
                    << ",\"frame_index\":" << jsonEscape(std::to_string(metadata.frameIndex))
                    << ",\"timestamp_ns\":" << jsonEscape(std::to_string(metadata.timestampNs))
                    << ",\"bits_per_sample\":" << m_settings.significantBits
                    << ",\"pixel_format\":"
                    << jsonEscape(pixelFormatName(m_settings.pixelType))
                    << ",\"pixel_format_id\":" << pixelFormatId(m_settings.pixelType)
                    << ",\"source_roi_x\":" << metadata.sourceRoiX
                    << ",\"source_roi_y\":" << metadata.sourceRoiY
                    << ",\"source_roi_width\":" << metadata.sourceRoiWidth
                    << ",\"source_roi_height\":" << metadata.sourceRoiHeight
                    << '}';

                TIFFCreateDirectory(m_tiff);
                const std::string descriptionText = description.str();
                configureTiffDirectory(m_tiff, m_settings, descriptionText.c_str());
                if (!writeTiffStrips(m_tiff, m_settings, data, error)
                    || !TIFFWriteDirectory(m_tiff))
                {
                    if (error.empty()) error = "Failed to write the TIFF frame";
                    return false;
                }
                ++m_framesWritten;
                return true;
            }

            bool flush(std::string& error) override
            {
                if (m_tiff == nullptr)
                {
                    error = "TIFF output is not open";
                    return false;
                }
                if (!TIFFFlush(m_tiff))
                {
                    error = "Failed to flush the TIFF output";
                    return false;
                }
                return true;
            }

            bool close(std::string& error) override
            {
                static_cast<void>(error);
                if (m_tiff == nullptr)
                {
                    return true;
                }
                TIFFClose(m_tiff);
                m_tiff = nullptr;
                if (m_framesWritten == 0)
                {
                    std::error_code ignored;
                    std::filesystem::remove(m_settings.outputPath, ignored);
                }
                return true;
            }

            bool isOpen() const noexcept override { return m_tiff != nullptr; }

        private:
            WriterSettings m_settings;
            TIFF* m_tiff{nullptr};
            std::size_t m_framesWritten{0};
        };

        // Store raw frame bytes with CSV metadata
        class BinaryBackend final : public Backend
        {
        public:
            ~BinaryBackend() override
            {
                std::string ignored;
                close(ignored);
            }

            bool open(const WriterSettings& settings, std::string& error) override
            {
                std::error_code filesystemError;
                const bool rawExists = std::filesystem::exists(settings.outputPath, filesystemError);
                filesystemError.clear();
                const bool metadataExists = std::filesystem::exists(settings.frameMetadataPath,
                                                                     filesystemError);
                if (rawExists || metadataExists)
                {
                    if (!settings.overwrite)
                    {
                        error = "Binary output already exists";
                        return false;
                    }
                    std::filesystem::remove(settings.outputPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to replace the existing binary payload";
                        return false;
                    }
                    filesystemError.clear();
                    std::filesystem::remove(settings.frameMetadataPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to replace the existing binary output";
                        return false;
                    }
                }
                for (const auto& path : {settings.outputPath, settings.frameMetadataPath})
                {
                    const auto parent = path.parent_path();
                    if (!parent.empty())
                    {
                        std::filesystem::create_directories(parent, filesystemError);
                        if (filesystemError)
                        {
                            error = "Failed to create the binary output directory";
                            return false;
                        }
                    }
                }
                m_raw.open(settings.outputPath, std::ios::binary | std::ios::trunc);
                m_metadata.open(settings.frameMetadataPath, std::ios::binary | std::ios::trunc);
                if (!m_raw || !m_metadata)
                {
                    m_raw.close();
                    m_metadata.close();
                    std::filesystem::remove(settings.outputPath, filesystemError);
                    filesystemError.clear();
                    std::filesystem::remove(settings.frameMetadataPath, filesystemError);
                    error = "Failed to open the binary output";
                    return false;
                }
                m_metadata << kBinaryFrameMetadataHeader << '\n';
                if (!m_metadata)
                {
                    m_raw.close();
                    m_metadata.close();
                    std::filesystem::remove(settings.outputPath, filesystemError);
                    filesystemError.clear();
                    std::filesystem::remove(settings.frameMetadataPath, filesystemError);
                    error = "Failed to write the binary frame metadata header";
                    return false;
                }
                m_settings = settings;
                m_framesWritten = 0;
                return true;
            }

            bool append(const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& metadata,
                        std::string& error) override
            {
                if (!m_raw.is_open() || !m_metadata.is_open())
                {
                    error = "Binary output is not open";
                    return false;
                }
                if (!validBinaryFrame(m_settings, data, byteCount, metadata, error))
                {
                    return false;
                }
                m_raw.write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
                m_metadata << csvField(metadata.cameraId) << ','
                    << metadata.frameIndex << ',' << metadata.timestampNs << ','
                    << m_settings.width << ',' << m_settings.height << ','
                    << m_settings.significantBits << ',' << metadata.stride << ','
                    << pixelFormatName(m_settings.pixelType) << ','
                    << pixelFormatId(m_settings.pixelType) << ','
                    << byteCount << ',' << metadata.sourceRoiX << ',' << metadata.sourceRoiY << ','
                    << metadata.sourceRoiWidth << ',' << metadata.sourceRoiHeight << '\n';
                if (!m_raw || !m_metadata)
                {
                    error = "Failed to append the binary frame";
                    return false;
                }
                ++m_framesWritten;
                return true;
            }

            bool flush(std::string& error) override
            {
                if (!m_raw.is_open() || !m_metadata.is_open())
                {
                    error = "Binary output is not open";
                    return false;
                }
                m_raw.flush();
                m_metadata.flush();
                if (!m_raw || !m_metadata)
                {
                    error = "Failed to flush the binary output";
                    return false;
                }
                return true;
            }

            bool close(std::string& error) override
            {
                bool success = true;
                if (m_raw.is_open())
                {
                    m_raw.flush();
                    success = success && static_cast<bool>(m_raw);
                    m_raw.close();
                }
                if (m_metadata.is_open())
                {
                    m_metadata.flush();
                    success = success && static_cast<bool>(m_metadata);
                    m_metadata.close();
                }
                if (m_framesWritten == 0)
                {
                    std::error_code ignored;
                    std::filesystem::remove(m_settings.outputPath, ignored);
                    std::filesystem::remove(m_settings.frameMetadataPath, ignored);
                }
                if (!success) error = "Failed to finalize the binary output";
                return success;
            }

            bool isOpen() const noexcept override
            {
                return m_raw.is_open() && m_metadata.is_open();
            }

        private:
            WriterSettings m_settings;
            std::ofstream m_raw;
            std::ofstream m_metadata;
            std::size_t m_framesWritten{0};
        };

        // Store asynchronous OME TIFF series
        class OmeTiffBackend final : public Backend
        {
        public:
            ~OmeTiffBackend() override
            {
                std::string ignored;
                close(ignored);
            }

            bool open(const WriterSettings& settings, std::string& error) override
            {
                std::error_code filesystemError;
                m_outputRoot = settings.positionCount > 1
                    ? multiTiffRoot(settings.outputPath)
                    : settings.outputPath;
                if (std::filesystem::exists(m_outputRoot, filesystemError))
                {
                    if (!settings.overwrite)
                    {
                        error = "OME-TIFF output already exists";
                        return false;
                    }
                    std::filesystem::remove_all(m_outputRoot, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to replace the existing OME-TIFF output";
                        return false;
                    }
                }
                const auto parent = m_outputRoot.parent_path();
                if (!parent.empty())
                {
                    std::filesystem::create_directories(parent, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to create the OME-TIFF output directory";
                        return false;
                    }
                }
                if (settings.positionCount > 1)
                {
                    std::filesystem::create_directories(m_outputRoot, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to create the multi-position OME-TIFF directory";
                        return false;
                    }
                }
                m_settings = settings;
                m_series.clear();
                m_series.resize(static_cast<std::size_t>(settings.positionCount));
                for (int position = 0; position < settings.positionCount; ++position)
                {
                    auto& series = m_series[static_cast<std::size_t>(position)];
                    series.path = settings.positionCount == 1
                        ? settings.outputPath
                        : positionTiffPath(m_outputRoot, position);
                    series.uuid = settings.positionCount == 1
                        ? settings.uuid
                        : createUuid();
#if defined(_WIN32)
                    series.tiff = TIFFOpenW(series.path.c_str(), "w8");
#else
                    const std::string path = pathUtf8(series.path);
                    series.tiff = TIFFOpen(path.c_str(), "w8");
#endif
                    if (series.tiff == nullptr)
                    {
                        error = "Failed to open the OME-TIFF output";
                        closeSeries();
                        std::filesystem::remove_all(m_outputRoot, filesystemError);
                        return false;
                    }
                }
                m_nextPlaneIndex.assign(static_cast<std::size_t>(settings.positionCount), 0);
                m_zeroFrame.assign(static_cast<std::size_t>(settings.width)
                                       * static_cast<std::size_t>(settings.height)
                                       * bytesPerPixel(settings.pixelType),
                                   0);
                m_jobs.clear();
                m_freeFrames.clear();
                m_activeJobs = 0;
                m_metadataRevision = 0;
                m_workerError.clear();
                m_accepting = true;
                m_open = true;
                try
                {
                    m_worker = std::thread([this]
                    {
                        workerLoop();
                    });
                }
                catch (const std::exception& exception)
                {
                    m_accepting = false;
                    m_open = false;
                    closeSeries();
                    std::filesystem::remove_all(m_outputRoot, filesystemError);
                    error = "Failed to start OME-TIFF writer: "
                        + std::string(exception.what());
                    return false;
                }
                return true;
            }

            bool append(const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& suppliedMetadata,
                        std::string& error) override
            {
                if (!m_open)
                {
                    error = "OME-TIFF output is not open";
                    return false;
                }
                if (!validFrame(m_settings, data, byteCount, suppliedMetadata, error))
                {
                    return false;
                }

                FrameMetadata metadata = suppliedMetadata;
                auto& nextPlaneIndex = m_nextPlaneIndex[
                    static_cast<std::size_t>(metadata.positionIndex)];
                if (metadata.t < 0)
                {
                    std::int64_t capacity = 0;
                    if (planeCapacity(m_settings, capacity)
                        && capacity >= 0 && nextPlaneIndex >= capacity)
                    {
                        error = "OME-TIFF contains all configured frames";
                        return false;
                    }
                    metadata = coordinatesForSequenceIndex(m_settings,
                                                           nextPlaneIndex,
                                                           metadata.positionIndex);
                }
                std::int64_t planeIndex = 0;
                if (!sequenceIndex(m_settings, metadata, planeIndex))
                {
                    error = "OME-TIFF frame coordinates exceed the supported range";
                    return false;
                }
                if (planeIndex < nextPlaneIndex)
                {
                    error = "OME-TIFF frame is duplicate or out of acquisition order";
                    return false;
                }
                while (nextPlaneIndex < planeIndex)
                {
                    const FrameMetadata fill = coordinatesForSequenceIndex(
                        m_settings, nextPlaneIndex, metadata.positionIndex);
                    if (!enqueuePlane(nullptr, 0, fill, error))
                    {
                        return false;
                    }
                    ++nextPlaneIndex;
                }
                if (!enqueuePlane(data, byteCount, metadata, error))
                {
                    return false;
                }
                ++nextPlaneIndex;
                return true;
            }

            bool flush(std::string& error) override
            {
                if (!m_open)
                {
                    error = "OME-TIFF output is not open";
                    return false;
                }
                if (!waitForQueuedWrites(error))
                {
                    return false;
                }
                for (auto& series : m_series)
                {
                    if (!checkpointSeries(series, true, error))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool close(std::string& error) override
            {
                if (!m_open)
                {
                    return true;
                }
                {
                    std::lock_guard lock(m_queueMutex);
                    m_accepting = false;
                }
                m_jobReady.notify_all();
                m_queueSpace.notify_all();
                if (m_worker.joinable())
                {
                    m_worker.join();
                }
                bool success = true;
                if (!m_workerError.empty())
                {
                    error = m_workerError;
                    success = false;
                }
                for (auto& series : m_series)
                {
                    std::string checkpointError;
                    if (!checkpointSeries(series, false, checkpointError))
                    {
                        if (error.empty())
                        {
                            error = std::move(checkpointError);
                        }
                        success = false;
                    }
                }
                std::vector<std::filesystem::path> emptyFiles;
                bool hasPlanes = false;
                for (const auto& series : m_series)
                {
                    if (series.planes.empty())
                    {
                        emptyFiles.push_back(series.path);
                    }
                    else
                    {
                        hasPlanes = true;
                    }
                }
                closeSeries();
                std::error_code filesystemError;
                for (const auto& path : emptyFiles)
                {
                    std::filesystem::remove(path, filesystemError);
                    if (filesystemError && success)
                    {
                        error = "Failed to remove an empty OME-TIFF output";
                        success = false;
                    }
                    filesystemError.clear();
                }
                if (!hasPlanes && m_settings.positionCount > 1)
                {
                    std::filesystem::remove(m_outputRoot, filesystemError);
                    if (filesystemError && success)
                    {
                        error = "Failed to remove the empty OME-TIFF directory";
                        success = false;
                    }
                }
                m_nextPlaneIndex.clear();
                m_zeroFrame.clear();
                m_freeFrames.clear();
                m_open = false;
                return success;
            }

            bool isOpen() const noexcept override
            {
                return m_open;
            }

        private:
            struct TiffSeries
            {
                std::filesystem::path path;
                std::string uuid;
                TIFF* tiff{nullptr};
                std::vector<Plane> planes;
                std::uint64_t checkpointedRevision{0};
            };

            struct Job
            {
                std::vector<std::uint8_t> frame;
                FrameMetadata metadata;
                bool zeroFill{false};
            };

            TIFF* openSeries(const std::filesystem::path& path, const char* mode) const
            {
#if defined(_WIN32)
                return TIFFOpenW(path.c_str(), mode);
#else
                const std::string pathString = pathUtf8(path);
                return TIFFOpen(pathString.c_str(), mode);
#endif
            }

            bool waitForQueuedWrites(std::string& error)
            {
                std::unique_lock lock(m_queueMutex);
                m_queueDrained.wait(lock, [this]
                {
                    return !m_workerError.empty()
                        || (m_jobs.empty() && m_activeJobs == 0);
                });
                if (!m_workerError.empty())
                {
                    error = m_workerError;
                    return false;
                }
                return true;
            }

            bool checkpointSeries(TiffSeries& series,
                                  bool reopen,
                                  std::string& error)
            {
                if (series.tiff == nullptr)
                {
                    error = "OME-TIFF output is not open";
                    return false;
                }
                const bool metadataChanged = !series.planes.empty()
                    && series.checkpointedRevision != m_metadataRevision;
                if (metadataChanged)
                {
                    const std::string xml = buildMetadata(series);
                    if (!TIFFSetDirectory(series.tiff, 0)
                        || !TIFFSetField(series.tiff, TIFFTAG_IMAGEDESCRIPTION, xml.c_str())
                        || !TIFFRewriteDirectory(series.tiff))
                    {
                        error = "Failed to checkpoint the OME-XML metadata";
                        return false;
                    }
                }
                if (!TIFFFlush(series.tiff))
                {
                    error = "Failed to flush the OME-TIFF output";
                    return false;
                }
                if (metadataChanged)
                {
                    series.checkpointedRevision = m_metadataRevision;
                }
                if (!reopen || !metadataChanged)
                {
                    return true;
                }
                TIFFClose(series.tiff);
                series.tiff = openSeries(series.path, "a8");
                if (series.tiff == nullptr)
                {
                    error = "Failed to reopen the OME-TIFF output after checkpoint";
                    return false;
                }
                return true;
            }

            void closeSeries()
            {
                for (auto& series : m_series)
                {
                    if (series.tiff != nullptr)
                    {
                        TIFFClose(series.tiff);
                        series.tiff = nullptr;
                    }
                    series.planes.clear();
                }
                m_series.clear();
            }

            std::string buildMetadata(const TiffSeries& destination) const
            {
                std::vector<TiffFileMetadata> files;
                files.reserve(m_series.size());
                for (std::size_t position = 0; position < m_series.size(); ++position)
                {
                    const auto& series = m_series[position];
                    if (series.planes.empty())
                    {
                        continue;
                    }
                    files.push_back(TiffFileMetadata{
                        .position = static_cast<int>(position),
                        .path = series.path,
                        .uuid = series.uuid,
                        .planes = &series.planes
                    });
                }
                return buildOmeXml(m_settings, files, destination.uuid);
            }

            bool enqueuePlane(const void* data,
                              std::size_t byteCount,
                              const FrameMetadata& metadata,
                              std::string& error)
            {
                Job job;
                job.metadata = metadata;
                job.zeroFill = data == nullptr;
                if (data != nullptr)
                {
                    {
                        std::lock_guard lock(m_queueMutex);
                        if (!m_freeFrames.empty())
                        {
                            job.frame = std::move(m_freeFrames.back());
                            m_freeFrames.pop_back();
                        }
                    }
                    job.frame.resize(byteCount);
                    std::memcpy(job.frame.data(), data, byteCount);
                }

                std::unique_lock lock(m_queueMutex);
                m_queueSpace.wait(lock, [this]
                {
                    return !m_accepting || !m_workerError.empty()
                        || m_jobs.size() < m_queueCapacity;
                });
                if (!m_workerError.empty())
                {
                    error = m_workerError;
                    return false;
                }
                if (!m_accepting)
                {
                    error = "OME-TIFF writer is closing";
                    return false;
                }
                m_jobs.push_back(std::move(job));
                lock.unlock();
                m_jobReady.notify_one();
                return true;
            }

            void workerLoop()
            {
                while (true)
                {
                    Job job;
                    {
                        std::unique_lock lock(m_queueMutex);
                        m_jobReady.wait(lock, [this]
                        {
                            return !m_jobs.empty() || !m_accepting;
                        });
                        if (m_jobs.empty())
                        {
                            return;
                        }
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                        ++m_activeJobs;
                        m_queueSpace.notify_one();
                    }

                    std::string error;
                    const void* data = job.zeroFill ? m_zeroFrame.data() : job.frame.data();
                    const bool success = writePlane(data, job.metadata, error);
                    {
                        std::lock_guard lock(m_queueMutex);
                        --m_activeJobs;
                        if (success)
                        {
                            recycleFrame(job.frame);
                            if (m_jobs.empty() && m_activeJobs == 0)
                            {
                                m_queueDrained.notify_all();
                            }
                            continue;
                        }
                        m_workerError = error.empty() ? "OME-TIFF writer failed" : std::move(error);
                        m_accepting = false;
                        m_jobs.clear();
                    }
                    m_jobReady.notify_all();
                    m_queueSpace.notify_all();
                    m_queueDrained.notify_all();
                    return;
                }
            }

            void recycleFrame(std::vector<std::uint8_t>& frame)
            {
                if (frame.empty() || m_freeFrames.size() >= m_queueCapacity + 1)
                {
                    return;
                }
                frame.clear();
                m_freeFrames.push_back(std::move(frame));
            }

            bool writePlane(const void* data,
                            const FrameMetadata& metadata,
                            std::string& error)
            {
                auto& series = m_series[static_cast<std::size_t>(metadata.positionIndex)];
                TIFF* tiff = series.tiff;

                series.planes.push_back(Plane{
                    .ifd = static_cast<int>(series.planes.size()),
                    .metadata = metadata
                });
                if (series.planes.size() == 1)
                {
                    const std::string xml = buildMetadata(series);
                    configureTiffDirectory(tiff, m_settings, xml.c_str());
                }
                else
                {
                    configureTiffDirectory(tiff, m_settings, nullptr);
                }

                if (!writeTiffStrips(tiff, m_settings, data, error)
                    || !TIFFWriteDirectory(tiff))
                {
                    series.planes.pop_back();
                    if (error.empty()) error = "Failed to write the OME-TIFF frame";
                    return false;
                }
                ++m_metadataRevision;
                return true;
            }
            WriterSettings m_settings;
            std::filesystem::path m_outputRoot;
            std::vector<TiffSeries> m_series;
            std::vector<std::int64_t> m_nextPlaneIndex;
            std::vector<std::uint8_t> m_zeroFrame;
            std::deque<Job> m_jobs;
            std::deque<std::vector<std::uint8_t>> m_freeFrames;
            std::thread m_worker;
            std::mutex m_queueMutex;
            std::condition_variable m_jobReady;
            std::condition_variable m_queueSpace;
            std::condition_variable m_queueDrained;
            std::string m_workerError;
            std::size_t m_queueCapacity{8};
            std::size_t m_activeJobs{0};
            std::uint64_t m_metadataRevision{0};
            bool m_accepting{false};
            bool m_open{false};
        };

        // Store OME Zarr series through the filesystem backend
        class OmeZarrBackend final : public Backend
        {
            struct Series
            {
                int positionIndex{0};
                std::string groupName;
                std::ofstream frameMetadata;
                std::int64_t nextPlaneIndex{0};
            };

        public:
            ~OmeZarrBackend() override
            {
                std::string ignored;
                close(ignored);
            }

            bool open(const WriterSettings& settings, std::string& error) override
            {
                std::error_code filesystemError;
                if (std::filesystem::exists(settings.outputPath, filesystemError))
                {
                    if (!settings.overwrite)
                    {
                        error = "OME-Zarr output already exists";
                        return false;
                    }
                    std::filesystem::remove_all(settings.outputPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to replace the existing OME-Zarr output";
                        return false;
                    }
                }
                const auto parent = settings.outputPath.parent_path();
                if (!parent.empty())
                {
                    std::filesystem::create_directories(parent, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to create the OME-Zarr output directory";
                        return false;
                    }
                }

                m_settings = settings;
                m_series.clear();
                m_framesWritten = 0;
                std::vector<std::string> seriesMetadata;
                std::vector<std::string> seriesNames;
                seriesMetadata.reserve(static_cast<std::size_t>(settings.positionCount));
                seriesNames.reserve(static_cast<std::size_t>(settings.positionCount));
                for (int position = 0; position < settings.positionCount; ++position)
                {
                    Series series;
                    series.positionIndex = position;
                    series.groupName = seriesName(settings, position);
                    seriesNames.push_back(series.groupName);
                    seriesMetadata.push_back(globalMetadata(series));
                    m_series.emplace(position, std::move(series));
                }

                if (!m_writer.open(settings, seriesNames, seriesMetadata, error))
                {
                    cleanupFailedOpen();
                    return false;
                }

                for (auto& [position, series] : m_series)
                {
                    static_cast<void>(position);
                    const auto groupPath = series.groupName.empty()
                                               ? settings.outputPath
                                               : settings.outputPath / series.groupName;
                    std::filesystem::create_directories(groupPath, filesystemError);
                    if (filesystemError)
                    {
                        error = "Failed to create the OME-Zarr image group";
                        cleanupFailedOpen();
                        return false;
                    }
                    series.frameMetadata.open(groupPath / "scopewriter.frames.jsonl",
                                              std::ios::out | std::ios::trunc);
                    if (!series.frameMetadata)
                    {
                        error = "Failed to open the OME-Zarr frame metadata";
                        cleanupFailedOpen();
                        return false;
                    }
                }
                m_open = true;
                return true;
            }

            bool append(const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& suppliedMetadata,
                        std::string& error) override
            {
                if (!m_open)
                {
                    error = "OME-Zarr output is not open";
                    return false;
                }
                if (!validFrame(m_settings, data, byteCount, suppliedMetadata, error))
                {
                    return false;
                }
                auto seriesIterator = m_series.find(suppliedMetadata.positionIndex);
                if (seriesIterator == m_series.end())
                {
                    error = "OME-Zarr position index is invalid";
                    return false;
                }
                auto& series = seriesIterator->second;
                FrameMetadata metadata = suppliedMetadata;
                if (metadata.t < 0)
                {
                    std::int64_t capacity = 0;
                    if (planeCapacity(m_settings, capacity)
                        && capacity >= 0 && series.nextPlaneIndex >= capacity)
                    {
                        error = "OME-Zarr contains all configured frames";
                        return false;
                    }
                    metadata = coordinatesForSequenceIndex(m_settings,
                                                           series.nextPlaneIndex,
                                                           metadata.positionIndex);
                }
                std::int64_t planeIndex = 0;
                if (!sequenceIndex(m_settings, metadata, planeIndex))
                {
                    error = "OME-Zarr frame coordinates exceed the supported range";
                    return false;
                }
                if (planeIndex < series.nextPlaneIndex)
                {
                    error = "OME-Zarr frame is duplicate or out of acquisition order";
                    return false;
                }
                while (series.nextPlaneIndex < planeIndex)
                {
                    const FrameMetadata fill = coordinatesForSequenceIndex(
                        m_settings, series.nextPlaneIndex, series.positionIndex);
                    if (!appendPlane(series, nullptr, byteCount, fill, error))
                    {
                        return false;
                    }
                }
                if (!appendPlane(series, data, byteCount, metadata, error))
                {
                    return false;
                }
                series.frameMetadata << frameMetadataJson(metadata) << '\n';
                if (!series.frameMetadata)
                {
                    error = "Failed to write OME-Zarr frame metadata";
                    return false;
                }
                ++m_framesWritten;
                return true;
            }

            bool flush(std::string& error) override
            {
                if (!m_open)
                {
                    error = "OME-Zarr output is not open";
                    return false;
                }
                for (auto& [position, series] : m_series)
                {
                    static_cast<void>(position);
                    series.frameMetadata.flush();
                    if (!series.frameMetadata)
                    {
                        error = "Failed to flush OME-Zarr frame metadata";
                        return false;
                    }
                }
                return m_writer.flush(error);
            }

            bool close(std::string& error) override
            {
                if (!m_open)
                {
                    return true;
                }
                bool success = true;
                for (auto& [position, series] : m_series)
                {
                    static_cast<void>(position);
                    series.frameMetadata.flush();
                    if (!series.frameMetadata)
                    {
                        error = "Failed to flush OME-Zarr frame metadata";
                        success = false;
                    }
                    series.frameMetadata.close();
                }
                std::string writerError;
                if (!m_writer.close(writerError))
                {
                    if (error.empty())
                    {
                        error = std::move(writerError);
                    }
                    success = false;
                }
                m_series.clear();
                m_open = false;
                return success;
            }

            bool isOpen() const noexcept override
            {
                return m_open;
            }

        private:
            bool appendPlane(Series& series,
                             const void* data,
                             std::size_t byteCount,
                             const FrameMetadata& metadata,
                             std::string& error)
            {
                if (!m_writer.append(series.positionIndex,
                                     metadata.t,
                                     metadata.c,
                                     metadata.z,
                                     data,
                                     byteCount,
                                     error))
                {
                    return false;
                }
                ++series.nextPlaneIndex;
                return true;
            }

            std::string globalMetadata(const Series& series) const
            {
                std::ostringstream json;
                const std::string positionName = !m_settings.positions.empty()
                    && !m_settings.positions[static_cast<std::size_t>(series.positionIndex)].name.empty()
                    ? m_settings.positions[static_cast<std::size_t>(series.positionIndex)].name
                    : "Position " + std::to_string(series.positionIndex + 1);
                const std::string imageName = m_settings.positionCount > 1
                    ? m_settings.imageName + ' ' + positionName
                    : m_settings.imageName;
                json << "{\"creator\":" << jsonEscape(m_settings.creator)
                     << ",\"imageName\":" << jsonEscape(imageName)
                     << ",\"positionIndex\":" << series.positionIndex
                     << ",\"significantBits\":" << m_settings.significantBits
                     << ",\"frameMetadata\":\"scopewriter.frames.jsonl\""
                     << ",\"frameMetadataFormat\":\"json-lines\"";
                if (!m_settings.linkedMetadataFile.empty())
                {
                    json << ",\"linkedMetadataFile\":"
                         << jsonEscape(m_settings.linkedMetadataFile);
                }
                if (m_settings.acquisitionStartTimestampNs != 0)
                {
                    json << ",\"acquisitionStartTimestampNs\":"
                         << jsonEscape(std::to_string(m_settings.acquisitionStartTimestampNs));
                    const std::string date = isoTimestamp(m_settings.acquisitionStartTimestampNs);
                    if (!date.empty())
                        json << ",\"acquisitionDate\":" << jsonEscape(date);
                }
                const bool hasDetector = !m_settings.detector.manufacturer.empty()
                    || !m_settings.detector.model.empty()
                    || !m_settings.detector.serialNumber.empty()
                    || m_settings.detector.offset.has_value();
                if (hasDetector)
                {
                    json << ",\"detector\":{";
                    bool comma = false;
                    const auto addString = [&json, &comma](const char* key, const std::string& value)
                    {
                        if (value.empty()) return;
                        if (comma) json << ',';
                        json << '\"' << key << "\":" << jsonEscape(value);
                        comma = true;
                    };
                    addString("manufacturer", m_settings.detector.manufacturer);
                    addString("model", m_settings.detector.model);
                    addString("serialNumber", m_settings.detector.serialNumber);
                    if (m_settings.detector.offset)
                    {
                        if (comma) json << ',';
                        json << "\"offset\":" << number(*m_settings.detector.offset);
                    }
                    json << '}';
                }
                if (!m_settings.positions.empty())
                {
                    const auto& position = m_settings.positions[
                        static_cast<std::size_t>(series.positionIndex)];
                    json << ",\"position\":{\"name\":"
                         << jsonEscape(position.name.empty() ? series.groupName : position.name);
                    if (position.gridRow)
                        json << ",\"gridRow\":" << *position.gridRow;
                    if (position.gridColumn)
                        json << ",\"gridColumn\":" << *position.gridColumn;
                    if (position.xUm)
                        json << ",\"xUm\":" << number(*position.xUm);
                    if (position.yUm)
                        json << ",\"yUm\":" << number(*position.yUm);
                    if (position.zUm)
                        json << ",\"zUm\":" << number(*position.zUm);
                    json << '}';
                }
                if (!m_settings.metadata.empty())
                {
                    json << ",\"metadata\":{";
                    bool namespaceComma = false;
                    for (const auto& [nameSpace, values] : m_settings.metadata)
                    {
                        if (namespaceComma) json << ',';
                        namespaceComma = true;
                        json << jsonEscape(nameSpace) << ":{";
                        bool valueComma = false;
                        for (const auto& [key, value] : values)
                        {
                            if (valueComma) json << ',';
                            valueComma = true;
                            json << jsonEscape(key) << ':' << jsonEscape(value);
                        }
                        json << '}';
                    }
                    json << '}';
                }
                json << '}';
                return json.str();
            }

            std::string frameMetadataJson(const FrameMetadata& metadata) const
            {
                std::ostringstream json;
                json << "{\"outputIndex\":" << jsonEscape(std::to_string(m_framesWritten))
                     << ",\"t\":" << jsonEscape(std::to_string(metadata.t))
                     << ",\"c\":" << metadata.c
                     << ",\"z\":" << metadata.z
                     << ",\"frameIndex\":" << jsonEscape(std::to_string(metadata.frameIndex))
                     << ",\"timestampNs\":" << jsonEscape(std::to_string(metadata.timestampNs));
                if (m_settings.acquisitionStartTimestampNs != 0
                    && metadata.timestampNs >= m_settings.acquisitionStartTimestampNs)
                {
                    const double deltaMs = static_cast<double>(
                        metadata.timestampNs - m_settings.acquisitionStartTimestampNs) / 1000000.0;
                    json << ",\"deltaTMs\":" << number(deltaMs);
                }
                const double exposure = metadata.exposureMs > 0.0
                                            ? metadata.exposureMs
                                            : m_settings.defaultExposureMs;
                if (exposure > 0.0)
                    json << ",\"exposureTimeMs\":" << number(exposure);
                if (metadata.positionXUm)
                    json << ",\"positionXUm\":" << number(*metadata.positionXUm);
                if (metadata.positionYUm)
                    json << ",\"positionYUm\":" << number(*metadata.positionYUm);
                if (metadata.positionZUm)
                    json << ",\"positionZUm\":" << number(*metadata.positionZUm);
                if (!metadata.metadata.empty())
                {
                    json << ",\"metadata\":{";
                    bool comma = false;
                    for (const auto& [key, value] : metadata.metadata)
                    {
                        if (comma) json << ',';
                        comma = true;
                        json << jsonEscape(key) << ':' << jsonEscape(value);
                    }
                    json << '}';
                }
                json << '}';
                return json.str();
            }

            void cleanupFailedOpen()
            {
                for (auto& [position, series] : m_series)
                {
                    static_cast<void>(position);
                    series.frameMetadata.close();
                }
                m_series.clear();
                std::string ignoredError;
                static_cast<void>(m_writer.close(ignoredError));
                std::error_code ignored;
                std::filesystem::remove_all(m_settings.outputPath, ignored);
                m_open = false;
            }

            WriterSettings m_settings;
            internal::ZarrWriter m_writer;
            std::map<int, Series> m_series;
            std::int64_t m_framesWritten{0};
            bool m_open{false};
        };
    }

    // Hold the active backend and public error state
    struct Writer::Impl
    {
        std::unique_ptr<Backend> backend;
        std::string error;
    };

    // Create an empty writer
    Writer::Writer()
        : m_impl(std::make_unique<Impl>())
    {
    }

    // Close any active output before destruction
    Writer::~Writer()
    {
        if (m_impl)
        {
            close();
        }
    }

    Writer::Writer(Writer&&) noexcept = default;
    Writer& Writer::operator=(Writer&&) noexcept = default;

    // Validate settings and open the selected backend
    bool Writer::open(const WriterSettings& suppliedSettings)
    {
        if (isOpen() && !close())
        {
            return false;
        }
        m_impl->error.clear();
        WriterSettings settings = suppliedSettings;
        if (!validSettings(settings, m_impl->error))
        {
            return false;
        }
        if (settings.uuid.empty())
        {
            settings.uuid = createUuid();
        }
        if (settings.format == Format::OmeTiff)
        {
            m_impl->backend = std::make_unique<OmeTiffBackend>();
        }
        else if (settings.format == Format::OmeZarr)
        {
            m_impl->backend = std::make_unique<OmeZarrBackend>();
        }
        else if (settings.format == Format::Tiff)
        {
            m_impl->backend = std::make_unique<TiffBackend>();
        }
        else
        {
            m_impl->backend = std::make_unique<BinaryBackend>();
        }
        if (!m_impl->backend->open(settings, m_impl->error))
        {
            m_impl->backend.reset();
            return false;
        }
        return true;
    }

    // Append one frame to the active backend
    bool Writer::append(const void* data,
                        std::size_t byteCount,
                        const FrameMetadata& metadata)
    {
        if (!m_impl->backend)
        {
            m_impl->error = "ScopeWriter is not open";
            return false;
        }
        m_impl->error.clear();
        return m_impl->backend->append(data, byteCount, metadata, m_impl->error);
    }

    // Finalize and close the active backend
    bool Writer::close()
    {
        if (!m_impl || !m_impl->backend)
        {
            return true;
        }
        m_impl->error.clear();
        const bool success = m_impl->backend->close(m_impl->error);
        m_impl->backend.reset();
        return success;
    }

    // Checkpoint pending output without closing it
    bool Writer::flush()
    {
        if (!m_impl || !m_impl->backend)
        {
            if (m_impl)
            {
                m_impl->error = "ScopeWriter is not open";
            }
            return false;
        }
        m_impl->error.clear();
        return m_impl->backend->flush(m_impl->error);
    }

    // Report whether a backend is active
    bool Writer::isOpen() const noexcept
    {
        return m_impl && m_impl->backend && m_impl->backend->isOpen();
    }

    // Return the most recent writer error
    const std::string& Writer::lastError() const noexcept
    {
        return m_impl->error;
    }
}
