#pragma once

#include "scopewriter/ScopeWriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scopewriter::internal
{
    enum class SampleKind
    {
        Unsigned,
        Signed,
        Float
    };

    // Name of each pixel type in every storage format
    struct PixelTypeInfo
    {
        PixelType type;
        SampleKind kind;
        std::size_t bytes;
        const char* ome;        // OME-XML Pixels Type
        const char* zarr;       // Zarr V3 data_type
        const char* binaryName; // Binary index pixel format name
        unsigned int binaryId;  // Binary index pixel format id
    };

    inline constexpr std::array<PixelTypeInfo, 8> kPixelTypes{{
        {PixelType::UInt8, SampleKind::Unsigned, 1, "uint8", "uint8", "Mono8", 0},
        {PixelType::UInt16, SampleKind::Unsigned, 2, "uint16", "uint16", "Mono16", 1},
        {PixelType::UInt32, SampleKind::Unsigned, 4, "uint32", "uint32", "UInt32", 2},
        {PixelType::Int8, SampleKind::Signed, 1, "int8", "int8", "Int8", 3},
        {PixelType::Int16, SampleKind::Signed, 2, "int16", "int16", "Int16", 4},
        {PixelType::Int32, SampleKind::Signed, 4, "int32", "int32", "Int32", 5},
        {PixelType::Float32, SampleKind::Float, 4, "float", "float32", "Float32", 6},
        {PixelType::Float64, SampleKind::Float, 8, "double", "float64", "Float64", 7},
    }};

    inline const PixelTypeInfo& pixelTypeInfo(PixelType type)
    {
        for (const auto& info : kPixelTypes)
        {
            if (info.type == type)
            {
                return info;
            }
        }
        return kPixelTypes.front();
    }

    inline const PixelTypeInfo* findZarrType(std::string_view name)
    {
        for (const auto& info : kPixelTypes)
        {
            if (name == info.zarr)
            {
                return &info;
            }
        }
        return nullptr;
    }

    inline const PixelTypeInfo* findBinaryType(unsigned int id, std::string_view name)
    {
        for (const auto& info : kPixelTypes)
        {
            if (id == info.binaryId && name == info.binaryName)
            {
                return &info;
            }
        }
        return nullptr;
    }

    // TIFF SampleFormat values: 1 unsigned, 2 signed, 3 IEEE floating point
    inline std::uint16_t tiffSampleFormat(SampleKind kind)
    {
        switch (kind)
        {
        case SampleKind::Signed:
            return 2;
        case SampleKind::Float:
            return 3;
        case SampleKind::Unsigned:
            break;
        }
        return 1;
    }

    inline const PixelTypeInfo* findTiffType(std::uint16_t bitsPerSample,
                                             std::uint16_t sampleFormat)
    {
        for (const auto& info : kPixelTypes)
        {
            if (info.bytes * 8 == bitsPerSample
                && tiffSampleFormat(info.kind) == sampleFormat)
            {
                return &info;
            }
        }
        return nullptr;
    }
}
