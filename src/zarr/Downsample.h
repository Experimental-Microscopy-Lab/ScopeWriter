#pragma once

#include "scopewriter/ScopeWriter.h"

#include <cstdint>

namespace scopewriter::internal::zarr
{
    // Halve a tightly packed plane in X and Y by averaging 2x2 blocks. Odd edges
    // average the pixels they have. The target holds ((width + 1) / 2) by
    // ((height + 1) / 2) pixels. Integers round half up, floating point is exact
    // to the sample type.
    void downsample2x(PixelType type,
                      const std::uint8_t* source,
                      int width,
                      int height,
                      std::uint8_t* target);
}
