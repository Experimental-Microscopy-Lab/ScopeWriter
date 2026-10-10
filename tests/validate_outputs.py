"""Validate ScopeWriter output with independent readers of the published formats.

Usage: python validate_outputs.py <directory written by ScopeWriterTests>

OME-TIFF is checked against the OME-XML schema with ome-types and read with
tifffile. OME-Zarr is checked against the OME-NGFF 0.5 models of
ome-zarr-models and read with zarr. The pixel type tests write the same frames
in every format, so the arrays read back by the independent readers must match
each other and the declared OME pixel type.
"""

import sys
from pathlib import Path

import numpy as np
import ome_types
import tifffile
import zarr
from ome_zarr_models.v05 import Image

OME_TYPES = {
    "int8": np.int8,
    "int16": np.int16,
    "int32": np.int32,
    "uint8": np.uint8,
    "uint16": np.uint16,
    "uint32": np.uint32,
    "float": np.float32,
    "double": np.float64,
}

failures = []
checked = 0


def fail(path, message):
    failures.append(f"{path}: {message}")


def check_ome_xml(path, xml):
    try:
        return ome_types.from_xml(xml, validate=True)
    except Exception as error:  # noqa: BLE001 - report every validator failure
        fail(path, f"OME-XML is invalid: {error}")
        return None


def check_pixels(path, pixels, shape_by_axis, dtype):
    expected = {
        "T": pixels.size_t,
        "C": pixels.size_c,
        "Z": pixels.size_z,
        "Y": pixels.size_y,
        "X": pixels.size_x,
    }
    for axis, size in expected.items():
        if shape_by_axis.get(axis, 1) != size:
            fail(path, f"axis {axis} is {shape_by_axis.get(axis, 1)}, OME-XML says {size}")
    declared = OME_TYPES.get(pixels.type.value)
    if declared is None or np.dtype(declared) != np.dtype(dtype):
        fail(path, f"stored as {np.dtype(dtype)}, OME-XML says {pixels.type.value}")


def validate_ome_tiff(path):
    arrays = []
    with tifffile.TiffFile(path) as tiff:
        if not tiff.is_ome:
            fail(path, "tifffile does not recognise the file as OME-TIFF")
            return arrays
        ome = check_ome_xml(path, tiff.ome_metadata)
        if ome is None:
            return arrays
        if len(tiff.series) != len(ome.images):
            fail(path, f"{len(tiff.series)} TIFF series for {len(ome.images)} OME images")
            return arrays
        for series, image in zip(tiff.series, ome.images):
            check_pixels(path, image.pixels, dict(zip(series.axes, series.shape)), series.dtype)
            arrays.append(series.asarray())
    return arrays


def validate_ome_zarr_image(path, group, pixels=None):
    try:
        image = Image.from_zarr(group)
    except Exception as error:  # noqa: BLE001
        fail(path, f"not valid OME-NGFF 0.5: {error}")
        return None
    multiscale = image.attributes.ome.multiscales[0]
    names = [axis.name for axis in multiscale.axes]
    for dataset in multiscale.datasets:
        array = group[dataset.path]
        if list(array.metadata.dimension_names or []) != names:
            fail(path, f"dimension_names {array.metadata.dimension_names} != axes {names}")
    data = group[multiscale.datasets[0].path][...]
    if pixels is not None:
        check_pixels(path, pixels,
                     {name.upper(): size for name, size in zip(names, data.shape)},
                     data.dtype)
    return data


def validate_ome_zarr(path):
    root = zarr.open_group(path, mode="r")
    ome = root.attrs.get("ome", {})
    arrays = []
    if "bioformats2raw.layout" in ome:
        xml = path / "OME" / "METADATA.ome.xml"
        if not xml.is_file():
            fail(path, "bioformats2raw layout has no OME/METADATA.ome.xml")
            return arrays
        document = check_ome_xml(path, xml.read_text(encoding="utf-8"))
        series = zarr.open_group(path / "OME", mode="r").attrs.get("ome", {}).get("series")
        if not series or document is None or len(series) != len(document.images):
            fail(path, "OME group series do not match the OME-XML images")
            return arrays
        for name, image in zip(series, document.images):
            data = validate_ome_zarr_image(
                path / name, zarr.open_group(path / name, mode="r"), image.pixels)
            if data is not None:
                arrays.append(data)
    else:
        data = validate_ome_zarr_image(path, root)
        if data is not None:
            arrays.append(data)
    return arrays


def check_pixel_type_outputs(root):
    """The frames of every pixel type must read back identically from all formats."""
    sizes = {"int8": np.int8, "uint8": np.uint8, "int16": np.int16, "uint16": np.uint16,
             "int32": np.int32, "uint32": np.uint32, "float32": np.float32,
             "float64": np.float64}
    for name, dtype in sizes.items():
        stem = root / f"types-{name}"
        raw = np.frombuffer(stem.with_suffix(".bin").read_bytes(), dtype=dtype)
        reference = raw.reshape(2, 3, 5)
        with tifffile.TiffFile(stem.with_suffix(".tif")) as tiff:
            plain = tiff.asarray()
        with tifffile.TiffFile(str(stem) + ".ome.tiff") as tiff:
            ome_tiff = tiff.asarray()
        zarr_data = zarr.open_group(str(stem) + ".ome.zarr", mode="r")["0"][...]
        candidates = {
            "TIFF": plain,
            "OME-TIFF": ome_tiff,
            "OME-Zarr": zarr_data.reshape(2, 3, 5),
        }
        for label, data in candidates.items():
            data = np.asarray(data).reshape(2, 3, 5)
            if data.dtype != np.dtype(dtype):
                fail(stem, f"{label} was read as {data.dtype}, expected {np.dtype(dtype)}")
            elif data.tobytes() != reference.tobytes():
                fail(stem, f"{label} pixels differ from the binary frames")


def mean_down(plane):
    """Average 2x2 blocks of the last two axes. Odd edges average what they have."""
    height, width = plane.shape[-2:]
    rows = np.arange(0, height, 2)
    columns = np.arange(0, width, 2)
    sums = np.add.reduceat(
        np.add.reduceat(plane.astype(np.float64), rows, axis=-2), columns, axis=-1)
    counts = (np.add.reduceat(np.ones(height), rows)[:, None]
              * np.add.reduceat(np.ones(width), columns)[None, :])
    mean = sums / counts
    if np.issubdtype(plane.dtype, np.integer):
        return np.floor(mean + 0.5).astype(plane.dtype)
    return mean.astype(plane.dtype)


def check_pyramids(root):
    """Every coarser level must be the 2x2 mean of the level before it."""
    global checked
    for path in sorted(root.glob("pyramid-*.ome.zarr")):
        checked += 1
        group = zarr.open_group(path, mode="r")
        multiscale = group.attrs["ome"]["multiscales"][0]
        datasets = multiscale["datasets"]
        if len(datasets) < 2:
            fail(path, "expected a multi-resolution image")
            continue
        levels = [group[dataset["path"]][...] for dataset in datasets]
        for index in range(1, len(levels)):
            expected = mean_down(levels[index - 1])
            actual = levels[index]
            if actual.shape != expected.shape:
                fail(path, f"level {index} has shape {actual.shape}, expected {expected.shape}")
                continue
            if np.issubdtype(actual.dtype, np.integer):
                same = np.array_equal(actual, expected)
            else:
                same = np.allclose(actual, expected, rtol=1e-6)
            if not same:
                fail(path, f"level {index} is not the mean of level {index - 1}")
            first = datasets[0]["coordinateTransformations"][0]["scale"]
            scale = datasets[index]["coordinateTransformations"][0]["scale"]
            factor = 2 ** index
            if scale[:3] != first[:3] or scale[3] != first[3] * factor \
                    or scale[4] != first[4] * factor:
                fail(path, f"level {index} scale {scale} is not {factor}x the first level")


def main():
    global checked
    root = Path(sys.argv[1])
    for path in sorted(root.rglob("*.ome.tiff")):
        checked += 1
        validate_ome_tiff(path)
    for path in sorted(root.rglob("*.ome.zarr")):
        if path.is_dir():
            checked += 1
            validate_ome_zarr(path)
    check_pixel_type_outputs(root)
    check_pyramids(root)
    for message in failures:
        print("FAIL", message)
    print(f"{checked} outputs checked, {len(failures)} problems")
    return 1 if failures or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
