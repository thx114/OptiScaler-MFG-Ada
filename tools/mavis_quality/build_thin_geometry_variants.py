#!/usr/bin/env python3
"""Build opt-in thin-geometry cubin variants from an installed DLSS-G provider.

No NVIDIA payload is stored in source control.  This tool reads the provider
already installed on the developer machine, applies narrowly scoped PTX edits,
assembles them for sm_89 and emits the generated header used by an experimental
local build.
"""

from __future__ import annotations

import argparse
import ctypes
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile


FATBIN_MAGIC = 0xBA55ED50
PTX_KIND = 1
CUBIN_KIND = 2
ADA_ARCH = 89
BLACKWELL_ARCH = 120


def pe_sections(path: Path) -> tuple[bytes, list[tuple[int, int, int, str]]]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: invalid PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + optional_size
    sections = []
    for index in range(count):
        offset = first + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, _, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((raw_offset, max(virtual_size, raw_size), index, name))
    return data, sections


def iter_fatbins(data: bytes, sections: list[tuple[int, int, int, str]]):
    for raw_offset, size, _, name in sections:
        if name != ".data":
            continue
        blob = data[raw_offset:raw_offset + size]
        magic = struct.pack("<I", FATBIN_MAGIC)
        cursor = 0
        while True:
            relative = blob.find(magic, cursor)
            if relative < 0:
                break
            start = raw_offset + relative
            if start + 16 <= len(data):
                header = struct.unpack_from("<H", data, start + 6)[0]
                payload = struct.unpack_from("<Q", data, start + 8)[0]
                end = start + 16 + payload
                if header == 16 and 0 < payload < 4 << 20 and end <= len(data):
                    yield start, end
            cursor = relative + 4


def iter_entries(data: bytes, start: int, end: int):
    cursor = start + 16
    while cursor + 64 <= end:
        kind = struct.unpack_from("<H", data, cursor)[0]
        header = struct.unpack_from("<I", data, cursor + 4)[0]
        payload = struct.unpack_from("<Q", data, cursor + 8)[0]
        compressed = struct.unpack_from("<I", data, cursor + 16)[0]
        arch = struct.unpack_from("<I", data, cursor + 28)[0]
        raw = struct.unpack_from("<Q", data, cursor + 56)[0]
        if not 64 <= header <= 256 or payload > end - cursor - header:
            return
        yield kind, arch, cursor + header, int(payload), compressed, int(raw)
        cursor += header + payload


def lz4_decompress(data: bytes, expected: int) -> bytes:
    output = bytearray()
    cursor = 0

    def extended_length(value: int) -> int:
        nonlocal cursor
        if value == 15:
            while True:
                if cursor >= len(data):
                    raise ValueError("truncated LZ4 length")
                extra = data[cursor]
                cursor += 1
                value += extra
                if extra != 255:
                    break
        return value

    while cursor < len(data):
        token = data[cursor]
        cursor += 1
        literals = extended_length(token >> 4)
        if cursor + literals > len(data):
            raise ValueError("truncated LZ4 literal run")
        output.extend(data[cursor:cursor + literals])
        cursor += literals
        if cursor == len(data):
            break
        if cursor + 2 > len(data):
            raise ValueError("truncated LZ4 back-reference")
        distance = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        count = extended_length(token & 15) + 4
        if distance == 0 or distance > len(output):
            raise ValueError("invalid LZ4 back-reference")
        for _ in range(count):
            output.append(output[-distance])
        if len(output) > expected:
            raise ValueError("LZ4 output exceeds declared size")
    if len(output) != expected:
        raise ValueError(f"LZ4 output is {len(output)} bytes, expected {expected}")
    return bytes(output)


def fingerprint_elf(blob: bytes) -> tuple[int, int, int]:
    if len(blob) < 0x40 or blob[:4] != b"\x7fELF":
        raise ValueError("not an ELF cubin")
    section_offset = struct.unpack_from("<Q", blob, 0x28)[0]
    section_size, section_count, string_index = struct.unpack_from("<HHH", blob, 0x3A)
    if section_size < 0x40 or string_index >= section_count:
        raise ValueError("invalid ELF section table")
    string_header = section_offset + string_index * section_size
    strings = struct.unpack_from("<Q", blob, string_header + 0x18)[0]
    text = shared = registers = 0
    for index in range(section_count):
        section = section_offset + index * section_size
        name_offset = struct.unpack_from("<I", blob, section)[0]
        end = blob.find(b"\0", strings + name_offset)
        name = blob[strings + name_offset:end]
        size = struct.unpack_from("<Q", blob, section + 0x20)[0]
        info = struct.unpack_from("<I", blob, section + 0x2C)[0]
        if name.startswith(b".text."):
            text = size
            registers = (info >> 24) & 0xFF
        elif name.startswith(b".nv.shared"):
            shared = size
    if not text:
        raise ValueError("cubin has no text section")
    return int(text), int(shared), int(registers)


def local_storage_bytes(blob: bytes) -> int:
    """Return statically allocated CUDA local storage (spill/stack) bytes."""
    if len(blob) < 0x40 or blob[:4] != b"\x7fELF":
        raise ValueError("not an ELF cubin")
    section_offset = struct.unpack_from("<Q", blob, 0x28)[0]
    section_size, section_count, string_index = struct.unpack_from(
        "<HHH", blob, 0x3A
    )
    if section_size < 0x40 or string_index >= section_count:
        raise ValueError("invalid ELF section table")
    string_header = section_offset + string_index * section_size
    strings = struct.unpack_from("<Q", blob, string_header + 0x18)[0]
    total = 0
    for index in range(section_count):
        section = section_offset + index * section_size
        name_offset = struct.unpack_from("<I", blob, section)[0]
        end = blob.find(b"\0", strings + name_offset)
        name = blob[strings + name_offset:end]
        if name.startswith(b".nv.local"):
            total += struct.unpack_from("<Q", blob, section + 0x20)[0]
    return int(total)


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"{label}: found {count} instances, expected exactly one")
    return source.replace(old, new, 1)


def patch_previous_scatter(source: str) -> str:
    anchor = "ld.param.f32 %f24, [%rd6+60];\n"
    return replace_once(
        source,
        anchor,
        anchor + "mul.ftz.f32 %f24, %f24, 0f3F000000; // thin-geometry previous scatter\n",
        "previous scatter divisor load",
    )


def patch_intermediate_scatter(source: str) -> str:
    anchor = "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n"
    return replace_once(
        source,
        anchor,
        anchor + "mul.ftz.f32 %f2, %f2, 0f3F000000; // thin-geometry intermediate scatter\n",
        "intermediate scatter divisor load",
    )


_SILHOUETTE_NEIGHBORS = {
    "curr_to_prev": {
        "anchor": "fma.rn.ftz.f32 %f14, %f7, %f7, %f157;\n",
        "center": ("%f7", "%f8", "%f9"),
        "neighbors": [
            ("%f55", "%f56", "%f57"),
            ("%f78", "%f79", "%f80"),
            ("%f97", "%f98", "%f99"),
            ("%f120", "%f121", "%f122"),
        ],
        "diagonals": {
            "same": (("%f41", "%f42", "%f43"),
                     ("%f131", "%f132", "%f133")),
            "opposite": (("%f109", "%f110", "%f111"),
                         ("%f66", "%f67", "%f68")),
        },
        "length": "%f14",
        "reload": "",
    },
    "prev_to_curr": {
        "anchor": "fma.rn.ftz.f32 %f22, %f15, %f15, %f942;\n",
        "center": ("%f15", "%f16", "%f17"),
        "neighbors": [
            ("%f840", "%f841", "%f842"),
            ("%f863", "%f864", "%f865"),
            ("%f882", "%f883", "%f884"),
            ("%f905", "%f906", "%f907"),
        ],
        "diagonals": {
            "same": (("%f826", "%f827", "%f828"),
                     ("%f916", "%f917", "%f918")),
            "opposite": (("%f894", "%f895", "%f896"),
                         ("%f851", "%f852", "%f853")),
        },
        "length": "%f22",
        "reload": (
            "ld.param.f32 %f2, "
            "[Kernel_EstimateIntermMvecsScatter_param_0+120];\n"
        ),
    },
}


def _silhouette_support_program(direction: str, aggressive: bool = False) -> str:
    """Condition only our extra retention on same-surface local support.

    The processed-depth threshold of three is already used by this provider's
    scatter family. Motion and depth are evaluated in the same shared tile and
    direction. A single cardinal same-depth, motion-coherent neighbor preserves
    thin two-pixel structures; no majority/background vote is introduced.
    """
    spec = _SILHOUETTE_NEIGHBORS[direction]
    center_x, center_y, center_depth = spec["center"]
    marker = "AGGRESSIVE_V1" if aggressive else "BALANCED_V1"
    depth_limit = "0f40000000" if aggressive else "0f40400000"
    lines = [
        f"// MFGUNLOCK_SILHOUETTE_BOUNDARY_GUARD_{marker}_{direction.upper()}",
        spec["reload"].rstrip("\n"),
        "mov.f32 %qgf0, 0f00000000;",
        f"div.approx.ftz.f32 %qgf2, {spec['length']}, %f2;",
        "max.ftz.f32 %qgf2, %qgf2, 0f3F800000;",
    ]
    for neighbor_x, neighbor_y, neighbor_depth in spec["neighbors"]:
        lines += [
            f"sub.ftz.f32 %qgf3, {neighbor_x}, {center_x};",
            f"sub.ftz.f32 %qgf4, {neighbor_y}, {center_y};",
            "mul.ftz.f32 %qgf5, %qgf4, %qgf4;",
            "fma.rn.ftz.f32 %qgf5, %qgf3, %qgf3, %qgf5;",
            "div.approx.ftz.f32 %qgf6, %qgf5, %qgf2;",
            "sub.ftz.f32 %qgf6, 0f3F800000, %qgf6;",
            "setp.gt.f32 %qgp0, %qgf6, 0f00000000;",
            f"sub.ftz.f32 %qgf7, {neighbor_depth}, {center_depth};",
            "abs.ftz.f32 %qgf7, %qgf7;",
            f"setp.lt.and.f32 %qgp0, %qgf7, {depth_limit}, %qgp0;",
            "@!%qgp0 mov.f32 %qgf6, 0f00000000;",
        ]
        if aggressive:
            # Support must exceed one full-neighbor equivalent. Squaring the
            # excess suppresses marginal boundaries, while summation avoids
            # introducing another register-heavy top-two sorting network.
            lines.append("add.f32 %qgf0, %qgf0, %qgf6;")
        else:
            lines += [
                "min.ftz.f32 %qgf6, %qgf6, 0f3F800000;",
                "max.f32 %qgf0, %qgf0, %qgf6;",
            ]
    if aggressive:
        lines += [
            "sub.f32 %qgf0, %qgf0, 0f3F800000;",
            "max.f32 %qgf0, %qgf0, 0f00000000;",
            "min.f32 %qgf0, %qgf0, 0f3F800000;",
            "mul.f32 %qgf0, %qgf0, %qgf0;",
            # At full confidence this permits only half as much additional
            # relaxation as Balanced: K_effective bottoms out at 0.75*K_native.
            "fma.rn.f32 %qgf11, %qgf0, 0fBE800000, 0f3F800000;",
            "mul.ftz.f32 %f2, %f2, %qgf11;",
        ]
    else:
        lines += [
            # K_effective stays within [0.5*K_native, K_native]. Unsupported
            # boundaries therefore return to the provider's native rejection;
            # the guard never makes that native path stricter.
            "fma.rn.f32 %qgf11, %qgf0, 0fBF000000, 0f3F800000;",
            "mul.ftz.f32 %f2, %f2, %qgf11;",
        ]
    return "\n".join(line for line in lines if line) + "\n"


def _patch_silhouette_boundary_guard(source: str, aggressive: bool) -> str:
    source = replace_once(
        source,
        ".reg .pred %p<656>;\n",
        ".reg .pred %p<656>;\n.reg .pred %qgp<2>;\n.reg .f32 %qgf<12>;\n",
        "silhouette guard register declaration",
    )
    for direction, spec in _SILHOUETTE_NEIGHBORS.items():
        source = replace_once(
            source,
            spec["anchor"],
            spec["anchor"] + _silhouette_support_program(direction, aggressive),
            f"{direction} silhouette guard insertion",
        )
    return source


def patch_silhouette_boundary_guard(source: str) -> str:
    return _patch_silhouette_boundary_guard(source, aggressive=False)


def patch_silhouette_boundary_guard_aggressive(source: str) -> str:
    return _patch_silhouette_boundary_guard(source, aggressive=True)


def patch_refined_geometry(source: str) -> str:
    # Keep best-neighbor support (no majority vote that erases thin geometry).
    # Only the last unit of the existing processed-depth window is tapered.
    source = patch_silhouette_boundary_guard(source)
    source = source.replace("max.ftz.f32 %qgf2, %qgf2, 0f3F800000;",
                            "max.ftz.f32 %qgf2, %qgf2, 0f3F800000;\n"
                            "rcp.approx.ftz.f32 %qgf2, %qgf2;")
    source = source.replace("div.approx.ftz.f32 %qgf6, %qgf5, %qgf2;",
                            "mul.ftz.f32 %qgf6, %qgf5, %qgf2;")
    source = source.replace("@!%qgp0 mov.f32 %qgf6, 0f00000000;",
                            "sub.sat.f32 %qgf7, 0f40400000, %qgf7;\n"
                            "min.f32 %qgf6, %qgf6, %qgf7;\n"
                            "@!%qgp0 mov.f32 %qgf6, 0f00000000;")
    # Saturated depth support already expresses the ordered depth window and
    # bounds the combined support to one. Remove the now redundant operations.
    source = source.replace("setp.lt.and.f32 %qgp0, %qgf7, 0f40400000, %qgp0;\n", "")
    source = source.replace("min.ftz.f32 %qgf6, %qgf6, 0f3F800000;\n", "")
    return source.replace("BALANCED_V1", "REFINED_V1")


def patch_geometry_confidence_v2(source: str) -> str:
    """Smooth only the final local-support confidence, not native rejection.

    Replacing the predicate/zero pair with a saturating subtraction is
    equivalent for finite nonnegative squared motion error and preserves
    NaN -> zero. It also makes room for one smoothstep per direction without
    adding neighbor reads or a multi-neighbor vote that loses thin geometry.
    """
    source = patch_refined_geometry(source)
    old = (
        "sub.ftz.f32 %qgf6, 0f3F800000, %qgf6;\n"
        "setp.gt.f32 %qgp0, %qgf6, 0f00000000;\n"
    )
    if source.count(old) != 8 or source.count("@!%qgp0 mov.f32 %qgf6, 0f00000000;\n") != 8:
        raise ValueError("geometry confidence V2: motion-support anchors changed")
    source = source.replace(old, "sub.ftz.sat.f32 %qgf6, 0f3F800000, %qgf6;\n")
    source = source.replace("@!%qgp0 mov.f32 %qgf6, 0f00000000;\n", "")
    old_final = (
        "fma.rn.f32 %qgf11, %qgf0, 0fBF000000, 0f3F800000;\n"
    )
    if source.count(old_final) != 2:
        raise ValueError("geometry confidence V2: final support anchors changed")
    smooth = (
        "// MFGUNLOCK_GEOMETRY_CONFIDENCE_V2\n"
        "fma.rn.f32 %qgf6, %qgf0, 0fC0000000, 0f40400000;\n"
        "mul.f32 %qgf0, %qgf0, %qgf0;\n"
        "mul.f32 %qgf0, %qgf0, %qgf6;\n"
    )
    return source.replace(old_final, smooth + old_final)


def patch_adaptive_geometry_v1(source: str) -> str:
    """Add foreground/background asymmetry without new reads or scatter taps.

    The provider normalizes both conventional and reversed depth so larger
    processed depth is nearer.  V2 used abs(neighbor-center), which treats the
    newly revealed background and the moving foreground identically.  This
    variant keeps full foreground-side support (center nearer than neighbor)
    while retaining the existing 2..3-unit taper when a nearer neighbor can
    occlude a background center.  Motion coherence and best-neighbor thin-
    geometry preservation remain mandatory.

    Scatter coverage itself is already signed and motion-adaptive in the
    provider (bounded X/Y extents and directional atomic writes).  Reusing it
    avoids extra taps, collisions and GPU work; this patch only changes whether
    our extra retention may accompany those native directional writes.
    """
    source = patch_geometry_confidence_v2(source)
    registers = ".reg .f32 %qgf<12>;\n"
    source = replace_once(
        source, registers,
        registers +
        "// MFGUNLOCK_ASYMMETRIC_DISOCCLUSION_V1\n"
        "// MFGUNLOCK_NATIVE_DIRECTIONAL_SCATTER_GATED_V1\n",
        "adaptive geometry marker",
    )
    symmetric = (
        "abs.ftz.f32 %qgf7, %qgf7;\n"
        "sub.sat.f32 %qgf7, 0f40400000, %qgf7;\n"
    )
    asymmetric = (
        "// Signed processed depth: positive means a nearer neighbor.\n"
        "sub.sat.f32 %qgf7, 0f40400000, %qgf7;\n"
    )
    if source.count(symmetric) != 8:
        raise ValueError("adaptive geometry: signed-depth anchors changed")
    return source.replace(symmetric, asymmetric)


def patch_adaptive_geometry_v2(source: str) -> str:
    """Blend best-neighbor and second-neighbor support without new taps.

    A single coherent neighbor retains the existing Aggressive relaxation;
    two coherent neighbors converge continuously to Balanced. This preserves
    one-pixel detail while making isolated support less likely to switch the
    full Balanced relaxation on and off between frames.
    """
    source = patch_adaptive_geometry_v1(source)
    source = replace_once(
        source,
        "// MFGUNLOCK_ASYMMETRIC_DISOCCLUSION_V1\n",
        "// MFGUNLOCK_ASYMMETRIC_DISOCCLUSION_V2\n"
        "// MFGUNLOCK_TWO_NEIGHBOR_CONFIDENCE_V2\n",
        "adaptive geometry V2 marker",
    )
    if source.count("mov.f32 %qgf0, 0f00000000;\n") != 2:
        raise ValueError("adaptive geometry V2: support initializers changed")
    source = source.replace(
        "mov.f32 %qgf0, 0f00000000;\n",
        "mov.f32 %qgf0, 0f00000000;\n"
        "mov.f32 %qgf1, 0f00000000;\n",
    )
    best_update = "max.f32 %qgf0, %qgf0, %qgf6;\n"
    if source.count(best_update) != 8:
        raise ValueError("adaptive geometry V2: best-support anchors changed")
    source = source.replace(
        best_update,
        "min.f32 %qgf8, %qgf0, %qgf6;\n"
        "max.f32 %qgf1, %qgf1, %qgf8;\n" + best_update,
    )
    final = "fma.rn.f32 %qgf11, %qgf0, 0fBF000000, 0f3F800000;\n"
    if source.count(final) != 2:
        raise ValueError("adaptive geometry V2: final relaxation anchors changed")
    consensus = (
        "fma.rn.f32 %qgf6, %qgf1, 0fC0000000, 0f40400000;\n"
        "mul.f32 %qgf1, %qgf1, %qgf1;\n"
        "mul.f32 %qgf1, %qgf1, %qgf6;\n"
        "fma.rn.f32 %qgf6, %qgf1, 0f3E800000, 0f3E800000;\n"
        "mul.f32 %qgf0, %qgf0, %qgf6;\n"
        "sub.f32 %qgf11, 0f3F800000, %qgf0;\n"
    )
    return source.replace(final, consensus)


def _diagonal_support_program(direction: str) -> str:
    spec = _SILHOUETTE_NEIGHBORS[direction]
    center_x, center_y, center_depth = spec["center"]
    same = spec["diagonals"]["same"]
    opposite = spec["diagonals"]["opposite"]
    done = f"MFGUNLOCK_GEOMETRY_DIAGONALS_DONE_{direction.upper()}_V3"
    lines = [
        f"// MFGUNLOCK_CONTINUOUS_DIAGONAL_SUPPORT_{direction.upper()}_V31",
        # Below half a pixel there is no stable direction from which to select
        # a diagonal pair. Cardinal weights are also exactly one there.
        f"@%qgp1 bra {done};",
        # Fade diagonal help out continuously as the second cardinal becomes
        # decisive: one through 0.35 and zero from 0.60.
        "sub.f32 %qgf3, %qgf1, 0f3EB33333;",
        "mul.sat.f32 %qgf3, %qgf3, 0f40800000;",
        "fma.rn.f32 %qgf4, %qgf3, 0fC0000000, 0f40400000;",
        "mul.f32 %qgf3, %qgf3, %qgf3;",
        "mul.f32 %qgf3, %qgf3, %qgf4;",
        "sub.f32 %qgf8, 0f3F800000, %qgf3;",
        # Fade the hard diagonal ratio gate over 0.30..0.60. qgf9/qgf10
        # retain the normalized absolute motion components.
        "max.f32 %qgf3, %qgf9, %qgf10;",
        "min.f32 %qgf4, %qgf9, %qgf10;",
        "max.f32 %qgf3, %qgf3, 0f358637BD;",
        "div.approx.f32 %qgf4, %qgf4, %qgf3;",
        "sub.f32 %qgf4, %qgf4, 0f3E99999A;",
        "mul.sat.f32 %qgf4, %qgf4, 0f40555555;",
        "fma.rn.f32 %qgf3, %qgf4, 0fC0000000, 0f40400000;",
        "mul.f32 %qgf4, %qgf4, %qgf4;",
        "mul.f32 %qgf4, %qgf4, %qgf3;",
        "mul.f32 %qgf8, %qgf8, %qgf4;",
        "mul.f32 %qgf8, %qgf8, %qgf11;",
        "setp.le.f32 %qgp0, %qgf8, 0f00000000;",
        f"@%qgp0 bra {done};",
        # Same-sign motion selects TL/BR; opposite-sign selects TR/BL.
        f"mul.f32 %qgf3, {center_x}, {center_y};",
        "setp.ge.f32 %qgp0, %qgf3, 0f00000000;",
    ]
    for index in range(2):
        same_x, same_y, same_depth = same[index]
        opposite_x, opposite_y, opposite_depth = opposite[index]
        lines += [
            f"selp.f32 %qgf3, {same_x}, {opposite_x}, %qgp0;",
            f"sub.ftz.f32 %qgf3, %qgf3, {center_x};",
            f"selp.f32 %qgf4, {same_y}, {opposite_y}, %qgp0;",
            f"sub.ftz.f32 %qgf4, %qgf4, {center_y};",
            "mul.ftz.f32 %qgf5, %qgf4, %qgf4;",
            "fma.rn.ftz.f32 %qgf5, %qgf3, %qgf3, %qgf5;",
            "mul.ftz.f32 %qgf6, %qgf5, %qgf2;",
            "sub.ftz.sat.f32 %qgf6, 0f3F800000, %qgf6;",
            f"selp.f32 %qgf7, {same_depth}, {opposite_depth}, %qgp0;",
            f"sub.ftz.f32 %qgf7, %qgf7, {center_depth};",
            "sub.sat.f32 %qgf7, 0f40400000, %qgf7;",
            "min.f32 %qgf6, %qgf6, %qgf7;",
        ]
        if index == 0:
            lines.append("mov.f32 %qgf11, %qgf6;")
        else:
            lines += [
                # Both opposite sides of the selected diagonal must agree.
                "min.f32 %qgf6, %qgf6, %qgf11;",
                "mul.f32 %qgf6, %qgf6, %qgf8;",
                "min.f32 %qgf7, %qgf0, %qgf6;",
                "max.f32 %qgf1, %qgf1, %qgf7;",
                "max.f32 %qgf0, %qgf0, %qgf6;",
            ]
    lines.append(f"{done}:")
    return "\n".join(lines) + "\n"


def _temporal_history_program(direction: str) -> str:
    """Conservatively stabilize addon support for one motion direction.

    The host validates phase and dimensions and publishes a byte-addressed
    plane through the module-local control symbol. A missing/disabled control
    is an exact local-only fast path. History stores confidence, never color,
    depth, or motion vectors.
    """
    direction_index = 0 if direction == "curr_to_prev" else 1
    suffix = direction.upper()
    done = f"MFGUNLOCK_TEMPORAL_HISTORY_DONE_{suffix}_V31"
    first = f"MFGUNLOCK_TEMPORAL_HISTORY_FIRST_{suffix}_V31"
    lines = [
        f"// MFGUNLOCK_TEMPORAL_GEOMETRY_{suffix}_V31",
        "ld.global.u32 %qgr0, [mfgunlock_v31_history_control+8];",
        "setp.eq.u32 %qgp0, %qgr0, 1;",
        f"@!%qgp0 bra {done};",
        "ld.global.u64 %qgrd0, [mfgunlock_v31_history_control];",
        "setp.ne.u64 %qgp0, %qgrd0, 0;",
        f"@!%qgp0 bra {done};",
        "ld.global.v4.u32 {%qgr1, %qgr2, %qgr3, %qgr5}, [mfgunlock_v31_history_control+16];",
        "setp.eq.u32 %qgp0, %qgr1, %r78;",
        "setp.eq.u32 %qgp1, %qgr2, %r79;",
        "and.pred %qgp0, %qgp0, %qgp1;",
        f"@!%qgp0 bra {done};",
        # Derive the phase bucket from the provider's verified interpolation
        # parameter. The host publishes N only after observing every expected
        # phase; no launch counter or motion-derived estimate is used.
        "cvt.rn.f32.u32 %qgf3, %qgr3;",
        "mul.f32 %qgf3, %f1, %qgf3;",
        "cvt.rni.u32.f32 %qgr4, %qgf3;",
        "sub.u32 %qgr2, %qgr3, %qgr4;",
        "setp.gt.u32 %qgp1, %qgr4, %qgr2;",
        "min.u32 %qgr3, %qgr4, %qgr2;",
        "add.u32 %qgr3, %qgr3, -1;",
        "selp.u32 %qgr4, 1, 0, %qgp1;",
        f"xor.b32 %qgr4, %qgr4, {direction_index};",
        "mad.lo.u32 %qgr3, %qgr3, 2, %qgr4;",
        "mul.lo.u32 %qgr3, %qgr3, %qgr5;",
        "mad.lo.u32 %qgr5, %r99, %qgr1, %r98;",
        "add.u32 %qgr3, %qgr3, %qgr5;",
        "cvt.u64.u32 %qgrd1, %qgr3;",
        "add.u64 %qgrd1, %qgrd0, %qgrd1;",
        "ld.global.u8 %qgr0, [%qgrd1];",
        "mul.f32 %qgf3, %qgf0, 0f40000000;",
        "setp.eq.u32 %qgp0, %qgr0, 255;",
        f"@%qgp0 bra {first};",
        "cvt.rn.f32.u32 %qgf4, %qgr0;",
        "mul.f32 %qgf4, %qgf4, 0f3B810204;",
        # A symmetric phase visits the same byte with directions exchanged.
        # Permit recovery only on k <= N-k; the paired second visit may lower
        # confidence immediately but cannot add another 0.20 in this source
        # frame. This remains correct for ascending or descending phase order.
        "@%qgp1 min.f32 %qgf3, %qgf3, %qgf4;",
        "@!%qgp1 add.f32 %qgf4, %qgf4, 0f3E4CCCCD;",
        "@!%qgp1 min.f32 %qgf3, %qgf3, %qgf4;",
        f"{first}:",
        "mul.f32 %qgf0, %qgf3, 0f3F000000;",
        "mul.f32 %qgf3, %qgf3, 0f437E0000;",
        "cvt.rni.u32.f32 %qgr0, %qgf3;",
        "min.u32 %qgr0, %qgr0, 254;",
        "st.global.u8 [%qgrd1], %qgr0;",
        f"{done}:",
    ]
    return "\n".join(lines) + "\n"


def patch_adaptive_geometry_v3(source: str, temporal: bool = True) -> str:
    """Orient cardinal support and add a gated pair of existing diagonals.

    The provider has already loaded the complete 3x3 tile into registers. This
    patch only consumes those values and never introduces another load.
    """
    source = patch_adaptive_geometry_v2(source)
    if temporal:
        source = replace_once(
            source,
            ".address_size 64\n",
            ".address_size 64\n\n"
            ".visible .global .align 4 .u32 mfgunlock_v31_history_magic = "
            "0x56333148;\n"
            ".visible .global .align 16 .b8 mfgunlock_v31_history_control[32];\n",
            "adaptive geometry V3.1 temporal globals",
        )
        source = replace_once(
            source,
            ".reg .f32 %qgf<12>;\n",
            ".reg .f32 %qgf<12>;\n"
            ".reg .b32 %qgr<6>;\n"
            ".reg .b64 %qgrd<2>;\n",
            "adaptive geometry V3.1 temporal registers",
        )
    source = replace_once(
        source,
        "// MFGUNLOCK_TWO_NEIGHBOR_CONFIDENCE_V2\n",
        "// MFGUNLOCK_TWO_NEIGHBOR_CONFIDENCE_V2\n"
        "// MFGUNLOCK_ORIENTED_GEOMETRY_V3\n",
        "adaptive geometry V3 marker",
    )
    normalization_anchor = "rcp.approx.ftz.f32 %qgf2, %qgf2;\n"
    normalization = (
        normalization_anchor
        + "sqrt.approx.ftz.f32 %qgf8, {length};\n"
        + "setp.le.f32 %qgp1, %qgf8, 0f3F000000;\n"
        + "max.f32 %qgf3, %qgf8, 0f358637BD;\n"
        + "rcp.approx.ftz.f32 %qgf3, %qgf3;\n"
        + "abs.f32 %qgf9, {center_x};\n"
        + "mul.f32 %qgf9, %qgf9, %qgf3;\n"
        + "abs.f32 %qgf10, {center_y};\n"
        + "mul.f32 %qgf10, %qgf10, %qgf3;\n"
        + "sub.f32 %qgf11, %qgf8, 0f3F000000;\n"
        + "mul.sat.f32 %qgf11, %qgf11, 0f3F800000;\n"
        + "fma.rn.f32 %qgf3, %qgf11, 0fC0000000, 0f40400000;\n"
        + "mul.f32 %qgf11, %qgf11, %qgf11;\n"
        + "mul.f32 %qgf11, %qgf11, %qgf3;\n"
    )
    offset = 0
    for direction in ("curr_to_prev", "prev_to_curr"):
        index = source.find(normalization_anchor, offset)
        if index < 0:
            raise ValueError(f"adaptive geometry V3: {direction} normalization missing")
        spec = _SILHOUETTE_NEIGHBORS[direction]
        replacement = normalization.format(
            length=spec["length"], center_x=spec["center"][0],
            center_y=spec["center"][1]
        )
        source = source[:index] + replacement + source[index + len(normalization_anchor):]
        offset = index + len(replacement)
    if source.find(normalization_anchor, offset) >= 0:
        raise ValueError("adaptive geometry V3: unexpected normalization anchor")

    update = (
        "min.f32 %qgf6, %qgf6, %qgf7;\n"
        "min.f32 %qgf8, %qgf0, %qgf6;\n"
        "max.f32 %qgf1, %qgf1, %qgf8;\n"
        "max.f32 %qgf0, %qgf0, %qgf6;\n"
    )
    components = ("%qgf9", "%qgf10", "%qgf10", "%qgf9") * 2
    offset = 0
    for component in components:
        index = source.find(update, offset)
        if index < 0:
            raise ValueError("adaptive geometry V3: cardinal update missing")
        weighted = (
            "min.f32 %qgf6, %qgf6, %qgf7;\n"
            f"sub.f32 %qgf8, 0f3F800000, {component};\n"
            "mul.f32 %qgf8, %qgf8, %qgf11;\n"
            "fma.rn.f32 %qgf8, %qgf8, 0fBE800000, 0f3F800000;\n"
            "mul.f32 %qgf6, %qgf6, %qgf8;\n"
            "min.f32 %qgf8, %qgf0, %qgf6;\n"
            "max.f32 %qgf1, %qgf1, %qgf8;\n"
            "max.f32 %qgf0, %qgf0, %qgf6;\n"
        )
        source = source[:index] + weighted + source[index + len(update):]
        offset = index + len(weighted)
    if source.find(update, offset) >= 0:
        raise ValueError("adaptive geometry V3: unexpected cardinal update")

    confidence = "// MFGUNLOCK_GEOMETRY_CONFIDENCE_V2\n"
    offset = 0
    for direction in ("curr_to_prev", "prev_to_curr"):
        index = source.find(confidence, offset)
        if index < 0:
            raise ValueError(f"adaptive geometry V3: {direction} confidence missing")
        diagonal = _diagonal_support_program(direction)
        source = source[:index] + diagonal + source[index:]
        offset = index + len(diagonal) + len(confidence)
    # Isolated evidence retains only one eighth of the native divisor
    # relaxation; two coherent supports still reach the V2 half relaxation.
    consensus = (
        "fma.rn.f32 %qgf6, %qgf1, 0f3E800000, 0f3E800000;\n"
    )
    stable_consensus = (
        "fma.rn.f32 %qgf6, %qgf1, 0f3EC00000, 0f3E000000;\n"
    )
    if source.count(consensus) != 2:
        raise ValueError("adaptive geometry V3.1: consensus anchors changed")
    source = source.replace(consensus, stable_consensus)
    if temporal:
        history_anchor = stable_consensus + "mul.f32 %qgf0, %qgf0, %qgf6;\n"
        offset = 0
        for direction in ("curr_to_prev", "prev_to_curr"):
            index = source.find(history_anchor, offset)
            if index < 0:
                raise ValueError(
                    f"adaptive geometry V3.1: {direction} history anchor missing")
            insertion = history_anchor + _temporal_history_program(direction)
            source = (source[:index] + insertion +
                      source[index + len(history_anchor):])
            offset = index + len(insertion)
        if source.find(history_anchor, offset) >= 0:
            raise ValueError("adaptive geometry V3.1: unexpected history anchor")
    return source


def patch_adaptive_geometry_v31_local(source: str) -> str:
    return patch_adaptive_geometry_v3(source, temporal=False)


def patch_adaptive_geometry_v31_temporal(source: str) -> str:
    return patch_adaptive_geometry_v3(source, temporal=True)


def patch_adaptive_inpaint_decision_v1(source: str) -> str:
    """Fail closed when the provider's inpaint-need mask is unordered.

    For every finite mask value this is bit-for-bit the same decision as the
    Blackwell source.  NaN previously compared false and was therefore treated
    as a valid pixel; the unordered comparison sends it to the provider's
    existing inpaint neighborhood instead. No radius, buffer or output changes.
    """
    anchors = (
        "setp.gt.ftz.f32 %p6, %f39, 0f00000000;\n",
        "setp.gt.ftz.f32 %p13, %f43, 0f00000000;\n",
    )
    for index, anchor in enumerate(anchors):
        source = replace_once(
            source, anchor,
            ("// MFGUNLOCK_INPAINT_DECISION_NONFINITE_V1\n" if index == 0 else "") +
            anchor.replace("setp.gt.", "setp.gtu."),
            f"inpaint decision mask {index}",
        )
    return source


def patch_adaptive_inpaint_decision_v2(source: str) -> str:
    """Preserve every finite V1 decision and fail closed for all non-finites."""
    anchors = (
        ("%p6", "%p5", "%f39", "%f40"),
        ("%p13", "%p12", "%f43", "%f44"),
    )
    for index, (result, scratch_pred, value, scratch_value) in enumerate(anchors):
        anchor = f"setp.gt.ftz.f32 {result}, {value}, 0f00000000;\n"
        program = (
            ("// MFGUNLOCK_INPAINT_DECISION_NONFINITE_V2\n" if index == 0 else "")
            + f"abs.f32 {scratch_value}, {value};\n"
            + f"setp.geu.f32 {scratch_pred}, {scratch_value}, 0f7F800000;\n"
            + anchor
            + f"or.pred {result}, {result}, {scratch_pred};\n"
        )
        source = replace_once(source, anchor, program,
                              f"inpaint V2 decision mask {index}")
    return source


def patch_adaptive_inpaint_decision_v3_local(source: str) -> str:
    """Tag hard rejects while preserving the V2 output decision bit-exactly.

    Bit zero remains the provider's needs-inpainting mask. Bit one is private
    metadata consumed only by the temporal variant after the 3x3 reduction.
    Every existing downstream test is non-zero based, so Local V3 produces the
    same decision as V2 and introduces no global or texture access.
    """
    source = replace_once(
        source, ".reg .pred %p<84>;\n",
        ".reg .pred %p<84>;\n.reg .pred %qip<2>;\n",
        "inpaint V3 local predicate declaration")
    for index, (result, invalid, value, scratch) in enumerate((
            ("%rs139", "%p5", "%f39", "%f40"),
            ("%rs140", "%p12", "%f43", "%f44"))):
        source = replace_once(
            source, f"mov.u16 {result}, 1;\n",
            f"mov.u16 {result}, 3;\n",
            f"inpaint V3 hard coordinate tag {index}")
        anchor = (
            f"setp.gt.ftz.f32 {'%p6' if index == 0 else '%p13'}, "
            f"{value}, 0f00000000;\n"
            f"selp.u16 {result}, 1, 0, "
            f"{'%p6' if index == 0 else '%p13'};\n"
        )
        native_pred = "%p6" if index == 0 else "%p13"
        program = (
            ("// MFGUNLOCK_INPAINT_LOCAL_V3\n" if index == 0 else "")
            + f"abs.f32 {scratch}, {value};\n"
            + f"setp.geu.f32 %qip0, {scratch}, 0f7F800000;\n"
            + f"setp.gt.ftz.f32 {native_pred}, {value}, 0f00000000;\n"
            + f"selp.u16 {result}, 1, 0, {native_pred};\n"
            + f"@%qip0 mov.u16 {result}, 3;\n"
        )
        source = replace_once(source, anchor, program,
                              f"inpaint V3 local decision {index}")
    return source


def _inpaint_temporal_program() -> str:
    hard_sources = (
        "%rs101", "%rs111", "%rs131", "%rs98", "%rs134",
        "%rs128", "%rs90", "%rs75", "%rs138")
    hard_reduce = [f"mov.u16 %qis0, {hard_sources[0]};"]
    hard_reduce += [f"or.b16 %qis0, %qis0, {item};"
                    for item in hard_sources[1:]]
    return "\n".join([
        "// MFGUNLOCK_INPAINT_TEMPORAL_V34",
        *hard_reduce,
        "and.b16 %qis0, %qis0, 2;",
        "setp.ne.u16 %qip0, %qis0, 0;",
        "ld.global.u32 %qir0, [mfgunlock_v34_inpaint_control+8];",
        "setp.eq.u32 %qip1, %qir0, 1;",
        "@!%qip1 bra MFGUNLOCK_INPAINT_HISTORY_DONE_V34;",
        "ld.global.u64 %qird0, [mfgunlock_v34_inpaint_control];",
        "setp.ne.u64 %qip1, %qird0, 0;",
        "@!%qip1 bra MFGUNLOCK_INPAINT_HISTORY_DONE_V34;",
        "ld.global.v4.u32 {%qir1,%qir2,%qir3,%qir4}, [mfgunlock_v34_inpaint_control+16];",
        "setp.eq.u32 %qip1, %qir1, %r57;",
        "setp.eq.u32 %qip2, %qir2, %r58;",
        "and.pred %qip1, %qip1, %qip2;",
        "@!%qip1 bra MFGUNLOCK_INPAINT_HISTORY_DONE_V34;",
        "ld.global.v4.u32 {%qir5,%qir6,%qir7,%qir8}, [mfgunlock_v34_inpaint_control+32];",
        "mad.lo.u32 %qir9, %qir5, 2, %qir6;",
        "mul.lo.u32 %qir9, %qir9, %qir4;",
        "mad.lo.u32 %qir10, %r56, %qir1, %r13;",
        "add.u32 %qir9, %qir9, %qir10;",
        "cvt.u64.u32 %qird1, %qir9;",
        "add.u64 %qird1, %qird0, %qird1;",
        "ld.global.u8 %qir0, [%qird1];",
        "selp.u32 %qir1, 62, 0, %p33;",
        "@%qip0 bra MFGUNLOCK_INPAINT_HISTORY_HARD_V34;",
        "setp.eq.u32 %qip1, %qir0, 255;",
        "@%qip1 bra MFGUNLOCK_INPAINT_HISTORY_FIRST_V34;",
        "and.b32 %qir2, %qir0, 63;",
        "shr.u32 %qir3, %qir0, 6;",
        "mov.u32 %qir4, %qir1;",
        "mov.u32 %qir5, %qir3;",
        "setp.gt.u32 %qip1, %qir1, %qir2;",
        "@%qip1 add.u32 %qir4, %qir2, 12;",
        "@%qip1 min.u32 %qir4, %qir4, %qir1;",
        "setp.lt.u32 %qip2, %qir1, %qir2;",
        "setp.eq.u32 %qip3, %qir3, 1;",
        "and.pred %qip3, %qip2, %qip3;",
        "@%qip3 add.u32 %qir4, %qir1, 12;",
        "@%qip3 min.u32 %qir4, %qir4, %qir2;",
        "@%qip3 mov.u32 %qir5, 2;",
        "sub.s32 %qir6, %qir1, %qir2;",
        "abs.s32 %qir6, %qir6;",
        "setp.ge.u32 %qip1, %qir1, 47;",
        "setp.le.u32 %qip2, %qir6, 4;",
        "and.pred %qip1, %qip1, %qip2;",
        "@!%qip1 bra MFGUNLOCK_INPAINT_HISTORY_NOT_STABLE_V34;",
        "setp.eq.u32 %qip2, %qir3, 3;",
        "setp.eq.u32 %qip3, %qir3, 1;",
        "or.pred %qip2, %qip2, %qip3;",
        "selp.u32 %qir5, 1, 3, %qip2;",
        "bra MFGUNLOCK_INPAINT_HISTORY_PACK_V34;",
        "MFGUNLOCK_INPAINT_HISTORY_NOT_STABLE_V34:",
        "setp.eq.u32 %qip2, %qir5, 3;",
        "@%qip2 mov.u32 %qir5, 0;",
        "bra MFGUNLOCK_INPAINT_HISTORY_PACK_V34;",
        "MFGUNLOCK_INPAINT_HISTORY_FIRST_V34:",
        "setp.ge.u32 %qip1, %qir1, 47;",
        "selp.u32 %qir5, 3, 0, %qip1;",
        "mov.u32 %qir4, %qir1;",
        "bra MFGUNLOCK_INPAINT_HISTORY_PACK_V34;",
        "MFGUNLOCK_INPAINT_HISTORY_HARD_V34:",
        "mov.u32 %qir4, 0;",
        "mov.u32 %qir5, 0;",
        "MFGUNLOCK_INPAINT_HISTORY_PACK_V34:",
        "shl.b32 %qir5, %qir5, 6;",
        "or.b32 %qir5, %qir5, %qir4;",
        "st.global.u8 [%qird1], %qir5;",
        "setp.ne.u32 %qip1, %qir4, 0;",
        "or.pred %p33, %p33, %qip1;",
        "MFGUNLOCK_INPAINT_HISTORY_DONE_V34:",
    ]) + "\n"


def patch_adaptive_inpaint_decision_v3_temporal(source: str) -> str:
    source = patch_adaptive_inpaint_decision_v3_local(source)
    source = replace_once(
        source, ".address_size 64\n",
        ".address_size 64\n\n"
        ".visible .global .align 4 .u32 mfgunlock_v34_inpaint_magic = "
        "0x56333449;\n"
        ".visible .global .align 16 .b8 mfgunlock_v34_inpaint_control[48];\n",
        "inpaint V3.4 temporal globals")
    source = replace_once(
        source, ".reg .pred %qip<2>;\n",
        ".reg .pred %qip<4>;\n"
        ".reg .b16 %qis<1>;\n"
        ".reg .b32 %qir<11>;\n"
        ".reg .b64 %qird<2>;\n",
        "inpaint V3.4 temporal registers")
    anchor = "@%p33 bra $L__BB0_9;\n"
    return replace_once(source, anchor,
                        _inpaint_temporal_program() + anchor,
                        "inpaint V3.4 temporal decision")


def patch_validated_warp_blend(source: str) -> str:
    """Apply an independently authored conservative warp-validation experiment.

    Tony Joaca/DLSSG-Transfusion identified BlendCandidatesFused as a useful quality
    point.  This variant keeps that mechanism separate from scatter retention,
    retains the provider's bounds/sentinel/finite checks and uses a gradual
    0.85..1.0 confidence floor instead of forcing every accepted warp to 1.0.
    """
    declarations = ".reg .pred %p<260>;\n"
    extra_declarations = (
        declarations
        + ".reg .pred %qv<7>;\n"
        + ".reg .f32 %qf<12>;\n"
    )
    source = replace_once(source, declarations, extra_declarations, "blend register declaration")

    anchor = "ld.param.u8 %rs8, [%rd6+220];\n"
    program = """// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
"""
    return replace_once(source, anchor, program + anchor, "blend UIR insertion point")


def patch_refined_blend(source: str) -> str:
    header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis/quality_refinement.hpp"
    fragment = header.read_text(encoding="utf-8").split('R"PTX(', 1)[1].split(')PTX"', 1)[0]
    source = patch_validated_warp_blend(source)
    anchor = "min.f32 %qf1, %qf1, 0f3F800000;\n"
    return replace_once(source, anchor, anchor + fragment, "smooth confidence weights")


def patch_refined_border_blend(source: str) -> str:
    smooth_header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis/quality_refinement.hpp"
    smooth = smooth_header.read_text(encoding="utf-8").split('R"PTX(', 1)[1].split(')PTX"', 1)[0]
    border_header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis/quality_border.hpp"
    border = border_header.read_text(encoding="utf-8").split('R"PTX(', 1)[1].split(')PTX"', 1)[0]
    source = patch_refined_blend(source)
    return replace_once(source, smooth, smooth + border, "symmetric border confidence")


def patch_adaptive_blend(source: str) -> str:
    source = patch_refined_border_blend(source)
    header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis/adaptive_quality.hpp"
    arbitration = header.read_text(encoding="utf-8").split('R"PTX(', 1)[1].split(')PTX"', 1)[0]
    border_header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis/quality_border.hpp"
    border = border_header.read_text(encoding="utf-8").split('R"PTX(', 1)[1].split(')PTX"', 1)[0]
    return replace_once(source, border, border + arbitration,
                        "candidate arbitration")


def _fragment(name: str, symbol_index: int = 0) -> str:
    header = Path(__file__).resolve().parents[2] / "OptiScaler/framegen/dlssg/mavis" / name
    parts = header.read_text(encoding="utf-8").split('R"PTX(')[1:]
    return parts[symbol_index].split(')PTX"', 1)[0]


def patch_adaptive_blend_v2(source: str) -> str:
    source = patch_validated_warp_blend(source)
    semantic_gates = (
        "add.f32 %qf10, %qf9, 0f3DA3D70A;\n"
        "setp.lt.f32 %qv4, %qf10, %qf6;\n"
        "setp.lt.f32 %qv2, %qf9, 0f3E19999A;\n"
        "and.pred %qv4, %qv4, %qv2;\n"
        "and.pred %qv4, %qv4, %qv3;\n"
        "setp.gt.f32 %qv2, %qf6, 0f3E800000;\n"
        "setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;\n"
        "and.pred %qv5, %qv5, %qv2;\n"
        "or.pred %qv5, %qv5, %qv4;\n"
        "and.pred %qv0, %qv0, %qv5;\n"
        "setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;\n"
        "and.pred %qv6, %qv6, %qv2;\n"
        "or.pred %qv6, %qv6, %qv4;\n"
        "and.pred %qv1, %qv1, %qv6;\n"
    )
    source = replace_once(source, semantic_gates,
                          "// MFGUNLOCK_SOFT_ELIGIBILITY_V2\n",
                          "soft warp eligibility V2")
    source = replace_once(source, "// MFGUNLOCK_VALIDATED_WARP_BLEND_V1\n",
                          "// MFGUNLOCK_VALIDATED_WARP_BLEND_V2\n",
                          "validated warp V2 marker")
    smooth, border, arbitration = (
        _fragment("adaptive_quality_v2.hpp", index) for index in range(3)
    )
    anchor = "min.f32 %qf1, %qf1, 0f3F800000;\n"
    source = replace_once(source, anchor, anchor + smooth,
                          "smooth warp confidence V2")
    source = replace_once(source, smooth, smooth + border,
                          "border confidence V2")
    return replace_once(source, border, border + arbitration,
                        "candidate arbitration V2")


def patch_adaptive_blend_v3(source: str) -> str:
    source = patch_adaptive_blend_v2(source)
    relative, smooth_v3, border_distances_v3, border_v3, arbitration_v3 = (
        _fragment("adaptive_quality_v3.hpp", index) for index in range(5)
    )
    absolute = (
        "sub.f32 %qf6, %f115, %f119;\n"
        "sub.f32 %qf7, %f116, %f120;\n"
        "sub.f32 %qf8, %f117, %f121;\n"
        "abs.f32 %qf6, %qf6;\n"
        "abs.f32 %qf7, %qf7;\n"
        "abs.f32 %qf8, %qf8;\n"
        "add.f32 %qf6, %qf6, %qf7;\n"
        "add.f32 %qf6, %qf6, %qf8;\n"
        "sub.f32 %qf9, %f125, %f131;\n"
        "sub.f32 %qf10, %f126, %f132;\n"
        "sub.f32 %qf11, %f127, %f133;\n"
        "abs.f32 %qf9, %qf9;\n"
        "abs.f32 %qf10, %qf10;\n"
        "abs.f32 %qf11, %qf11;\n"
        "add.f32 %qf9, %qf9, %qf10;\n"
        "add.f32 %qf9, %qf9, %qf11;\n"
    )
    smooth_v2, border_v2, arbitration_v2 = (
        _fragment("adaptive_quality_v2.hpp", index) for index in range(3)
    )
    source = replace_once(source, absolute, border_distances_v3 + relative,
                          "relative photometric error V3")
    source = replace_once(source, smooth_v2, smooth_v3,
                          "smooth confidence V3")
    source = replace_once(source, border_v2, border_v3,
                          "directional border V3")
    source = replace_once(source, arbitration_v2, arbitration_v3,
                          "candidate arbitration V3")
    source = replace_once(source, ".reg .f32 %qf<12>;\n",
                          ".reg .f32 %qf<8>;\n",
                          "quality V3 register declaration")
    return replace_once(source, "MFGUNLOCK_VALIDATED_WARP_BLEND_V2",
                        "MFGUNLOCK_VALIDATED_WARP_BLEND_V3",
                        "validated warp V3 marker")


def patch_adaptive_blend_v3_photometric_only(source: str) -> str:
    """Diagnostic/default-profile A/B: V3 photometry with the V2 border."""
    source = patch_adaptive_blend_v3(source)
    border_v2 = _fragment("adaptive_quality_v2.hpp", 1)
    border_distances_v3 = _fragment("adaptive_quality_v3.hpp", 2)
    border_v3 = _fragment("adaptive_quality_v3.hpp", 3)
    source = replace_once(source, border_distances_v3, "",
                          "V3 photometric-only border distances")
    return replace_once(source, border_v3, border_v2,
                        "V3 photometric-only border confidence")


def patch_adaptive_blend_v3_directional_only(source: str) -> str:
    """Diagnostic/default-profile A/B: V2 photometry with the V3 border."""
    source = patch_adaptive_blend_v2(source)
    border_v2 = _fragment("adaptive_quality_v2.hpp", 1)
    border_distances_v3 = _fragment("adaptive_quality_v3.hpp", 2)
    border_v3 = _fragment("adaptive_quality_v3.hpp", 3)
    smooth_v2 = _fragment("adaptive_quality_v2.hpp", 0)
    anchors = (
        "// MFGUNLOCK_V2_NATIVE_ANCHORS_FOR_DIRECTIONAL_BORDER_V3\n"
        "add.sat.f32 %f174, %f148, 0f00000000;\n"
        "add.sat.f32 %f175, %f149, 0f00000000;\n"
    )
    absolute_anchor = "sub.f32 %qf6, %f115, %f119;\n"
    source = replace_once(source, absolute_anchor,
                          border_distances_v3 + absolute_anchor,
                          "V3 directional-only border distances")
    source = replace_once(source, smooth_v2, anchors + smooth_v2,
                          "V3 directional-only native anchors")
    return replace_once(source, border_v2, border_v3,
                        "V3 directional-only border confidence")


PATCHERS = {
    "Kernel_EstimatePrev2CurrScatter": ("previous_scatter", patch_previous_scatter, ADA_ARCH),
    "Kernel_EstimateIntermMvecsScatter": ("intermediate_scatter", patch_intermediate_scatter, BLACKWELL_ARCH),
    "Kernel_BlendCandidatesFused": ("validated_warp_blend", patch_validated_warp_blend, BLACKWELL_ARCH),
    "Kernel_OutputPull": ("adaptive_inpaint_decision_v1", patch_adaptive_inpaint_decision_v1, BLACKWELL_ARCH),
}

EXTRA_PATCHERS = {
    "Kernel_BlendCandidatesFused": [
        ("validated_warp_blend_refined", patch_refined_blend),
        ("validated_warp_blend_refined_border", patch_refined_border_blend),
        ("adaptive_quality_blend_v1", patch_adaptive_blend),
        ("adaptive_quality_blend_v2", patch_adaptive_blend_v2),
        ("adaptive_quality_blend_v3", patch_adaptive_blend_v3),
    ],
    "Kernel_EstimateIntermMvecsScatter": [
        ("adaptive_quality_geometry_v1", patch_adaptive_geometry_v1),
        ("adaptive_quality_geometry_v2", patch_adaptive_geometry_v2),
        ("adaptive_quality_geometry_v31_local", patch_adaptive_geometry_v31_local),
        ("adaptive_quality_geometry_v31_temporal", patch_adaptive_geometry_v31_temporal),
        ("geometry_support_smooth_v2", patch_geometry_confidence_v2),
        ("geometry_motion_depth_refined", patch_refined_geometry),
        ("geometry_motion_depth", patch_silhouette_boundary_guard),
        ("geometry_motion_depth_aggressive",
         patch_silhouette_boundary_guard_aggressive),
    ],
    "Kernel_OutputPull": [
        ("adaptive_inpaint_decision_v2", patch_adaptive_inpaint_decision_v2),
        ("adaptive_inpaint_decision_v3_local",
         patch_adaptive_inpaint_decision_v3_local),
        ("adaptive_inpaint_decision_v3_temporal",
         patch_adaptive_inpaint_decision_v3_temporal),
    ],
}


class CudaDriverCompiler:
    """Compile PTX with the installed NVIDIA driver when ptxas is absent."""

    def __init__(self) -> None:
        if platform.system() != "Windows":
            raise RuntimeError("CUDA driver JIT currently requires Windows")
        self.cuda = ctypes.WinDLL("nvcuda.dll")
        self.cuda.cuInit.argtypes = [ctypes.c_uint]
        self.cuda.cuDeviceGet.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
        self.cuda.cuCtxCreate_v2.argtypes = [
            ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint, ctypes.c_int
        ]
        self.cuda.cuCtxDestroy_v2.argtypes = [ctypes.c_void_p]
        self.cuda.cuGetErrorString.argtypes = [
            ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)
        ]
        self.cuda.cuLinkCreate_v2.argtypes = [
            ctypes.c_uint, ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
        ]
        self.cuda.cuLinkAddData_v2.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.cuda.cuLinkComplete.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self.cuda.cuLinkDestroy.argtypes = [ctypes.c_void_p]
        self._check(self.cuda.cuInit(0), "cuInit")
        device = ctypes.c_int()
        self._check(self.cuda.cuDeviceGet(ctypes.byref(device), 0), "cuDeviceGet")
        self.context = ctypes.c_void_p()
        self._check(
            self.cuda.cuCtxCreate_v2(ctypes.byref(self.context), 0, device),
            "cuCtxCreate_v2",
        )

    def _check(self, result: int, operation: str) -> None:
        if result == 0:
            return
        message = ctypes.c_char_p()
        self.cuda.cuGetErrorString(result, ctypes.byref(message))
        detail = message.value.decode(errors="replace") if message.value else str(result)
        raise RuntimeError(f"{operation} failed: {detail}")

    def compile(self, source: str, name: str) -> bytes:
        state = ctypes.c_void_p()
        self._check(
            self.cuda.cuLinkCreate_v2(0, None, None, ctypes.byref(state)),
            "cuLinkCreate_v2",
        )
        try:
            encoded = ctypes.create_string_buffer(source.encode("ascii"))
            self._check(
                self.cuda.cuLinkAddData_v2(
                    state, 1, ctypes.cast(encoded, ctypes.c_void_p),
                    len(encoded), f"{name}.ptx".encode("ascii"), 0, None, None,
                ),
                f"cuLinkAddData_v2({name})",
            )
            pointer = ctypes.c_void_p()
            size = ctypes.c_size_t()
            self._check(
                self.cuda.cuLinkComplete(
                    state, ctypes.byref(pointer), ctypes.byref(size)
                ),
                f"cuLinkComplete({name})",
            )
            return ctypes.string_at(pointer, size.value)
        finally:
            self.cuda.cuLinkDestroy(state)

    def close(self) -> None:
        if self.context:
            self.cuda.cuCtxDestroy_v2(self.context)
            self.context = ctypes.c_void_p()


def compile_ptx(ptxas: Path | None, driver: CudaDriverCompiler | None,
                source: str, directory: Path, name: str) -> bytes:
    if driver is not None:
        cubin = driver.compile(source, name)
        print(f"  driver JIT ELF text/shared/registers [{name}]: "
              f"{fingerprint_elf(cubin)}")
        return cubin
    if ptxas is None:
        raise ValueError("no PTX compiler selected")
    source_path = directory / f"{name}.ptx"
    cubin_path = directory / f"{name}.cubin"
    source_path.write_text(source, encoding="ascii", newline="\n")
    command = [str(ptxas), "-arch=sm_89", "-O3", "-v"]
    strict_ptxas = name in {
        "adaptive_quality_geometry_v31_local",
        "adaptive_quality_geometry_v31_temporal",
        "adaptive_inpaint_decision_v3_local",
        "adaptive_inpaint_decision_v3_temporal",
    }
    if strict_ptxas:
        # Forty registers is the Ada occupancy boundary for this 324-thread
        # kernel. Override the provider's permissive directive, but retain the
        # hard zero-spill/local-storage gate below.
        command += ["--warn-on-spills", "--override-directive-values",
                    f"-maxrregcount={40 if 'geometry' in name else 48}"]
    command += [str(source_path), "-o", str(cubin_path)]
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(
            f"ptxas failed for {name}:\n{result.stdout}\n{result.stderr}"
        )
    if strict_ptxas:
        diagnostics = result.stdout + "\n" + result.stderr
        spill = re.search(
            r"(\d+) bytes spill stores,\s*(\d+) bytes spill loads",
            diagnostics,
        )
        stack = re.search(r"(\d+) bytes stack frame", diagnostics)
        if spill is None or stack is None:
            raise RuntimeError(
                f"ptxas did not report stack/spill counts for {name}"
            )
        if (int(stack.group(1)) != 0 or int(spill.group(1)) != 0 or
                int(spill.group(2)) != 0):
            raise RuntimeError(
                f"{name} uses ptxas stack/spills: "
                f"stack={stack.group(1)}, stores={spill.group(1)}, "
                f"loads={spill.group(2)}"
            )
    print(f"  compiler resources [{name}]:\n{result.stdout.strip()}\n{result.stderr.strip()}")
    print(f"  ELF text/shared/registers [{name}]: {fingerprint_elf(cubin_path.read_bytes())}")
    return cubin_path.read_bytes()


def extract_kernels(provider: Path):
    data, sections = pe_sections(provider)
    for start, end in iter_fatbins(data, sections):
        entries = list(iter_entries(data, start, end))
        sources: dict[int, str] = {}
        ada_cubin = None
        for kind, arch, offset, size, compressed, raw in entries:
            if kind == PTX_KIND and arch in (ADA_ARCH, BLACKWELL_ARCH):
                payload = data[offset:offset + compressed]
                decoded = lz4_decompress(payload, raw) if compressed else data[offset:offset + size]
                sources[arch] = decoded.replace(b"\r", b"").rstrip(b"\0").decode("ascii")
            elif kind == CUBIN_KIND and arch == ADA_ARCH and not compressed:
                ada_cubin = data[offset:offset + size]
        if ada_cubin is None or not sources:
            continue
        names = set()
        for source in sources.values():
            match = re.search(r"\.entry\s+([A-Za-z0-9_]+)\s*\(", source)
            if match:
                names.add(match.group(1))
        if len(names) == 1:
            yield names.pop(), sources, ada_cubin


def emit_header(records: list[dict], output: Path, providers: list[Path]) -> None:
    lines = [
        "// Generated by tools/build_thin_geometry_variants.py -- do not edit, do not commit.",
        "// Built from locally installed NVIDIA providers; no payload belongs in source control.",
        "// providers: " + ", ".join(path.name for path in providers),
        "",
        "#pragma once",
        "",
        "struct CubinVariant {",
        "  unsigned source_text;",
        "  unsigned source_shared;",
        "  unsigned source_regs;",
        "  unsigned slot_size;",
        "  unsigned long long source_fnv1a64;",
        "  unsigned size;",
        "  const unsigned char* data;",
        "  const char* mechanism;",
        "};",
        "",
    ]
    for index, record in enumerate(records):
        lines.append(f"static const unsigned char kThinGeometryCubin{index}[] = {{")
        blob = record["replacement"]
        for offset in range(0, len(blob), 16):
            lines.append("  " + "".join(f"0x{byte:02x}," for byte in blob[offset:offset + 16]))
        lines.extend(["};", ""])
    lines.append("static const CubinVariant kThinGeometryCubins[] = {")
    for index, record in enumerate(records):
        text, shared, registers = record["source_fingerprint"]
        lines.append(
            f"  {{{text}u, {shared}u, {registers}u, {record['slot_size']}u, "
            f"0x{record['source_hash']:016x}ull, sizeof(kThinGeometryCubin{index}), "
            f"kThinGeometryCubin{index}, \"{record['mechanism']}\"}},"
        )
    lines.extend(["};", ""])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="ascii", newline="\n")


def load_generated_records(path: Path) -> list[dict]:
    """Load an existing generated table so released V1 cubins stay byte-exact."""
    text = path.read_text(encoding="ascii")
    arrays = {
        match.group("name"): bytes(
            int(value, 16)
            for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group("body"))
        )
        for match in re.finditer(
            r"static const unsigned char (?P<name>kThinGeometryCubin\d+)\[\] = \{"
            r"(?P<body>.*?)\n\};",
            text,
            re.DOTALL,
        )
    }
    table = re.compile(
        r"\{(?P<text>\d+)u,\s*(?P<shared>\d+)u,\s*(?P<regs>\d+)u,\s*"
        r"(?P<slot>\d+)u,\s*0x(?P<hash>[0-9a-fA-F]+)ull,\s*"
        r"sizeof\((?P<array>kThinGeometryCubin\d+)\),\s*(?P=array),\s*"
        r'"(?P<mechanism>[^"]+)"\}'
    )
    records = []
    for match in table.finditer(text):
        name = match.group("array")
        if name not in arrays:
            raise ValueError(f"existing generated header is missing {name}")
        records.append({
            "mechanism": match.group("mechanism"),
            "source_fingerprint": (
                int(match.group("text")),
                int(match.group("shared")),
                int(match.group("regs")),
            ),
            "slot_size": int(match.group("slot")),
            "source_hash": int(match.group("hash"), 16),
            "replacement": arrays[name],
        })
    if not records:
        raise ValueError("existing generated header contains no cubin records")
    return records


def record_key(record: dict) -> tuple:
    return (
        record["mechanism"], record["source_fingerprint"],
        record["slot_size"], record["source_hash"],
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("provider", nargs="+", type=Path)
    compiler = parser.add_mutually_exclusive_group(required=True)
    compiler.add_argument("--ptxas", type=Path)
    compiler.add_argument("--driver-jit", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--preserve-existing", action="store_true",
        help="keep every existing cubin byte-exact and append only new variants",
    )
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument(
        "--allow-oversized-v3", action="store_true",
        help=(
            "emit a zero-spill geometry V3 cubin even when it exceeds the "
            "in-place slot/resource gate; the runtime must install it through "
            "an exact fatbin descriptor redirect"
        ),
    )
    args = parser.parse_args()

    existing = []
    if args.preserve_existing:
        if not args.output.exists():
            raise ValueError("--preserve-existing requires an existing output header")
        existing = load_generated_records(args.output)
    records = []
    seen = set()
    driver = CudaDriverCompiler() if args.driver_jit else None
    try:
        with tempfile.TemporaryDirectory(prefix="mfg-thin-geometry-") as temp:
            directory = Path(temp)
            for provider in args.provider:
                print(f"provider: {provider}")
                for name, sources, ada_cubin in extract_kernels(provider):
                    if name not in PATCHERS:
                        continue
                    mechanism, patcher, source_arch = PATCHERS[name]
                    if source_arch not in sources:
                        raise ValueError(f"{name}: missing sm_{source_arch} PTX")
                    source = sources[source_arch]
                    if args.inspect:
                        print(f"  {name}: sm_{source_arch}, {len(source)} PTX bytes")
                        if name != "Kernel_BlendCandidatesFused":
                            for line_number, line in enumerate(source.splitlines(), 1):
                                if "+60]" in line or "+120]" in line:
                                    print(f"    {line_number:04d}: {line}")
                    baseline_source = source.replace(".target sm_120", ".target sm_89")
                    baseline = (
                        ada_cubin
                        if source_arch == ADA_ARCH
                        else compile_ptx(args.ptxas, driver, baseline_source,
                                         directory, mechanism + "_baseline")
                    )
                    variants = [(mechanism, patcher)]
                    variants.extend(EXTRA_PATCHERS.get(name, []))
                    for variant_name, variant_patcher in variants:
                        patched_source = variant_patcher(baseline_source)
                        is_v31_local = (
                            variant_name == "adaptive_quality_geometry_v31_local")
                        is_v31_temporal = (
                            variant_name == "adaptive_quality_geometry_v31_temporal")
                        is_v31_geometry = is_v31_local or is_v31_temporal
                        is_inpaint_v3_local = (
                            variant_name == "adaptive_inpaint_decision_v3_local")
                        is_inpaint_v3_temporal = (
                            variant_name == "adaptive_inpaint_decision_v3_temporal")
                        is_inpaint_v3 = (is_inpaint_v3_local or
                                         is_inpaint_v3_temporal)
                        load_reference = (
                            patch_adaptive_geometry_v2(baseline_source)
                            if is_v31_geometry
                            else patch_adaptive_inpaint_decision_v2(
                                baseline_source)
                            if is_inpaint_v3
                            else baseline_source
                        )
                        if (variant_name == "adaptive_quality_blend_v3" and
                                patched_source.count("ld.") !=
                                load_reference.count("ld.")):
                            raise ValueError(
                                f"{variant_name}: V3 introduced a load"
                            )
                        if is_v31_local:
                            if (patched_source.count("tex.") !=
                                    load_reference.count("tex.") or
                                    patched_source.count("ld.global") !=
                                    load_reference.count("ld.global") or
                                    patched_source.count("st.global") !=
                                    load_reference.count("st.global")):
                                raise ValueError(
                                    f"{variant_name}: local cubin introduced a global or texture access"
                                )
                        if is_v31_temporal:
                            if (patched_source.count("tex.") !=
                                    load_reference.count("tex.") or
                                    patched_source.count("ld.global") !=
                                    load_reference.count("ld.global") + 8 or
                                    patched_source.count("ld.global.u8") !=
                                    load_reference.count("ld.global.u8") + 2 or
                                    patched_source.count("st.global.u8") !=
                                    load_reference.count("st.global.u8") + 2):
                                raise ValueError(
                                    f"{variant_name}: compact temporal load/store gate failed"
                                )
                        if is_inpaint_v3_local:
                            if (patched_source.count("tex.") !=
                                    load_reference.count("tex.") or
                                    patched_source.count("ld.global") !=
                                    load_reference.count("ld.global") or
                                    patched_source.count("st.global") !=
                                    load_reference.count("st.global")):
                                raise ValueError(
                                    f"{variant_name}: Local V3 introduced a global or texture access"
                                )
                        if is_inpaint_v3_temporal:
                            if (patched_source.count("tex.") !=
                                    load_reference.count("tex.") or
                                    patched_source.count("ld.global") !=
                                    load_reference.count("ld.global") + 5 or
                                    patched_source.count("st.global") !=
                                    load_reference.count("st.global") + 1 or
                                    patched_source.count("ld.global.u8") !=
                                    load_reference.count("ld.global.u8") + 1 or
                                    patched_source.count("st.global.u8") !=
                                    load_reference.count("st.global.u8") + 1):
                                raise ValueError(
                                    f"{variant_name}: temporal confidence byte load/store gate failed"
                                )
                        try:
                            replacement = compile_ptx(
                                args.ptxas, driver, patched_source, directory,
                                variant_name)
                        except RuntimeError as error:
                            if (not is_v31_temporal and
                                    not is_inpaint_v3_temporal) or args.ptxas is None:
                                raise
                            print(
                                f"  skip {variant_name}: "
                                "ptxas gate failed; the separately generated Local cubin remains available:\n"
                                f"{error}"
                            )
                            continue
                        replacement_fingerprint = fingerprint_elf(replacement)
                        replacement_local = local_storage_bytes(replacement)
                        if variant_name == "adaptive_quality_blend_v3":
                            if replacement_fingerprint[2] > 48 or replacement_local:
                                raise ValueError(
                                    "adaptive_quality_blend_v3 exceeds its "
                                    "48-register/zero-local-storage gate: "
                                    f"fp={replacement_fingerprint}, "
                                    f"local={replacement_local}"
                                )
                        if is_v31_geometry:
                            if args.ptxas is None:
                                print(
                                    f"  skip {variant_name}: "
                                    "driver JIT is validation-only; release "
                                    "geometry requires ptxas"
                                )
                                continue
                            text, shared, registers = replacement_fingerprint
                            text_limit = 39552 if is_v31_local else 0xFFFFFFFF
                            resource_gate_failed = (
                                (is_v31_local and text > text_limit) or
                                shared != 7776 or registers > 40 or
                                replacement_local
                            )
                            if resource_gate_failed and not args.allow_oversized_v3:
                                print(
                                    f"  skip {variant_name}: "
                                    "resource gate failed "
                                    f"size={len(replacement)}, fp="
                                    f"{replacement_fingerprint}, "
                                    f"local={replacement_local}"
                                )
                                continue
                            if resource_gate_failed:
                                print(
                                    f"  keep {variant_name} as "
                                    "redirect-only ptxas cubin: "
                                    f"size={len(replacement)}, fp="
                                    f"{replacement_fingerprint}, "
                                    f"local={replacement_local}"
                                )
                        if is_inpaint_v3:
                            if args.ptxas is None:
                                print(
                                    f"  skip {variant_name}: driver JIT is "
                                    "validation-only; release inpaint V3 requires ptxas"
                                )
                                continue
                            text, shared, registers = replacement_fingerprint
                            resource_gate_failed = (
                                shared != 784 or registers > 48 or
                                replacement_local)
                            if resource_gate_failed:
                                if is_inpaint_v3_temporal:
                                    print(
                                        f"  skip {variant_name}: resource gate failed; "
                                        "Local V3 remains available size="
                                        f"{len(replacement)}, fp={replacement_fingerprint}, "
                                        f"local={replacement_local}"
                                    )
                                    continue
                                raise ValueError(
                                    f"{variant_name}: resource gate failed "
                                    f"size={len(replacement)}, fp={replacement_fingerprint}, "
                                    f"local={replacement_local}"
                                )
                        if len(baseline) > len(ada_cubin):
                            print(f"  skip {variant_name}: baseline {len(baseline)} > slot {len(ada_cubin)}")
                            continue
                        if (len(replacement) > len(ada_cubin) and not
                                ((is_v31_geometry or is_inpaint_v3) and
                                 args.allow_oversized_v3)):
                            print(f"  skip {variant_name}: replacement {len(replacement)} > slot {len(ada_cubin)}")
                            continue
                        key = (variant_name, fingerprint_elf(ada_cubin), len(ada_cubin), fnv1a64(ada_cubin))
                        if key in seen:
                            continue
                        seen.add(key)
                        record = {
                            "mechanism": variant_name,
                            "source_fingerprint": fingerprint_elf(ada_cubin),
                            "slot_size": len(ada_cubin),
                            "source_hash": fnv1a64(ada_cubin),
                            "replacement": replacement,
                        }
                        records.append(record)
                        print(
                            f"  ok {variant_name}: fp={record['source_fingerprint']} "
                            f"slot={len(ada_cubin)} replacement={len(replacement)} "
                            f"source-fnv={record['source_hash']:016x}"
                        )
    finally:
        if driver is not None:
            driver.close()
    if not records:
        raise SystemExit("no supported experimental variants were generated")
    if existing:
        existing_keys = {record_key(record) for record in existing}
        additions = [
            record for record in records
            if record_key(record) not in existing_keys
        ]
        records = existing + additions
        print(f"preserved {len(existing)} existing variants byte-exact; "
              f"appended {len(additions)} new variants")
    emit_header(records, args.output, args.provider)
    print(f"wrote {args.output} ({len(records)} variants)")


if __name__ == "__main__":
    main()
