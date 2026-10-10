# ScopeWriter

<p align="center">
  <img src="resources/ScopeWriter_Icon.svg" alt="ScopeWriter logo" width="180">
</p>

[![Compile Check](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml)

ScopeWriter is a C++ library for writing microscopy image streams.

## Formats

| Format | Output | Compression |
| --- | --- | --- |
| OME-TIFF | BigTIFF with OME-XML | Deflate or none |
| OME-Zarr | OME-NGFF 0.5 on Zarr V3 | Zstd or none |
| TIFF | Multi-page BigTIFF with per-frame JSON metadata | Deflate or none |
| Binary | Raw frames with a CSV index | None |

Supports monochrome frames of `UInt8`, `UInt16`, `UInt32`, `Int8`, `Int16`, `Int32`,
`Float32` and `Float64`, TCZP coordinates, physical metadata, unbounded time series
and row strides. A zero stride means tightly packed rows; zero `significantBits` uses
the pixel type width. Frames are written in the byte order of the host, which must be
little endian.

| Pixel type | OME-XML | Zarr V3 | TIFF |
| --- | --- | --- | --- |
| `UInt8`, `UInt16`, `UInt32` | `uint8`, `uint16`, `uint32` | same | unsigned integer |
| `Int8`, `Int16`, `Int32` | `int8`, `int16`, `int32` | same | signed integer |
| `Float32`, `Float64` | `float`, `double` | `float32`, `float64` | IEEE floating point |

OME-Zarr can store a resolution pyramid, which viewers such as napari and Neuroglancer use to
show large images quickly. Set `zarrPyramidLevels` to the number of levels, or to 0 to halve the
image until it fits in one chunk. Each level halves X and Y by averaging 2x2 blocks (integers
round half up) and is listed in `multiscales` with the scale of its level. The default of 1
writes the full resolution only.

Multi-position OME-Zarr follows the `bioformats2raw.layout` container: one group per
position and an `OME` group that lists the series and holds `METADATA.ome.xml`.

## Validation

`tests/validate_outputs.py` checks the files written by `ScopeWriterTests` with
independent readers: OME-XML against the OME schema with `ome-types`, OME-Zarr against the
OME-NGFF 0.5 models of `ome-zarr-models`, and the pixels of every format with `tifffile`
and `zarr`. CI runs it on every change.

```bash
build/ScopeWriterTests outputs
pip install -r tests/requirements.txt
python tests/validate_outputs.py outputs
```

## Build

Requires CMake 3.23 or newer and a C++20 compiler. libtiff, zlib, Zstandard, and CRC32C are declared in `vcpkg.json` and installed by vcpkg when the vcpkg toolchain is used.

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

## Use

From source (the parent project must provide the same dependencies, for example by listing them in its own `vcpkg.json`):

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
