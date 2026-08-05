# ScopeWriter

[![Compile Check](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml)

ScopeWriter is a C++ library for writing microscopy image streams.

## Formats

| Format | Output | Compression |
| --- | --- | --- |
| OME-TIFF | BigTIFF with OME-XML | Deflate or none |
| OME-Zarr | OME-NGFF 0.5 on Zarr V3 | Zstd or none |
| TIFF | Multi-page BigTIFF with per-frame JSON metadata | Deflate or none |
| Binary | Raw frames with a CSV index | None |

Supports unsigned 8-bit and 16-bit frames, TCZP coordinates, physical metadata,
unbounded time series and row strides. A zero stride means tightly packed rows;
zero `significantBits` uses the pixel type width.

## Build

Requires CMake 3.23 or newer and a C++20 compiler. Dependencies are bundled.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

## Use

From source:

```cmake
add_subdirectory(ScopeWriter)
target_link_libraries(my_application PRIVATE ScopeWriter::ScopeWriter)
```

From an installed package:

```cmake
find_package(ScopeWriter CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE ScopeWriter::ScopeWriter)
```

```cpp
#include <scopewriter/ScopeWriter.h>
#include <stdexcept>

scopewriter::WriterSettings settings;
settings.format = scopewriter::Format::OmeTiff;
settings.outputPath = "image.ome.tiff";
settings.width = 512;
settings.height = 512;
settings.pixelType = scopewriter::PixelType::UInt16;

scopewriter::Writer writer;
if (!writer.open(settings))
    throw std::runtime_error(writer.lastError());
if (!writer.append(frame.data(), frame.size() * sizeof(std::uint16_t)))
    throw std::runtime_error(writer.lastError());
if (!writer.close())
    throw std::runtime_error(writer.lastError());
```

See [`ScopeWriter.h`](include/scopewriter/ScopeWriter.h) for the complete API.

## License

ScopeWriter uses the BSD 3-Clause License. Bundled dependencies and derived code
retain their original terms in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
