# ScopeWriter

[![Compile Check](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml)

ScopeWriter is a C++ library for writing microscopy image streams. It can be used as a CMake subproject or installed package.

The project is under active development and its API may change before 1.0.

## Formats

| Format | Output | Compression |
| --- | --- | --- |
| OME-TIFF | BigTIFF with OME-XML | Deflate or none |
| OME-Zarr | OME-NGFF 0.5 on Zarr V3 | Zstd or none |
| TIFF | Multi-page BigTIFF with per-frame JSON metadata | Deflate or none |
| Binary | Raw frames with a CSV index | None |

Unsigned 8-bit and 16-bit frames, TCZP coordinates, physical metadata and
unbounded time series are supported. Input rows may be tightly packed or use a
larger byte stride. A zero stride means tightly packed rows, and zero
`significantBits` uses the full width of the selected pixel type.

## Build

Requirements are CMake 3.23 or newer and a C++20 compiler. Dependencies are
bundled, so configuration does not download packages.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

Tests are enabled for standalone builds and disabled when used as a subproject.

## CMake integration

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
