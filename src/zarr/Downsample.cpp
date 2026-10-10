#include "Downsample.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace scopewriter::internal::zarr
{
    namespace
    {
        // Divide rounding half up for a positive divisor
        std::int64_t roundedQuotient(std::int64_t sum, std::int64_t count)
        {
            const std::int64_t numerator = 2 * sum + count;
            const std::int64_t divisor = 2 * count;
            std::int64_t quotient = numerator / divisor;
            if (numerator % divisor < 0)
            {
                --quotient;
            }
            return quotient;
        }

        template <typename T>
        T average(std::int64_t sum, int count)
        {
            return static_cast<T>(roundedQuotient(sum, count));
        }

        template <typename T>
        void downsample(const std::uint8_t* sourceBytes,
                        int width,
                        int height,
                        std::uint8_t* targetBytes)
        {
            const int targetWidth = (width + 1) / 2;
            const int targetHeight = (height + 1) / 2;
            for (int y = 0; y < targetHeight; ++y)
            {
                for (int x = 0; x < targetWidth; ++x)
                {
                    const int rows = (std::min)(2, height - 2 * y);
                    const int columns = (std::min)(2, width - 2 * x);
                    std::conditional_t<std::is_floating_point_v<T>, double, std::int64_t> sum = 0;
                    for (int row = 0; row < rows; ++row)
                    {
                        for (int column = 0; column < columns; ++column)
                        {
                            T sample;
                            std::memcpy(&sample,
                                        sourceBytes
                                            + (static_cast<std::size_t>(2 * y + row) * width
                                               + static_cast<std::size_t>(2 * x + column))
                                                * sizeof(T),
                                        sizeof(T));
                            sum += sample;
                        }
                    }
                    const int count = rows * columns;
                    T value;
                    if constexpr (std::is_floating_point_v<T>)
                    {
                        value = static_cast<T>(sum / count);
                    }
                    else
                    {
                        value = average<T>(sum, count);
                    }
                    std::memcpy(targetBytes
                                    + (static_cast<std::size_t>(y) * targetWidth
                                       + static_cast<std::size_t>(x))
                                        * sizeof(T),
                                &value,
                                sizeof(T));
                }
            }
        }
    }

    void downsample2x(PixelType type,
                      const std::uint8_t* source,
                      int width,
                      int height,
                      std::uint8_t* target)
    {
        switch (type)
        {
        case PixelType::UInt8:
            downsample<std::uint8_t>(source, width, height, target);
            break;
        case PixelType::UInt16:
            downsample<std::uint16_t>(source, width, height, target);
            break;
        case PixelType::UInt32:
            downsample<std::uint32_t>(source, width, height, target);
            break;
        case PixelType::Int8:
            downsample<std::int8_t>(source, width, height, target);
            break;
        case PixelType::Int16:
            downsample<std::int16_t>(source, width, height, target);
            break;
        case PixelType::Int32:
            downsample<std::int32_t>(source, width, height, target);
            break;
        case PixelType::Float32:
            downsample<float>(source, width, height, target);
            break;
        case PixelType::Float64:
            downsample<double>(source, width, height, target);
            break;
        }
    }
}
