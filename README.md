# ScopeWriter

[![Compile Check](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Experimental-Microscopy-Lab/ScopeWriter/actions/workflows/windows-ci.yml)

ScopeWriter is a reusable C++20 streaming storage library for microscopy frames.
It is an independent CMake package and has no source-level dependency on
ScopeOne. Applications provide frame buffers and metadata through the public
`scopewriter` API; ScopeWriter owns format encoding, compression, streaming,
finalization and format-specific sidecars.

## Project status

ScopeWriter is in an early stage of development. Its public APIs, metadata
model and internal architecture are not yet stable and may change without
backward compatibility. We are actively validating the library across
microscopy formats and acquisition workloads, and refining its design to make
it general-purpose, reliable and stable before declaring a stable release.

## Formats

| Format | `Format` value | Output | Compression |
| --- | --- | --- | --- |
| OME-TIFF | `OmeTiff` | BigTIFF with OME-XML | Deflate or none |
| OME-Zarr | `OmeZarr` | OME-NGFF 0.5 on Zarr V3 | Zstd or none |
| TIFF | `Tiff` | Multi-page BigTIFF with JSON `ImageDescription` | Deflate or none |
| Binary | `Binary` | Raw payload stream plus CSV frame index | None |

## Features

- OME-TIFF with BigTIFF, OME-XML, physical sizes and per-plane metadata
- OME-Zarr 0.5 on Zarr V3 with an internal filesystem streaming backend
- Multi-page BigTIFF with per-frame JSON metadata
- Raw binary frame streams with a CSV frame index sidecar
- Separate acquisition order and canonical TCZYX storage coordinates
- Bounded or unbounded time and explicit T, C, Z and position coordinates
- Multi-position OME-TIFF filesets and bioformats2raw OME-Zarr collections
- Sequential append with zero-filled gaps
- Bounded background queues and parallel OME-Zarr chunk compression
- Optional Deflate or Zstd compression
- Bundled, statically linked libtiff, zlib, zstd and crc32c
- No Qt, Python, FetchContent, vcpkg or external runtime packages

## Data model

Each `append` call supplies one YX camera frame. OME outputs map that frame to a
position and TCZ coordinate, while both OME formats store array axes as TCZYX.
`WriterSettings::acquisitionOrder` controls how implicit coordinates advance and
must contain T, C and Z exactly once. The rightmost axis varies fastest.

Set `timeCount` to a positive value for a bounded acquisition or zero for an
unbounded time series. Unbounded T must be the first acquisition axis. Frames may
also carry explicit coordinates. Forward coordinate gaps are written as zero
planes; duplicate and backward coordinates are rejected.

Supported pixel types are unsigned 8-bit and unsigned 16-bit. `significantBits`
describes the meaningful camera bits within that storage type.

Plain TIFF accepts tightly packed frames and stores `linkedMetadataFile` plus the
frame identity, pixel format and source ROI in `ImageDescription` JSON. Binary
preserves each submitted frame byte-for-byte, including row padding. Set
`frameMetadataPath` for its CSV index and provide `FrameMetadata::stride` on
each append.

`WriterSettings` describes the complete output and storage geometry.
`FrameMetadata` describes one appended frame. ScopeWriter does not accept Qt or
ScopeOne objects, and callers retain ownership of the input buffer after
`append` returns. Use one sequential producer per `Writer` instance.

The main format-specific settings are:

- `outputPath` for every format
- `frameMetadataPath` for the Binary CSV sidecar
- `linkedMetadataFile` for a plain TIFF external-metadata reference
- `zarrChunkWidth`, `zarrChunkHeight`, `zarrShardWidthChunks` and
  `zarrShardHeightChunks` for OME-Zarr layout
- `enableCompression` and `compressionLevel` for compressed formats
- `overwrite`, which is disabled by default

## Output layouts

A single-position OME-TIFF is written directly to the requested path. A
multi-position output such as `experiment.ome.tiff` becomes:

```text
experiment/
  experiment_p000.ome.tiff
  experiment_p001.ome.tiff
```

Every TIFF contains a complete redundant copy of the OME model. Each physical
file has its own UUID, and every `TiffData` block references the filename and
UUID that owns its pixels.

A single-position OME-Zarr stores the multiscales image at the root. A
multi-position OME-Zarr uses the OME-NGFF 0.5 bioformats2raw layout:

```text
experiment.ome.zarr/
  zarr.json
  OME/zarr.json
  Position A/0/zarr.json
  Position B/0/zarr.json
```

Position names become series path components. Grid positions append
`_row_column` to keep series names unique. OME-Zarr defaults to one shard per
TCZ plane with internal YX chunks up to 512 by 512 pixels. Configure
`zarrChunkWidth`, `zarrChunkHeight`, `zarrShardWidthChunks` and
`zarrShardHeightChunks` to split a plane into smaller shards. A zero shard chunk
count uses all chunks on that axis.

A plain TIFF is one BigTIFF file containing one directory per frame. Each
directory stores frame identity, timestamp, pixel format, significant bits and
source ROI in compact JSON under `ImageDescription`.

Binary output consists of two files:

```text
frames.bin
frames_frameinfo.csv
```

The `.bin` file concatenates submitted payloads exactly, including row padding.
The CSV records camera ID, frame index, timestamp, dimensions, significant
bits, stride, pixel format, payload size and source ROI for every frame.

## Frame metadata protocol v1

Plain TIFF and Binary use the stable protocol identifier
`scopewriter.frame-metadata.v1`. The exact Binary CSV header is the v1 format
signature and must not be reordered or partially interpreted.

| Field | Type | Meaning |
| --- | --- | --- |
| `camera_id` | UTF-8 string | Frame source within the acquisition |
| `frame_index` | unsigned integer | Source-assigned frame sequence number |
| `timestamp_ns` | unsigned integer | Absolute or acquisition-provided timestamp in nanoseconds |
| `width`, `height` | positive integer | Stored image dimensions in pixels |
| `bits_per_sample` | positive integer | Significant camera bits per sample |
| `stride` | positive integer | Binary source row stride in bytes |
| `pixel_format` | UTF-8 string | `Mono8` or `Mono16`, derived from `pixelType` |
| `pixel_format_id` | unsigned integer | `0` for `Mono8` or `1` for `Mono16` |
| `payload_bytes` | positive integer | Number of bytes appended to the Binary stream |
| `source_roi_x`, `source_roi_y` | integer | Source ROI origin in sensor pixels |
| `source_roi_width`, `source_roi_height` | integer | Source ROI size in sensor pixels |

Plain TIFF stores `schema`, `linked_metadata_file` and all applicable frame
fields in each page's JSON `ImageDescription`. Binary stores the frame fields
as one RFC 4180-style CSV row per payload; the header string exposed as
`kBinaryFrameMetadataHeader` identifies v1. `linked_metadata_file` is an opaque
relative or display path supplied by the caller. ScopeWriter does not open or
interpret the linked file.

## Metadata

OME-TIFF stores physical sizes, time increment, channel metadata, detector
metadata, stage positions, acquisition date, exposure and DeltaT in OME-XML.
OME-Zarr stores the corresponding axes, coordinate transformations and OMERO
channel display metadata in `zarr.json`. Generic acquisition metadata is
namespaced. Generic OME-Zarr frame metadata is written beside each image group
as `scopewriter.frames.jsonl`.

Existing output is rejected by default. Set `WriterSettings::overwrite` to true
only when replacing the complete destination is intended.

## Dependencies

ScopeWriter carries its C and C++ dependencies under `third_party` and builds
only the static components it uses. ScopeWriter contains its Zarr V3 filesystem
writer source and does not require acquire-zarr to be installed.

| Component | Use | License |
| --- | --- | --- |
| ScopeWriter original code | Public API and format coordination | BSD 3-Clause |
| libtiff 4.7.1 | TIFF and OME-TIFF encoding | libtiff license and bundled notices |
| zlib 1.3.1 | Deflate compression | zlib License |
| Zstandard 1.5.7 | Zarr chunk compression | BSD 3-Clause |
| CRC32C 1.1.2 | Zarr shard checksums | BSD 3-Clause |
| acquire-zarr 0.8.1 derived code | Local Zarr streaming primitives | Apache License 2.0 |

The complete declarations and Apache 2.0 text are in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Upstream license files are
retained in each dependency directory and installed under
`share/doc/ScopeWriter/licenses`. Distributions of ScopeWriter must include the
ScopeWriter license and applicable third-party notices.

## Build

Requirements are CMake 3.23 or newer, a C++20 compiler and a platform thread
implementation. No dependency download occurs during configuration.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

Set `SCOPEWRITER_BUILD_TESTS=OFF` to omit tests. It defaults to `ON` when
ScopeWriter is configured as the top-level project and `OFF` when included by a
parent project.

## Consume

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
settings.significantBits = 16;
settings.timeCount = 100;
settings.channelCount = 1;
settings.zCount = 1;
settings.acquisitionOrder = "TCZ";

scopewriter::Writer writer;
if (!writer.open(settings)) {
    throw std::runtime_error(writer.lastError());
}
if (!writer.append(frame.data(), frame.size() * sizeof(std::uint16_t))) {
    throw std::runtime_error(writer.lastError());
}
if (!writer.close()) {
    throw std::runtime_error(writer.lastError());
}
```

Check the return value of every operation and read `lastError()` on failure.
`close()` drains pending writes and finalizes metadata and is safe to call more
than once.

Binary additionally requires its sidecar path and frame stride:

```cpp
settings.format = scopewriter::Format::Binary;
settings.outputPath = "frames.bin";
settings.frameMetadataPath = "frames_frameinfo.csv";

scopewriter::FrameMetadata metadata;
metadata.cameraId = "Camera";
metadata.frameIndex = 0;
metadata.timestampNs = timestampNs;
metadata.stride = settings.width * sizeof(std::uint16_t);

if (!writer.open(settings)
    || !writer.append(frame.data(), frame.size() * sizeof(std::uint16_t), metadata)
    || !writer.close()) {
    throw std::runtime_error(writer.lastError());
}
```

## Error and output behavior

- Existing destinations are rejected unless `overwrite` is enabled.
- Empty TIFF and Binary outputs are removed when closed.
- `close()` drains queued writes and reports asynchronous failures.
- OME coordinate gaps are zero-filled; duplicate or backward coordinates are
  rejected.
- A failed operation returns `false` and updates `lastError()`.
- ScopeWriter writes datasets but does not provide a reader API.

## Current scope

ScopeWriter targets deterministic TCZP microscope acquisition on a local
filesystem. It does not currently provide plate or HCS layouts, pyramids,
cloud object stores, live read views, arbitrary dimensions, RGB pixels or
floating-point pixel types.

ScopeWriter is distributed under the BSD 3-Clause License.
Its Zarr V3 implementation contains code derived from acquire-zarr under the
Apache License 2.0. See `THIRD_PARTY_NOTICES.md` for attribution and
redistribution terms.
