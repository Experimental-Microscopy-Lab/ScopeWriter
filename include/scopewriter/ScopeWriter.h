#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "scopewriter/scopewriter_export.h"

namespace scopewriter
{
    inline constexpr char kFrameMetadataProtocol[] = "scopewriter.frame-metadata.v1";
    inline constexpr char kBinaryFrameMetadataHeader[] =
        "camera_id,frame_index,timestamp_ns,width,height,bits_per_sample,stride,"
        "pixel_format,pixel_format_id,payload_bytes,source_roi_x,source_roi_y,"
        "source_roi_width,source_roi_height";

    enum class Format
    {
        OmeTiff,
        OmeZarr,
        Tiff,
        Binary
    };

    enum class PixelType
    {
        UInt8,
        UInt16
    };

    struct DetectorMetadata
    {
        std::string manufacturer;
        std::string model;
        std::string serialNumber;
        std::optional<double> offset;
    };

    using MetadataMap = std::map<std::string, std::string>;
    using MetadataNamespaces = std::map<std::string, MetadataMap>;

    struct ChannelMetadata
    {
        std::string name;
        std::string fluorophore;
        std::optional<double> excitationWavelengthNm;
        std::optional<double> emissionWavelengthNm;
        std::optional<std::uint32_t> colorRGB;
    };

    struct PositionMetadata
    {
        std::string name;
        std::optional<int> gridRow;
        std::optional<int> gridColumn;
        std::optional<double> xUm;
        std::optional<double> yUm;
        std::optional<double> zUm;
    };

    struct WriterSettings
    {
        Format format{Format::OmeTiff};
        std::filesystem::path outputPath;
        std::filesystem::path frameMetadataPath;
        std::string linkedMetadataFile;
        int width{0};
        int height{0};
        PixelType pixelType{PixelType::UInt16};
        int significantBits{0};
        int positionCount{1};
        int timeCount{0};
        int channelCount{1};
        int zCount{1};
        std::string acquisitionOrder{"TCZ"};
        double physicalSizeXUm{0.0};
        double physicalSizeYUm{0.0};
        double physicalSizeZUm{0.0};
        double timeIncrementMs{0.0};
        double defaultExposureMs{0.0};
        std::uint64_t acquisitionStartTimestampNs{0};
        std::string imageName{"Image"};
        std::string creator{"ScopeWriter"};
        std::string uuid;
        DetectorMetadata detector;
        std::vector<ChannelMetadata> channels;
        std::vector<PositionMetadata> positions;
        MetadataNamespaces metadata;
        bool overwrite{false};
        bool enableCompression{true};
        int compressionLevel{6};
        int zarrChunkWidth{512};
        int zarrChunkHeight{512};
        int zarrShardWidthChunks{0};
        int zarrShardHeightChunks{0};
    };

    struct FrameMetadata
    {
        std::string cameraId;
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        std::size_t stride{0};
        int sourceRoiX{0};
        int sourceRoiY{0};
        int sourceRoiWidth{0};
        int sourceRoiHeight{0};
        int positionIndex{0};
        std::int64_t t{-1};
        int c{0};
        int z{0};
        double exposureMs{0.0};
        std::optional<double> positionXUm;
        std::optional<double> positionYUm;
        std::optional<double> positionZUm;
        MetadataMap metadata;
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
    class SCOPEWRITER_API Writer
    {
    public:
        Writer();
        ~Writer();

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        Writer(Writer&&) noexcept;
        Writer& operator=(Writer&&) noexcept;

        bool open(const WriterSettings& settings);
        bool append(const void* data, std::size_t byteCount, const FrameMetadata& metadata = {});
        bool close();
        bool isOpen() const noexcept;
        const std::string& lastError() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
