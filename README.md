# ScopeWriter

[![Compile Check](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml)

ScopeWriter is a reusable C++20 library for streaming microscopy frames to
standard image and dataset formats. It is an independent CMake package with no
source-level dependency on ScopeOne.

ScopeWriter is under active development. Its API and metadata model may change
before the first stable release.

## Formats

| Format | Output | Compression |
| --- | --- | --- |
| OME-TIFF | BigTIFF with OME-XML | Deflate or none |
| OME-Zarr | OME-NGFF 0.5 on Zarr V3 | Zstd or none |
| TIFF | Multi-page BigTIFF with per-frame JSON metadata | Deflate or none |
| Binary | Raw frame stream with a CSV index | None |

ScopeWriter supports unsigned 8-bit and 16-bit frames, bounded or unbounded
time series, explicit T, C, Z and position coordinates, multi-position
datasets, physical and acquisition metadata, asynchronous OME-Zarr writing and
optional compression.

Each `append` call submits one YX frame. OME formats map frames to TCZYX storage
coordinates. `WriterSettings` defines the dataset and storage layout, while
`FrameMetadata` identifies an individual frame. The caller retains ownership of
the submitted frame buffer after `append` returns.

Existing destinations are rejected unless `WriterSettings::overwrite` is
enabled. Check every operation's return value and use `lastError()` on failure.
`flush()` waits for queued writes and checkpoints visible metadata. `close()`
drains pending work and finalizes the dataset.

`WriterSettings::linkedMetadataFile` records a sidecar JSON file name in TIFF,
OME-TIFF and OME-Zarr metadata.

## Build

Requirements are CMake 3.23 or newer and a C++20 compiler. Dependencies are
bundled, so configuration does not download packages.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

Tests are enabled when ScopeWriter is the top-level project and disabled when
it is included by another project. Set `SCOPEWRITER_BUILD_TESTS` explicitly to
override this behavior.

## Use as an installed package

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
settings.linkedMetadataFile = "image_metadata.json";
settings.width = 512;
settings.height = 512;
settings.pixelType = scopewriter::PixelType::UInt16;
settings.significantBits = 16;
settings.timeCount = 100;
settings.channelCount = 1;
settings.zCount = 1;
settings.acquisitionOrder = "TCZ";

scopewriter::Writer writer;
if (!writer.open(settings)
    || !writer.append(frame.data(), frame.size() * sizeof(std::uint16_t))
    || !writer.close()) {
    throw std::runtime_error(writer.lastError());
}
```

ScopeOne consumes the source tree directly as a Git submodule. Other projects
can either do the same with `add_subdirectory` or use the installed CMake
package shown above.

## Dependencies and license

ScopeWriter builds static copies of libtiff, zlib, Zstandard and CRC32C. Its
OME-Zarr implementation contains code derived from acquire-zarr. Dependency
versions are fixed by the repository.

ScopeWriter is distributed under the BSD 3-Clause License. Third-party license
and attribution requirements are documented in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), with upstream license files
retained under `third_party`.

## Current scope

ScopeWriter targets deterministic TCZP microscopy acquisition on local
filesystems. Plate and HCS layouts, pyramids, cloud object stores, live read
views, arbitrary dimensions, RGB pixels and floating-point pixels are not yet
supported.
