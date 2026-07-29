// Portions of this implementation are derived from acquire-zarr 0.8.1
// This file was modified for ScopeWriter's filesystem-only OME-Zarr backend

#include "ZarrWriter.h"
#include "zarr/Chunk.h"
#include "zarr/FileHandle.h"
#include "zarr/FrameQueue.h"
#include "zarr/Shard.h"
#include "zarr/ThreadPool.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace scopewriter::internal
{
    namespace
    {
        int chunkWidth(const WriterSettings& settings)
        {
            return (std::min)(settings.width, settings.zarrChunkWidth);
        }

        int chunkHeight(const WriterSettings& settings)
        {
            return (std::min)(settings.height, settings.zarrChunkHeight);
        }

        int chunkCount(int extent, int chunkExtent)
        {
            return (extent + chunkExtent - 1) / chunkExtent;
        }

        int shardChunkCount(int totalChunks, int configuredChunks)
        {
            return configuredChunks > 0
                ? (std::min)(totalChunks, configuredChunks)
                : totalChunks;
        }

        std::string jsonEscape(const std::string& value)
        {
            std::ostringstream output;
            output << '"';
            for (const unsigned char character : value)
            {
                switch (character)
                {
                case '"': output << "\\\""; break;
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
            output << '"';
            return output.str();
        }

        std::string number(double value)
        {
            std::ostringstream output;
            output << std::setprecision(17) << value;
            return output.str();
        }

        std::string rgbHex(std::uint32_t color)
        {
            std::ostringstream output;
            output << std::uppercase << std::hex << std::setw(6) << std::setfill('0')
                   << (color & 0x00ffffffu);
            return output.str();
        }

        bool writeFile(const std::filesystem::path& path,
                       const void* data,
                       std::size_t byteCount,
                       std::string& error)
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(path.parent_path(), filesystemError);
            if (filesystemError)
            {
                error = "Failed to create OME-Zarr directory: " + filesystemError.message();
                return false;
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "Failed to open OME-Zarr file for writing";
                return false;
            }
            output.write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
            output.close();
            if (!output)
            {
                error = "Failed to write OME-Zarr file";
                return false;
            }
            return true;
        }

        bool writeTextFile(const std::filesystem::path& path,
                           const std::string& text,
                           std::string& error)
        {
            return writeFile(path, text.data(), text.size(), error);
        }

        std::string groupMetadata(const WriterSettings& settings,
                                  const std::string& customMetadata,
                                  int positionIndex)
        {
            std::ostringstream json;
            json << "{\n"
                 << "  \"zarr_format\": 3,\n"
                 << "  \"consolidated_metadata\": null,\n"
                 << "  \"node_type\": \"group\",\n"
                 << "  \"attributes\": {\n"
                 << "    \"scopewriter\": " << customMetadata << ",\n"
                 << "    \"ome\": {\n"
                 << "      \"version\": \"0.5\",\n"
                 << "      \"name\": \"/\",\n"
                 << "      \"multiscales\": [{\n"
                 << "        \"axes\": [";

            const auto axis = [&json](const char* name, const char* type, const char* unit)
            {
                json << "{\"name\":" << jsonEscape(name)
                     << ",\"type\":" << jsonEscape(type);
                if (unit != nullptr)
                {
                    json << ",\"unit\":" << jsonEscape(unit);
                }
                json << '}';
            };
            axis("t", "time", settings.timeIncrementMs > 0.0 ? "millisecond" : nullptr);
            json << ',';
            axis("c", "channel", nullptr);
            json << ',';
            axis("z", "space", settings.physicalSizeZUm > 0.0 ? "micrometer" : nullptr);
            json << ',';
            axis("y", "space", settings.physicalSizeYUm > 0.0 ? "micrometer" : nullptr);
            json << ',';
            axis("x", "space", settings.physicalSizeXUm > 0.0 ? "micrometer" : nullptr);

            json << "],\n"
                 << "        \"datasets\": [{\"path\":\"0\","
                 << "\"coordinateTransformations\":[{\"type\":\"scale\",\"scale\":["
                 << (settings.timeIncrementMs > 0.0 ? number(settings.timeIncrementMs) : "1")
                 << ",1,"
                 << (settings.physicalSizeZUm > 0.0 ? number(settings.physicalSizeZUm) : "1")
                 << ','
                 << (settings.physicalSizeYUm > 0.0 ? number(settings.physicalSizeYUm) : "1")
                 << ','
                 << (settings.physicalSizeXUm > 0.0 ? number(settings.physicalSizeXUm) : "1")
                 << "]}";
            if (!settings.positions.empty())
            {
                const auto& position = settings.positions[static_cast<std::size_t>(positionIndex)];
                if (position.xUm || position.yUm || position.zUm)
                {
                    json << ",{" << "\"type\":\"translation\",\"translation\":[0,0,"
                         << (position.zUm ? number(*position.zUm) : "0") << ','
                         << (position.yUm ? number(*position.yUm) : "0") << ','
                         << (position.xUm ? number(*position.xUm) : "0") << "]}";
                }
            }
            json << "]}]}],\n"
                 << "      \"omero\": {\"channels\":[";
            const std::uint64_t windowEnd = (std::uint64_t{1} << settings.significantBits) - 1;
            for (int channelIndex = 0; channelIndex < settings.channelCount; ++channelIndex)
            {
                if (channelIndex != 0)
                    json << ',';
                const ChannelMetadata* channel = settings.channels.empty()
                    ? nullptr
                    : &settings.channels[static_cast<std::size_t>(channelIndex)];
                const std::string label = channel != nullptr && !channel->name.empty()
                    ? channel->name
                    : "Channel " + std::to_string(channelIndex + 1);
                json << "{\"label\":" << jsonEscape(label)
                     << ",\"color\":" << jsonEscape(channel != nullptr && channel->colorRGB
                                                           ? rgbHex(*channel->colorRGB)
                                                           : "FFFFFF")
                     << ",\"window\":{\"start\":0,\"end\":" << windowEnd
                     << ",\"min\":0,\"max\":" << windowEnd << "}}";
            }
            json << "]}\n"
                 << "    }\n"
                 << "  }\n"
                 << "}\n";
            return json.str();
        }

        std::string rootGroupMetadata()
        {
            return "{\n"
                   "  \"zarr_format\": 3,\n"
                   "  \"node_type\": \"group\",\n"
                   "  \"attributes\": {\"ome\":{\"version\":\"0.5\","
                   "\"bioformats2raw.layout\":3}}\n"
                   "}\n";
        }

        std::string seriesGroupMetadata(const std::vector<std::string>& seriesNames)
        {
            std::ostringstream json;
            json << "{\n"
                 << "  \"zarr_format\": 3,\n"
                 << "  \"node_type\": \"group\",\n"
                 << "  \"attributes\": {\"ome\":{\"version\":\"0.5\",\"series\":[";
            for (std::size_t index = 0; index < seriesNames.size(); ++index)
            {
                if (index != 0)
                    json << ',';
                json << jsonEscape(seriesNames[index]);
            }
            json << "]}}\n}\n";
            return json.str();
        }

        std::string arrayMetadata(const WriterSettings& settings, std::int64_t sizeT)
        {
            const int chunkWidthValue = chunkWidth(settings);
            const int chunkHeightValue = chunkHeight(settings);
            const int chunksX = chunkCount(settings.width, chunkWidthValue);
            const int chunksY = chunkCount(settings.height, chunkHeightValue);
            const int shardChunksX = shardChunkCount(chunksX,
                                                     settings.zarrShardWidthChunks);
            const int shardChunksY = shardChunkCount(chunksY,
                                                     settings.zarrShardHeightChunks);
            const char* dataType = settings.pixelType == PixelType::UInt8 ? "uint8" : "uint16";

            std::ostringstream json;
            json << "{\n"
                 << "  \"shape\": [" << sizeT << ',' << settings.channelCount << ','
                 << settings.zCount << ','
                 << settings.height << ',' << settings.width << "],\n"
                 << "  \"chunk_grid\": {\"name\":\"regular\",\"configuration\":{"
                 << "\"chunk_shape\":[1,1,1," << shardChunksY * chunkHeightValue << ','
                 << shardChunksX * chunkWidthValue << "]}},\n"
                 << "  \"chunk_key_encoding\": {\"name\":\"default\","
                 << "\"configuration\":{\"separator\":\"/\"}},\n"
                 << "  \"fill_value\": 0,\n"
                 << "  \"attributes\": {},\n"
                 << "  \"zarr_format\": 3,\n"
                 << "  \"node_type\": \"array\",\n"
                 << "  \"storage_transformers\": [],\n"
                 << "  \"data_type\": " << jsonEscape(dataType) << ",\n"
                 << "  \"dimension_names\": [\"t\",\"c\",\"z\",\"y\",\"x\"],\n"
                 << "  \"codecs\": [{\"name\":\"sharding_indexed\",\"configuration\":{"
                 << "\"chunk_shape\":[1,1,1," << chunkHeightValue << ','
                 << chunkWidthValue << "],"
                 << "\"index_codecs\":[{\"name\":\"bytes\",\"configuration\":{"
                 << "\"endian\":\"little\"}},{\"name\":\"crc32c\"}],"
                 << "\"index_location\":\"end\",\"codecs\":[{\"name\":\"bytes\","
                 << "\"configuration\":{\"endian\":\"little\"}}";
            if (settings.enableCompression)
            {
                json << ",{\"name\":\"zstd\",\"configuration\":{\"level\":"
                     << settings.compressionLevel << ",\"checksum\":false}}";
            }
            json << "]}}]\n}\n";
            return json.str();
        }
    }

    struct ZarrWriter::Impl
    {
        void setWorkerError(std::string error)
        {
            {
                std::lock_guard lock(stateMutex);
                if (!workerError.empty())
                {
                    return;
                }
                workerError = error.empty() ? "OME-Zarr worker failed" : std::move(error);
            }
            if (frameQueue)
            {
                frameQueue->abort();
            }
        }

        std::string currentWorkerError() const
        {
            std::lock_guard lock(stateMutex);
            return workerError;
        }

        bool writeMetadata(std::string& error) const
        {
            if (settings.positionCount > 1
                && (!writeTextFile(settings.outputPath / "zarr.json", rootGroupMetadata(), error)
                    || !writeTextFile(settings.outputPath / "OME" / "zarr.json",
                                      seriesGroupMetadata(seriesNames),
                                      error)))
            {
                return false;
            }
            for (int position = 0; position < settings.positionCount; ++position)
            {
                const std::filesystem::path groupPath = position == 0 && settings.positionCount == 1
                    ? settings.outputPath
                    : settings.outputPath / seriesNames[static_cast<std::size_t>(position)];
                if (!writeTextFile(groupPath / "zarr.json",
                                   groupMetadata(settings,
                                                 seriesMetadata[static_cast<std::size_t>(position)],
                                                 position),
                                   error)
                    || !writeTextFile(groupPath / "0" / "zarr.json",
                                      arrayMetadata(settings,
                                                    sizeT[static_cast<std::size_t>(position)]),
                                      error))
                {
                    return false;
                }
            }
            return true;
        }

        bool processFrame(zarr::FrameQueue::Frame frame, std::string& error)
        {
            while (!shards.empty() && shards.front().expired())
            {
                shards.pop_front();
            }
            const int chunkWidthValue = chunkWidth(settings);
            const int chunkHeightValue = chunkHeight(settings);
            const int chunksX = chunkCount(settings.width, chunkWidthValue);
            const int chunksY = chunkCount(settings.height, chunkHeightValue);
            const int shardChunksX = shardChunkCount(chunksX,
                                                     settings.zarrShardWidthChunks);
            const int shardChunksY = shardChunkCount(chunksY,
                                                     settings.zarrShardHeightChunks);
            const int shardsX = chunkCount(chunksX, shardChunksX);
            const int shardsY = chunkCount(chunksY, shardChunksY);
            const std::size_t sampleBytes = settings.pixelType == PixelType::UInt8 ? 1u : 2u;
            const std::size_t sourceStride = static_cast<std::size_t>(settings.width)
                * sampleBytes;
            const auto* source = frame.data.data();
            const std::size_t chunkBytes = static_cast<std::size_t>(chunkWidthValue)
                * static_cast<std::size_t>(chunkHeightValue) * sampleBytes;
            const std::size_t chunksPerShard = static_cast<std::size_t>(shardChunksX)
                * static_cast<std::size_t>(shardChunksY);
            for (int shardY = 0; shardY < shardsY; ++shardY)
            {
                for (int shardX = 0; shardX < shardsX; ++shardX)
                {
                    const std::filesystem::path shardPath = frame.groupPath / "0" / "c"
                        / std::to_string(frame.t) / std::to_string(frame.c)
                        / std::to_string(frame.z) / std::to_string(shardY)
                        / std::to_string(shardX);
                    auto shard = std::make_shared<zarr::Shard>(shardPath,
                                                               chunksPerShard,
                                                               handlePool);
                    shards.push_back(shard);
                    for (int localChunkY = 0; localChunkY < shardChunksY; ++localChunkY)
                    {
                        for (int localChunkX = 0; localChunkX < shardChunksX; ++localChunkX)
                        {
                            const int chunkX = shardX * shardChunksX + localChunkX;
                            const int chunkY = shardY * shardChunksY + localChunkY;
                            const std::size_t chunkIndex =
                                static_cast<std::size_t>(localChunkY) * shardChunksX
                                + static_cast<std::size_t>(localChunkX);
                            if (frame.zeroFill || chunkX >= chunksX || chunkY >= chunksY)
                            {
                                if (!shard->skipChunk(chunkIndex, error))
                                {
                                    return false;
                                }
                                continue;
                            }

                            const int sourceX = chunkX * chunkWidthValue;
                            const int sourceY = chunkY * chunkHeightValue;
                            const int copyWidth = (std::min)(chunkWidthValue,
                                                            settings.width - sourceX);
                            const int copyHeight = (std::min)(chunkHeightValue,
                                                             settings.height - sourceY);
                            auto chunk = std::make_shared<zarr::Chunk>(chunkBytes,
                                                                      sampleBytes);
                            chunk->writeRows(
                                0,
                                source + static_cast<std::size_t>(sourceY) * sourceStride
                                    + static_cast<std::size_t>(sourceX) * sampleBytes,
                                sourceStride,
                                static_cast<std::size_t>(copyWidth) * sampleBytes,
                                static_cast<std::size_t>(chunkWidthValue) * sampleBytes,
                                static_cast<std::size_t>(copyHeight));
                            if (!chunk->hasData())
                            {
                                if (!shard->skipChunk(chunkIndex, error))
                                {
                                    return false;
                                }
                                continue;
                            }
                            if (!threadPool->push(
                                    [this, shard, chunk, chunkIndex](std::string& taskError)
                                    {
                                        std::vector<std::uint8_t> payload;
                                        if (!chunk->compressAndTake(settings.enableCompression,
                                                                    settings.compressionLevel,
                                                                    payload,
                                                                    taskError))
                                        {
                                            return false;
                                        }
                                        return shard->writeChunk(chunkIndex,
                                                                 payload,
                                                                 taskError);
                                    }))
                            {
                                error = currentWorkerError();
                                if (error.empty())
                                {
                                    error = "OME-Zarr worker pool stopped accepting chunks";
                                }
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }

        void consumeFrames()
        {
            zarr::FrameQueue::Frame frame;
            while (frameQueue->pop(frame))
            {
                std::string error;
                try
                {
                    if (processFrame(std::move(frame), error))
                    {
                        continue;
                    }
                }
                catch (const std::exception& exception)
                {
                    error = exception.what();
                }
                setWorkerError(std::move(error));
                return;
            }
        }

        WriterSettings settings;
        std::vector<std::string> seriesNames;
        std::vector<std::string> seriesMetadata;
        std::vector<std::int64_t> sizeT;
        std::shared_ptr<zarr::FileHandlePool> handlePool;
        std::unique_ptr<zarr::FrameQueue> frameQueue;
        std::unique_ptr<zarr::ThreadPool> threadPool;
        std::thread frameConsumer;
        std::deque<std::weak_ptr<zarr::Shard>> shards;
        mutable std::mutex stateMutex;
        std::string workerError;
        bool open{false};
    };

    ZarrWriter::ZarrWriter()
        : m_impl(std::make_unique<Impl>())
    {
    }

    ZarrWriter::~ZarrWriter()
    {
        std::string ignored;
        close(ignored);
    }

    bool ZarrWriter::open(const WriterSettings& settings,
                          const std::vector<std::string>& seriesNames,
                          const std::vector<std::string>& seriesMetadata,
                          std::string& error)
    {
        if (m_impl->open && !close(error))
        {
            return false;
        }
        if (seriesNames.size() != static_cast<std::size_t>(settings.positionCount)
            || seriesMetadata.size() != static_cast<std::size_t>(settings.positionCount))
        {
            error = "OME-Zarr series metadata count is invalid";
            return false;
        }
        m_impl->settings = settings;
        m_impl->seriesNames = seriesNames;
        m_impl->seriesMetadata = seriesMetadata;
        m_impl->sizeT.assign(static_cast<std::size_t>(settings.positionCount), 0);
        m_impl->shards.clear();
        {
            std::lock_guard lock(m_impl->stateMutex);
            m_impl->workerError.clear();
        }

        if (!m_impl->writeMetadata(error))
        {
            return false;
        }

        const unsigned int hardwareThreads = (std::max)(1u, std::thread::hardware_concurrency());
        const unsigned int workerCount = hardwareThreads;
        const std::size_t frameBytes = static_cast<std::size_t>(settings.width)
            * static_cast<std::size_t>(settings.height)
            * (settings.pixelType == PixelType::UInt8 ? 1u : 2u);
        const std::size_t frameCapacity = (std::clamp)(
            static_cast<std::size_t>(workerCount) * 2,
            std::size_t{2},
            std::size_t{8});
        try
        {
            m_impl->handlePool = std::make_shared<zarr::FileHandlePool>();
            m_impl->frameQueue = std::make_unique<zarr::FrameQueue>(
                frameCapacity,
                frameBytes * frameCapacity);
            m_impl->threadPool = std::make_unique<zarr::ThreadPool>(
                workerCount,
                static_cast<std::size_t>(workerCount) * 2,
                [this](std::string workerError)
                {
                    m_impl->setWorkerError(std::move(workerError));
                });
            m_impl->frameConsumer = std::thread([this]
            {
                m_impl->consumeFrames();
            });
        }
        catch (const std::exception& exception)
        {
            if (m_impl->frameQueue)
            {
                m_impl->frameQueue->close();
            }
            if (m_impl->frameConsumer.joinable())
            {
                m_impl->frameConsumer.join();
            }
            if (m_impl->threadPool)
            {
                m_impl->threadPool->awaitStop();
            }
            m_impl->threadPool.reset();
            m_impl->frameQueue.reset();
            m_impl->handlePool.reset();
            error = "Failed to start OME-Zarr workers: " + std::string(exception.what());
            return false;
        }
        m_impl->open = true;
        return true;
    }

    bool ZarrWriter::append(int positionIndex,
                            std::int64_t t,
                            int c,
                            int z,
                            const void* data,
                            std::size_t byteCount,
                            std::string& error)
    {
        if (!m_impl->open)
        {
            error = "OME-Zarr writer is not open";
            return false;
        }
        const std::filesystem::path groupPath = positionIndex == 0
            && m_impl->settings.positionCount == 1
            ? m_impl->settings.outputPath
            : m_impl->settings.outputPath
                / m_impl->seriesNames[static_cast<std::size_t>(positionIndex)];

        zarr::FrameQueue::Frame frame;
        frame.groupPath = groupPath;
        frame.t = t;
        frame.c = c;
        frame.z = z;
        frame.zeroFill = data == nullptr;
        if (data != nullptr)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            frame.data.assign(bytes, bytes + byteCount);
        }

        if (!m_impl->frameQueue->push(std::move(frame)))
        {
            error = m_impl->currentWorkerError();
            if (error.empty())
            {
                error = "OME-Zarr writer is closing";
            }
            return false;
        }
        m_impl->sizeT[static_cast<std::size_t>(positionIndex)] = (std::max)(
            m_impl->sizeT[static_cast<std::size_t>(positionIndex)], t + 1);
        return true;
    }

    bool ZarrWriter::close(std::string& error)
    {
        if (!m_impl->open)
        {
            return true;
        }
        m_impl->frameQueue->close();
        if (m_impl->frameConsumer.joinable())
        {
            m_impl->frameConsumer.join();
        }
        m_impl->threadPool->awaitStop();
        m_impl->open = false;

        std::string workerError = m_impl->currentWorkerError();
        for (const auto& weakShard : m_impl->shards)
        {
            const auto shard = weakShard.lock();
            if (!shard)
            {
                continue;
            }
            std::string finalizeError;
            if (!shard->finalize(finalizeError) && workerError.empty())
            {
                workerError = std::move(finalizeError);
            }
        }
        const bool metadataWritten = workerError.empty() && m_impl->writeMetadata(error);
        m_impl->shards.clear();
        m_impl->threadPool.reset();
        m_impl->frameQueue.reset();
        m_impl->handlePool.reset();
        if (!workerError.empty())
        {
            error = std::move(workerError);
            return false;
        }
        return metadataWritten;
    }
}
