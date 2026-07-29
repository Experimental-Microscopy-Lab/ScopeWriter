#include "scopewriter/ScopeWriter.h"

#include <tiffio.h>

#include <crc32c/crc32c.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::filesystem::path testRoot()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path()
            / ("scopewriter-tests-" + std::to_string(suffix));
    }

    void testDependencyVersions()
    {
        require(!scopewriter::libTiffVersion().empty(), "libtiff version is empty");
        require(!scopewriter::zlibVersion().empty(), "zlib version is empty");
    }

    TIFF* openTiff(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        return TIFFOpenW(path.c_str(), "r");
#else
        return TIFFOpen(path.string().c_str(), "r");
#endif
    }

    std::string readText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        require(static_cast<bool>(input), "Failed to open generated metadata");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    std::string readTiffDescription(const std::filesystem::path& path)
    {
        TIFF* tiff = openTiff(path);
        require(tiff != nullptr, "Failed to open generated OME-TIFF");
        char* description = nullptr;
        require(TIFFSetDirectory(tiff, 0) == 1
                    && TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &description) == 1
                    && description != nullptr,
                "Generated OME-TIFF has no OME-XML description");
        const std::string xml(description);
        TIFFClose(tiff);
        return xml;
    }

    std::string rootUuid(const std::string& xml)
    {
        const std::size_t ome = xml.find("<OME ");
        const std::size_t begin = xml.find("UUID=\"", ome);
        require(ome != std::string::npos && begin != std::string::npos,
                "OME-XML root UUID is missing");
        const std::size_t value = begin + 6;
        const std::size_t end = xml.find('"', value);
        require(end != std::string::npos, "OME-XML root UUID is malformed");
        return xml.substr(value, end - value);
    }

    std::uint64_t readLittleEndian64(const char* data)
    {
        std::uint64_t value = 0;
        for (int byte = 0; byte < 8; ++byte)
        {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[byte]))
                << (byte * 8);
        }
        return value;
    }

    std::uint32_t readLittleEndian32(const char* data)
    {
        std::uint32_t value = 0;
        for (int byte = 0; byte < 4; ++byte)
        {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[byte]))
                << (byte * 8);
        }
        return value;
    }

    std::vector<char> readBinary(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        require(static_cast<bool>(input), "Failed to open generated binary data");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void testOmeTiff(const std::filesystem::path& root)
    {
        const auto path = root / "output.ome.tiff";
        scopewriter::WriterSettings settings;
        settings.format = scopewriter::Format::OmeTiff;
        settings.outputPath = path;
        settings.width = 8;
        settings.height = 4;
        settings.pixelType = scopewriter::PixelType::UInt16;
        settings.significantBits = 12;
        settings.physicalSizeXUm = 0.25;
        settings.physicalSizeYUm = 0.25;
        settings.timeIncrementMs = 12.5;
        settings.defaultExposureMs = 5.0;
        settings.acquisitionStartTimestampNs = 1700000000000000000ull;
        settings.imageName = "Camera & channel";
        settings.creator = "ScopeWriterTests";
        settings.enableCompression = true;

        std::vector<std::uint16_t> frame(32, 42);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());

        scopewriter::FrameMetadata first;
        first.frameIndex = 10;
        first.timestampNs = settings.acquisitionStartTimestampNs;
        first.t = 0;
        first.positionXUm = 1.5;
        require(writer.append(frame.data(), frame.size() * sizeof(std::uint16_t), first),
                writer.lastError());

        scopewriter::FrameMetadata second;
        second.frameIndex = 11;
        second.timestampNs = settings.acquisitionStartTimestampNs + 12500000ull;
        second.t = 1;
        second.exposureMs = 7.5;
        require(writer.append(frame.data(), frame.size() * sizeof(std::uint16_t), second),
                writer.lastError());
        require(writer.close(), writer.lastError());

        TIFF* tiff = openTiff(path);
        require(tiff != nullptr, "Failed to reopen the OME-TIFF test output");
        require(TIFFNumberOfDirectories(tiff) == 2, "OME-TIFF directory count is incorrect");
        require(TIFFSetDirectory(tiff, 0) == 1, "Failed to select the first TIFF directory");
        char* description = nullptr;
        require(TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &description) == 1
                    && description != nullptr,
                "OME-XML is missing from ImageDescription");
        const std::string xml(description);
        std::vector<std::uint16_t> decoded(frame.size());
        require(TIFFReadEncodedStrip(tiff,
                                     0,
                                     decoded.data(),
                                     static_cast<tmsize_t>(decoded.size()
                                                           * sizeof(std::uint16_t)))
                    == static_cast<tmsize_t>(decoded.size() * sizeof(std::uint16_t)),
                "Failed to decode the compressed OME-TIFF frame");
        require(decoded == frame, "OME-TIFF pixels changed during compression");
        TIFFClose(tiff);

        require(xml.find("<OME ") != std::string::npos, "OME root element is missing");
        require(xml.find("SizeT=\"2\"") != std::string::npos, "Final SizeT is incorrect");
        require(xml.find("SignificantBits=\"12\"") != std::string::npos,
                "SignificantBits is missing");
        require(xml.find("DeltaT=\"12.5\" DeltaTUnit=\"ms\"") != std::string::npos,
                "DeltaT metadata is incorrect");
        require(xml.find("Camera &amp; channel") != std::string::npos,
                "OME-XML attributes are not escaped");
    }

    void testOmeZarr(const std::filesystem::path& root)
    {
        const auto path = root / "output.ome.zarr";
        scopewriter::WriterSettings settings;
        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = path;
        settings.width = 8;
        settings.height = 8;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        settings.physicalSizeXUm = 0.5;
        settings.physicalSizeYUm = 0.5;
        settings.imageName = "Camera";
        settings.enableCompression = true;
        settings.compressionLevel = 3;

        std::vector<std::uint8_t> frame(64, 9);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        require(writer.append(frame.data(), frame.size()), writer.lastError());
        scopewriter::FrameMetadata third;
        third.t = 2;
        require(writer.append(frame.data(), frame.size(), third), writer.lastError());
        require(writer.close(), writer.lastError());

        require(std::filesystem::exists(path / "zarr.json"), "OME-Zarr root metadata is missing");
        require(std::filesystem::exists(path / "0" / "zarr.json"),
                "OME-Zarr array metadata is missing");
        require(std::filesystem::exists(path / "scopewriter.frames.jsonl"),
                "OME-Zarr frame metadata is missing");
        bool foundShard = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path / "0" / "c"))
        {
            if (entry.is_regular_file())
            {
                foundShard = true;
                break;
            }
        }
        require(foundShard, "OME-Zarr pixel shards are missing");
        const auto shardPath = path / "0" / "c" / "0" / "0" / "0" / "0" / "0";
        std::ifstream shardInput(shardPath, std::ios::binary);
        require(static_cast<bool>(shardInput), "OME-Zarr pixel shard cannot be opened");
        const std::vector<char> shard{std::istreambuf_iterator<char>(shardInput),
                                      std::istreambuf_iterator<char>()};
        require(shard.size() > 20, "OME-Zarr pixel shard is too small");
        const char* index = shard.data() + shard.size() - 20;
        require(readLittleEndian32(index + 16)
                    == crc32c::Crc32c(reinterpret_cast<const std::uint8_t*>(index), 16),
                "OME-Zarr shard index checksum is invalid");
        const std::uint64_t offset = readLittleEndian64(index);
        const std::uint64_t extent = readLittleEndian64(index + 8);
        require(offset <= shard.size() - 20 && extent <= shard.size() - 20 - offset,
                "OME-Zarr shard index is invalid");
        std::vector<std::uint8_t> decoded(frame.size());
        const std::size_t decodedSize = ZSTD_decompress(decoded.data(),
                                                        decoded.size(),
                                                        shard.data() + offset,
                                                        static_cast<std::size_t>(extent));
        require(!ZSTD_isError(decodedSize) && decodedSize == decoded.size(),
                "OME-Zarr chunk cannot be decompressed");
        require(decoded == frame, "OME-Zarr pixels changed during compression");
        const auto gapPath = path / "0" / "c" / "1" / "0" / "0" / "0" / "0";
        std::ifstream gapInput(gapPath, std::ios::binary);
        const std::vector<unsigned char> gap{std::istreambuf_iterator<char>(gapInput),
                                             std::istreambuf_iterator<char>()};
        require(gap.size() == 20
                    && std::all_of(gap.begin(), gap.begin() + 16, [](unsigned char value)
                    {
                        return value == 0xff;
                    }),
                "OME-Zarr sequential gap is not represented as an empty shard");
        require(readLittleEndian32(reinterpret_cast<const char*>(gap.data() + 16))
                    == crc32c::Crc32c(gap.data(), 16),
                "OME-Zarr empty shard index checksum is invalid");
        const std::string groupMetadata = readText(path / "zarr.json");
        require(groupMetadata.find("\"ome\"") != std::string::npos
                    && groupMetadata.find("\"0.5\"") != std::string::npos,
                "OME-NGFF version metadata is missing");
        require(groupMetadata.find("\"scopewriter\"") != std::string::npos,
                "ScopeWriter dataset metadata is missing");
        const std::string arrayMetadata = readText(path / "0" / "zarr.json");
        require(arrayMetadata.find("\"shape\": [3,1,1,8,8]") != std::string::npos,
                "OME-Zarr array shape is incorrect");
        require(arrayMetadata.find("\"sharding_indexed\"") != std::string::npos
                    && arrayMetadata.find("\"zstd\"") != std::string::npos,
                "OME-Zarr codec metadata is incorrect");
        const std::string frameMetadata = readText(path / "scopewriter.frames.jsonl");
        require(frameMetadata.find("\"t\":\"0\"") != std::string::npos
                    && frameMetadata.find("\"t\":\"2\"") != std::string::npos,
                "OME-Zarr frame coordinates are missing");
    }

    void testOmeZarrMultiChunkShard(const std::filesystem::path& root)
    {
        const auto path = root / "multi-chunk.ome.zarr";
        scopewriter::WriterSettings settings;
        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = path;
        settings.width = 700;
        settings.height = 700;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        settings.enableCompression = true;
        settings.compressionLevel = 3;

        std::vector<std::uint8_t> frame(static_cast<std::size_t>(settings.width)
                                        * settings.height);
        for (int y = 0; y < settings.height; ++y)
        {
            for (int x = 0; x < settings.width; ++x)
            {
                frame[static_cast<std::size_t>(y) * settings.width + x]
                    = y < 512 ? (x < 512 ? 1 : 2) : (x < 512 ? 0 : 4);
            }
        }

        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        require(writer.append(frame.data(), frame.size()), writer.lastError());
        require(writer.close(), writer.lastError());

        const auto shard = readBinary(path / "0" / "c" / "0" / "0" / "0" / "0" / "0");
        constexpr std::size_t chunkCount = 4;
        constexpr std::size_t indexSize = chunkCount * 16 + 4;
        require(shard.size() > indexSize, "Multi-chunk OME-Zarr shard is too small");
        const char* index = shard.data() + shard.size() - indexSize;
        require(readLittleEndian32(index + chunkCount * 16)
                    == crc32c::Crc32c(reinterpret_cast<const std::uint8_t*>(index),
                                     chunkCount * 16),
                "Multi-chunk OME-Zarr index checksum is invalid");

        const std::uint64_t unwritten = (std::numeric_limits<std::uint64_t>::max)();
        const std::array<std::uint8_t, chunkCount> expected{1, 2, 0, 4};
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk)
        {
            const std::uint64_t offset = readLittleEndian64(index + chunk * 16);
            const std::uint64_t extent = readLittleEndian64(index + chunk * 16 + 8);
            if (expected[chunk] == 0)
            {
                require(offset == unwritten && extent == unwritten,
                        "All-zero OME-Zarr chunk was not skipped");
                continue;
            }
            require(offset <= shard.size() - indexSize
                        && extent <= shard.size() - indexSize - offset,
                    "Multi-chunk OME-Zarr index entry is invalid");
            std::vector<std::uint8_t> decoded(512 * 512);
            const std::size_t decodedSize = ZSTD_decompress(decoded.data(),
                                                            decoded.size(),
                                                            shard.data() + offset,
                                                            static_cast<std::size_t>(extent));
            require(!ZSTD_isError(decodedSize) && decodedSize == decoded.size(),
                    "Multi-chunk OME-Zarr payload cannot be decompressed");
            require(decoded.front() == expected[chunk],
                    "Multi-chunk OME-Zarr chunk coordinates are incorrect");
        }
    }

    void testOmeZarrConfigurableShards(const std::filesystem::path& root)
    {
        const auto path = root / "configured-shards.ome.zarr";
        scopewriter::WriterSettings settings;
        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = path;
        settings.width = 5;
        settings.height = 5;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        settings.zarrChunkWidth = 2;
        settings.zarrChunkHeight = 2;
        settings.zarrShardWidthChunks = 1;
        settings.zarrShardHeightChunks = 1;
        settings.compressionLevel = 3;

        std::vector<std::uint8_t> frame(25, 7);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        require(writer.append(frame.data(), frame.size()), writer.lastError());
        require(writer.close(), writer.lastError());

        const auto chunks = path / "0" / "c" / "0" / "0" / "0";
        for (int shardY = 0; shardY < 3; ++shardY)
        {
            for (int shardX = 0; shardX < 3; ++shardX)
            {
                require(std::filesystem::exists(chunks / std::to_string(shardY)
                                                / std::to_string(shardX)),
                        "Configured OME-Zarr shard grid is incomplete");
            }
        }

        const std::string metadata = readText(path / "0" / "zarr.json");
        require(metadata.find("\"chunk_shape\":[1,1,1,2,2]") != std::string::npos,
                "Configured OME-Zarr chunk and shard shape is missing");

        const auto edgeShard = readBinary(chunks / "2" / "2");
        require(edgeShard.size() > 20, "Configured edge shard is empty");
        const char* index = edgeShard.data() + edgeShard.size() - 20;
        const std::uint64_t offset = readLittleEndian64(index);
        const std::uint64_t extent = readLittleEndian64(index + 8);
        std::array<std::uint8_t, 4> decoded{};
        const std::size_t decodedSize = ZSTD_decompress(decoded.data(),
                                                        decoded.size(),
                                                        edgeShard.data() + offset,
                                                        static_cast<std::size_t>(extent));
        require(!ZSTD_isError(decodedSize) && decodedSize == decoded.size()
                    && decoded == std::array<std::uint8_t, 4>{7, 0, 0, 0},
                "Configured OME-Zarr edge shard pixels are incorrect");
    }

    void testChannelsAndMetadata(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.outputPath = root / "channels.ome.tiff";
        settings.width = 4;
        settings.height = 4;
        settings.pixelType = scopewriter::PixelType::UInt16;
        settings.significantBits = 12;
        settings.channelCount = 2;
        settings.channels = {
            scopewriter::ChannelMetadata{
                .name = "DAPI",
                .fluorophore = "DAPI",
                .excitationWavelengthNm = 405.0,
                .emissionWavelengthNm = 461.0,
                .colorRGB = 0x0000ffu
            },
            scopewriter::ChannelMetadata{
                .name = "FITC",
                .fluorophore = "EGFP",
                .excitationWavelengthNm = 488.0,
                .emissionWavelengthNm = 525.0,
                .colorRGB = 0x00ff00u
            }
        };
        settings.positions = {
            scopewriter::PositionMetadata{
                .name = "Control",
                .gridRow = 2,
                .gridColumn = 3,
                .xUm = 100.0,
                .yUm = 200.0
            }
        };
        settings.metadata = {{"urn:test:acquisition", {{"objective", "60x"}}}};

        std::vector<std::uint16_t> frame(16, 123);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        scopewriter::FrameMetadata dapi;
        dapi.t = 0;
        dapi.c = 0;
        dapi.metadata = {{"temperatureC", "37.0"}};
        require(writer.append(frame.data(), frame.size() * sizeof(std::uint16_t), dapi),
                writer.lastError());
        scopewriter::FrameMetadata fitc;
        fitc.t = 0;
        fitc.c = 1;
        require(writer.append(frame.data(), frame.size() * sizeof(std::uint16_t), fitc),
                writer.lastError());
        require(writer.close(), writer.lastError());

        TIFF* tiff = openTiff(settings.outputPath);
        require(tiff != nullptr, "Failed to open multi-channel OME-TIFF");
        char* description = nullptr;
        require(TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &description) == 1
                    && description != nullptr,
                "Multi-channel OME-XML is missing");
        const std::string xml(description);
        require(TIFFNumberOfDirectories(tiff) == 2,
                "Multi-channel OME-TIFF directory count is incorrect");
        TIFFClose(tiff);
        require(xml.find("SizeC=\"2\"") != std::string::npos,
                "OME-TIFF channel count is incorrect");
        require(xml.find("Name=\"DAPI\"") != std::string::npos
                    && xml.find("Name=\"FITC\"") != std::string::npos,
                "OME-TIFF channel names are missing");
        require(xml.find("TheC=\"1\"") != std::string::npos,
                "OME-TIFF channel plane coordinate is missing");
        require(xml.find("Namespace=\"urn:test:acquisition\"") != std::string::npos
                    && xml.find("urn:scopewriter:frame-metadata") != std::string::npos,
                "OME-TIFF structured metadata is missing");
        require(xml.find("StageLabel Name=\"Control\" X=\"100\"") != std::string::npos,
                "OME-TIFF position metadata is missing");

        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = root / "channels.ome.zarr";
        scopewriter::Writer zarrWriter;
        require(zarrWriter.open(settings), zarrWriter.lastError());
        require(zarrWriter.append(frame.data(), frame.size() * sizeof(std::uint16_t), dapi),
                zarrWriter.lastError());
        require(zarrWriter.append(frame.data(), frame.size() * sizeof(std::uint16_t), fitc),
                zarrWriter.lastError());
        require(zarrWriter.close(), zarrWriter.lastError());

        const std::string group = readText(settings.outputPath / "zarr.json");
        const std::string array = readText(settings.outputPath / "0" / "zarr.json");
        require(group.find("\"label\":\"DAPI\"") != std::string::npos
                    && group.find("\"color\":\"00FF00\"") != std::string::npos,
                "OME-Zarr OMERO channel metadata is missing");
        require(group.find("\"translation\":[0,0,0,200,100]") != std::string::npos,
                "OME-Zarr position translation is missing");
        require(group.find("urn:test:acquisition") != std::string::npos,
                "OME-Zarr global metadata is missing");
        require(array.find("\"shape\": [1,2,1,4,4]") != std::string::npos,
                "OME-Zarr channel dimension is incorrect");
        require(std::filesystem::exists(settings.outputPath / "0" / "c" / "0" / "0"
                                        / "0" / "0" / "0")
                    && std::filesystem::exists(settings.outputPath / "0" / "c" / "0" / "1"
                                               / "0" / "0" / "0"),
                "OME-Zarr channel shards are missing");
        const std::string frames = readText(settings.outputPath / "scopewriter.frames.jsonl");
        require(frames.find("\"c\":1") != std::string::npos
                    && frames.find("temperatureC") != std::string::npos,
                "OME-Zarr frame channel metadata is missing");
    }

    void testAcquisitionOrder(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.outputPath = root / "order.ome.tiff";
        settings.width = 2;
        settings.height = 2;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        settings.timeCount = 2;
        settings.channelCount = 2;
        settings.zCount = 2;
        settings.acquisitionOrder = "ZTC";

        std::vector<std::uint8_t> frame(4, 17);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        for (int z = 0; z < 2; ++z)
        {
            for (int t = 0; t < 2; ++t)
            {
                for (int c = 0; c < 2; ++c)
                {
                    scopewriter::FrameMetadata metadata;
                    metadata.t = t;
                    metadata.c = c;
                    metadata.z = z;
                    require(writer.append(frame.data(), frame.size(), metadata),
                            writer.lastError());
                }
            }
        }
        require(writer.close(), writer.lastError());
        TIFF* tiff = openTiff(settings.outputPath);
        require(tiff != nullptr, "Failed to open reordered OME-TIFF");
        require(TIFFNumberOfDirectories(tiff) == 8,
                "Reordered OME-TIFF directory count is incorrect");
        TIFFClose(tiff);

        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = root / "order.ome.zarr";
        scopewriter::Writer zarrWriter;
        require(zarrWriter.open(settings), zarrWriter.lastError());
        for (int z = 0; z < 2; ++z)
        {
            for (int t = 0; t < 2; ++t)
            {
                for (int c = 0; c < 2; ++c)
                {
                    scopewriter::FrameMetadata metadata;
                    metadata.t = t;
                    metadata.c = c;
                    metadata.z = z;
                    require(zarrWriter.append(frame.data(), frame.size(), metadata),
                            zarrWriter.lastError());
                }
            }
        }
        require(zarrWriter.close(), zarrWriter.lastError());
        require(std::filesystem::exists(settings.outputPath / "0" / "c" / "0" / "0"
                                        / "1" / "0" / "0")
                    && std::filesystem::exists(settings.outputPath / "0" / "c" / "1"
                                               / "1" / "0" / "0" / "0"),
                "Reordered OME-Zarr coordinates were not stored independently");
    }

    void testMultiPositionLayout(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.outputPath = root / "positions.ome.tiff";
        settings.width = 2;
        settings.height = 2;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        settings.positionCount = 2;
        settings.positions = {
            scopewriter::PositionMetadata{.name = "Alpha"},
            scopewriter::PositionMetadata{.name = "Beta"}
        };
        std::vector<std::uint8_t> frame(4, 23);
        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        scopewriter::FrameMetadata alpha;
        alpha.t = 0;
        require(writer.append(frame.data(), frame.size(), alpha), writer.lastError());
        scopewriter::FrameMetadata beta;
        beta.positionIndex = 1;
        beta.t = 0;
        require(writer.append(frame.data(), frame.size(), beta), writer.lastError());
        require(writer.close(), writer.lastError());

        const auto tiffRoot = root / "positions";
        const auto alphaTiff = tiffRoot / "positions_p000.ome.tiff";
        const auto betaTiff = tiffRoot / "positions_p001.ome.tiff";
        require(std::filesystem::exists(alphaTiff) && std::filesystem::exists(betaTiff),
                "Multi-position OME-TIFF files are missing");
        TIFF* alphaFile = openTiff(alphaTiff);
        TIFF* betaFile = openTiff(betaTiff);
        require(alphaFile != nullptr && betaFile != nullptr,
                "Multi-position OME-TIFF files cannot be opened");
        TIFFClose(alphaFile);
        TIFFClose(betaFile);
        const std::string alphaXml = readTiffDescription(alphaTiff);
        const std::string betaXml = readTiffDescription(betaTiff);
        const std::string alphaUuid = rootUuid(alphaXml);
        const std::string betaUuid = rootUuid(betaXml);
        require(alphaUuid != betaUuid,
                "Multi-position OME-TIFF files must have distinct UUIDs");
        for (const std::string* xml : {&alphaXml, &betaXml})
        {
            require(xml->find("Image:0") != std::string::npos
                        && xml->find("Image:1") != std::string::npos
                        && xml->find("Image Alpha") != std::string::npos
                        && xml->find("Image Beta") != std::string::npos,
                    "Each multi-position TIFF must contain the complete OME image model");
            require(xml->find("FileName=\"positions_p000.ome.tiff\">" + alphaUuid)
                            != std::string::npos
                        && xml->find("FileName=\"positions_p001.ome.tiff\">" + betaUuid)
                            != std::string::npos,
                    "Multi-position OME-TIFF UUID file references are incomplete");
        }

        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = root / "positions.ome.zarr";
        scopewriter::Writer zarrWriter;
        require(zarrWriter.open(settings), zarrWriter.lastError());
        require(zarrWriter.append(frame.data(), frame.size(), alpha), zarrWriter.lastError());
        require(zarrWriter.append(frame.data(), frame.size(), beta), zarrWriter.lastError());
        require(zarrWriter.close(), zarrWriter.lastError());
        require(std::filesystem::exists(settings.outputPath / "Alpha" / "0" / "zarr.json")
                    && std::filesystem::exists(settings.outputPath / "Beta" / "0" / "zarr.json")
                    && std::filesystem::exists(settings.outputPath / "OME" / "zarr.json"),
                "OME-Zarr bioformats2raw hierarchy is incomplete");
        const std::string rootMetadata = readText(settings.outputPath / "zarr.json");
        const std::string seriesMetadata = readText(settings.outputPath / "OME" / "zarr.json");
        require(rootMetadata.find("\"bioformats2raw.layout\":3") != std::string::npos,
                "OME-Zarr bioformats2raw layout marker is missing");
        require(seriesMetadata.find("\"series\":[\"Alpha\",\"Beta\"]")
                    != std::string::npos,
                "OME-Zarr series list is incorrect");
    }

    void testValidationAndOverwrite(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.outputPath = root / "validation.ome.tiff";
        settings.width = 2;
        settings.height = 2;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;
        std::vector<std::uint8_t> frame(4, 31);

        const auto rejects = [&settings](const std::string& expected)
        {
            scopewriter::Writer writer;
            require(!writer.open(settings), "Invalid writer settings were accepted");
            require(writer.lastError().find(expected) != std::string::npos,
                    "Invalid writer settings returned the wrong error");
        };

        settings.acquisitionOrder = "TCC";
        rejects("Acquisition order");
        settings.acquisitionOrder = "CTZ";
        settings.timeCount = 0;
        rejects("unbounded T");
        settings.acquisitionOrder = "TCZ";
        settings.channels = {
            scopewriter::ChannelMetadata{.name = "Same"},
            scopewriter::ChannelMetadata{.name = "Same"}
        };
        settings.channelCount = 2;
        rejects("unique");
        settings.channels.clear();
        settings.channelCount = 1;
        settings.positionCount = 2;
        settings.positions = {
            scopewriter::PositionMetadata{.name = "Unsafe/Name"},
            scopewriter::PositionMetadata{.name = "Safe"}
        };
        rejects("path components");
        settings.positionCount = 1;
        settings.positions = {
            scopewriter::PositionMetadata{.name = "Position", .gridRow = 1}
        };
        rejects("Position metadata");
        settings.positions.clear();
        settings.physicalSizeXUm = (std::numeric_limits<double>::quiet_NaN)();
        rejects("Physical sizes");
        settings.physicalSizeXUm = 0.0;
        settings.uuid = "not-a-uuid";
        rejects("UUID");

        settings.uuid.clear();
        settings.format = scopewriter::Format::OmeZarr;
        settings.zarrChunkWidth = 0;
        rejects("chunk and shard");
        settings.format = scopewriter::Format::OmeTiff;
        settings.zarrChunkWidth = 512;
        settings.timeCount = 1;
        scopewriter::Writer boundedTiff;
        require(boundedTiff.open(settings), boundedTiff.lastError());
        require(!boundedTiff.append(frame.data(), frame.size() - 1),
                "OME-TIFF accepted an invalid frame byte count");
        require(boundedTiff.append(frame.data(), frame.size()), boundedTiff.lastError());
        require(!boundedTiff.append(frame.data(), frame.size()),
                "OME-TIFF accepted more than the configured number of frames");
        require(boundedTiff.lastError().find("all configured frames") != std::string::npos,
                "OME-TIFF bounded stream returned the wrong error");
        require(boundedTiff.close(), boundedTiff.lastError());

        scopewriter::Writer existingTiff;
        require(!existingTiff.open(settings), "OME-TIFF replaced existing data by default");
        settings.overwrite = true;
        scopewriter::Writer replacementTiff;
        require(replacementTiff.open(settings), replacementTiff.lastError());
        require(replacementTiff.append(frame.data(), frame.size()), replacementTiff.lastError());
        require(replacementTiff.close(), replacementTiff.lastError());

        settings.format = scopewriter::Format::OmeZarr;
        settings.outputPath = root / "validation.ome.zarr";
        settings.overwrite = false;
        scopewriter::Writer boundedZarr;
        require(boundedZarr.open(settings), boundedZarr.lastError());
        scopewriter::FrameMetadata first;
        first.t = 0;
        require(boundedZarr.append(frame.data(), frame.size(), first), boundedZarr.lastError());
        require(!boundedZarr.append(frame.data(), frame.size(), first),
                "OME-Zarr accepted a duplicate coordinate");
        require(boundedZarr.lastError().find("duplicate") != std::string::npos,
                "OME-Zarr duplicate coordinate returned the wrong error");
        require(boundedZarr.close(), boundedZarr.lastError());

        scopewriter::Writer existingZarr;
        require(!existingZarr.open(settings), "OME-Zarr replaced existing data by default");
        settings.overwrite = true;
        scopewriter::Writer replacementZarr;
        require(replacementZarr.open(settings), replacementZarr.lastError());
        require(replacementZarr.append(frame.data(), frame.size()), replacementZarr.lastError());
        require(replacementZarr.close(), replacementZarr.lastError());
    }

    void testPlainTiff(const std::filesystem::path& root)
    {
        for (const bool sixteenBit : {false, true})
        {
            const auto path = root / (sixteenBit ? "plain16.tif" : "plain8.tif");
            scopewriter::WriterSettings settings;
            settings.format = scopewriter::Format::Tiff;
            settings.outputPath = path;
            settings.linkedMetadataFile = "session \"metadata\".json";
            settings.width = 3;
            settings.height = 2;
            settings.pixelType = sixteenBit ? scopewriter::PixelType::UInt16
                                            : scopewriter::PixelType::UInt8;
            settings.significantBits = sixteenBit ? 12 : 8;
            settings.enableCompression = sixteenBit;

            std::vector<std::uint16_t> words{1, 2, 3, 4, 5, 6};
            std::vector<std::uint8_t> bytes{1, 2, 3, 4, 5, 6};
            const void* pixels = sixteenBit ? static_cast<const void*>(words.data())
                                            : static_cast<const void*>(bytes.data());
            const std::size_t byteCount = sixteenBit ? words.size() * sizeof(std::uint16_t)
                                                     : bytes.size();
            scopewriter::FrameMetadata metadata;
            metadata.cameraId = "Camera \"A\"";
            metadata.frameIndex = 7;
            metadata.timestampNs = 123456789;
            metadata.sourceRoiX = 10;
            metadata.sourceRoiY = 20;
            metadata.sourceRoiWidth = 3;
            metadata.sourceRoiHeight = 2;

            scopewriter::Writer writer;
            require(writer.open(settings), writer.lastError());
            require(!writer.append(pixels, byteCount - 1, metadata),
                    "Plain TIFF accepted an invalid frame byte count");
            require(writer.append(pixels, byteCount, metadata), writer.lastError());
            metadata.frameIndex = 8;
            require(writer.append(pixels, byteCount, metadata), writer.lastError());
            require(writer.close(), writer.lastError());

            TIFF* tiff = openTiff(path);
            require(tiff != nullptr && TIFFNumberOfDirectories(tiff) == 2,
                    "Plain TIFF page count is incorrect");
            char* description = nullptr;
            require(TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &description) == 1
                        && description != nullptr,
                    "Plain TIFF frame metadata is missing");
            const std::string json(description);
            require(json.find("\"schema\":\"scopewriter.frame-metadata.v1\"")
                        != std::string::npos
                        && json.find("\"linked_metadata_file\":\"session \\\"metadata\\\".json\"")
                        != std::string::npos
                        && json.find("\"camera_id\":\"Camera \\\"A\\\"\"")
                        != std::string::npos
                        && json.find("\"bits_per_sample\":"
                                     + std::to_string(settings.significantBits))
                            != std::string::npos,
                    "Plain TIFF JSON metadata is incorrect");
            std::vector<std::uint8_t> decoded(byteCount);
            require(TIFFReadEncodedStrip(tiff, 0, decoded.data(),
                                         static_cast<tmsize_t>(decoded.size()))
                        == static_cast<tmsize_t>(decoded.size()),
                    "Failed to decode the plain TIFF frame");
            require(std::equal(decoded.begin(), decoded.end(),
                               static_cast<const std::uint8_t*>(pixels)),
                    "Plain TIFF pixels changed during writing");
            TIFFClose(tiff);

            scopewriter::Writer existing;
            require(!existing.open(settings), "Plain TIFF replaced existing data by default");
            settings.overwrite = true;
            scopewriter::Writer replacement;
            require(replacement.open(settings), replacement.lastError());
            require(replacement.append(pixels, byteCount, metadata), replacement.lastError());
            require(replacement.close(), replacement.lastError());
        }

        scopewriter::WriterSettings emptySettings;
        emptySettings.format = scopewriter::Format::Tiff;
        emptySettings.outputPath = root / "empty.tif";
        emptySettings.width = 1;
        emptySettings.height = 1;
        emptySettings.pixelType = scopewriter::PixelType::UInt8;
        emptySettings.significantBits = 8;
        scopewriter::Writer empty;
        require(empty.open(emptySettings), empty.lastError());
        require(empty.close(), empty.lastError());
        require(!std::filesystem::exists(emptySettings.outputPath),
                "An empty plain TIFF output was retained");
    }

    void testBinary(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.format = scopewriter::Format::Binary;
        settings.outputPath = root / "frames.bin";
        settings.frameMetadataPath = root / "frames_frameinfo.csv";
        settings.width = 2;
        settings.height = 2;
        settings.pixelType = scopewriter::PixelType::UInt16;
        settings.significantBits = 12;

        const std::vector<std::uint8_t> first{1, 2, 3, 4, 90, 91, 5, 6, 7, 8, 92, 93};
        const std::vector<std::uint8_t> second{9, 10, 11, 12, 94, 95, 13, 14, 15, 16, 96, 97};
        scopewriter::FrameMetadata metadata;
        metadata.cameraId = "Camera, \"A\"";
        metadata.frameIndex = 21;
        metadata.timestampNs = 987654321;
        metadata.stride = 6;
        metadata.sourceRoiX = 3;
        metadata.sourceRoiY = 4;
        metadata.sourceRoiWidth = 2;
        metadata.sourceRoiHeight = 2;

        scopewriter::Writer writer;
        require(writer.open(settings), writer.lastError());
        require(!writer.append(first.data(), first.size() - 1, metadata),
                "Binary writer accepted a payload inconsistent with stride");
        require(writer.append(first.data(), first.size(), metadata), writer.lastError());
        metadata.frameIndex = 22;
        require(writer.append(second.data(), second.size(), metadata), writer.lastError());
        require(writer.close(), writer.lastError());

        std::vector<char> expected;
        expected.insert(expected.end(), first.begin(), first.end());
        expected.insert(expected.end(), second.begin(), second.end());
        require(readBinary(settings.outputPath) == expected,
                "Binary writer changed frame bytes or padding");
        const std::string csv = readText(settings.frameMetadataPath);
        require(csv.starts_with(std::string(scopewriter::kBinaryFrameMetadataHeader) + '\n')
                    && csv.find("\"Camera, \"\"A\"\"\",21,987654321,2,2,12,6,Mono16,1,12,3,4,2,2\n")
                        != std::string::npos,
                "Binary frame metadata CSV is incorrect");

        scopewriter::Writer existing;
        require(!existing.open(settings), "Binary writer replaced existing data by default");
        settings.overwrite = true;
        scopewriter::Writer replacement;
        require(replacement.open(settings), replacement.lastError());
        require(replacement.append(first.data(), first.size(), metadata), replacement.lastError());
        require(replacement.close(), replacement.lastError());

        settings.outputPath = root / "empty.bin";
        settings.frameMetadataPath = root / "empty_frameinfo.csv";
        settings.overwrite = false;
        scopewriter::Writer empty;
        require(empty.open(settings), empty.lastError());
        require(empty.close(), empty.lastError());
        require(!std::filesystem::exists(settings.outputPath)
                    && !std::filesystem::exists(settings.frameMetadataPath),
                "An empty binary output was retained");
    }

    void testEmptyAndPartialTiff(const std::filesystem::path& root)
    {
        scopewriter::WriterSettings settings;
        settings.outputPath = root / "empty.ome.tiff";
        settings.width = 2;
        settings.height = 2;
        settings.pixelType = scopewriter::PixelType::UInt8;
        settings.significantBits = 8;

        scopewriter::Writer empty;
        require(empty.open(settings), empty.lastError());
        require(empty.close(), empty.lastError());
        require(!std::filesystem::exists(settings.outputPath),
                "An empty acquisition left an invalid TIFF file");

        settings.outputPath = root / "partial.ome.tiff";
        settings.positionCount = 2;
        settings.positions = {
            scopewriter::PositionMetadata{.name = "Missing"},
            scopewriter::PositionMetadata{.name = "Captured"}
        };
        std::vector<std::uint8_t> frame(4, 47);
        scopewriter::FrameMetadata captured;
        captured.positionIndex = 1;
        scopewriter::Writer partial;
        require(partial.open(settings), partial.lastError());
        require(partial.append(frame.data(), frame.size(), captured), partial.lastError());
        require(partial.close(), partial.lastError());

        const auto directory = root / "partial";
        const auto missing = directory / "partial_p000.ome.tiff";
        const auto present = directory / "partial_p001.ome.tiff";
        require(!std::filesystem::exists(missing) && std::filesystem::exists(present),
                "Partial multi-position TIFF left an invalid empty file");
        const std::string xml = readTiffDescription(present);
        require(xml.find("Image:0") == std::string::npos
                    && xml.find("Image:1") != std::string::npos
                    && xml.find("Captured") != std::string::npos,
                "Partial multi-position TIFF metadata does not describe acquired data");
    }
}

int main(int argc, char** argv)
{
    const bool preserveOutput = argc > 1;
    const auto root = preserveOutput ? std::filesystem::path(argv[1]) : testRoot();
    try
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        testDependencyVersions();
        testOmeTiff(root);
        testOmeZarr(root);
        testOmeZarrMultiChunkShard(root);
        testOmeZarrConfigurableShards(root);
        testChannelsAndMetadata(root);
        testAcquisitionOrder(root);
        testMultiPositionLayout(root);
        testValidationAndOverwrite(root);
        testEmptyAndPartialTiff(root);
        testPlainTiff(root);
        testBinary(root);
        if (!preserveOutput)
        {
            std::filesystem::remove_all(root);
        }
        std::cout << "ScopeWriter tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        if (!preserveOutput)
        {
            std::filesystem::remove_all(root, ignored);
        }
        return 1;
    }
}
