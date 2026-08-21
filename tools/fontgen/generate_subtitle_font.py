#!/usr/bin/env python3
"""Generate the production 1-bpp subtitle font from Lato Semibold."""

from __future__ import annotations

import argparse
import hashlib
import html
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


PIXEL_SIZE = 34
THRESHOLD = 96
CELL_WIDTH = 64
CELL_HEIGHT = 64
GRID_COLUMNS = 16
ORIGIN_X = 16
BASELINE_Y = 48
DEFAULT_FONT = Path("/usr/share/fonts/truetype/lato/Lato-Semibold.ttf")
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPO_ROOT / "src/svc/subtitle_pipeline/subtitle_font.c"
SPANISH_CODEPOINTS = "¡¿ÁÉÍÑÓÚÜáéíñóúü"
CODEPOINTS = tuple(sorted(set(range(0x20, 0x7F)) | {ord(ch) for ch in SPANISH_CODEPOINTS}))


@dataclass(frozen=True)
class FontMetrics:
    units_per_em: int
    ascender: int
    descender: int
    line_gap: int
    advances: dict[int, int]


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    width: int
    height: int
    x_offset: int
    y_offset: int
    advance: int
    bitmap: bytes


def be_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def be_i16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">h", data, offset)[0]


def be_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def table_directory(data: bytes) -> dict[str, tuple[int, int]]:
    tables: dict[str, tuple[int, int]] = {}
    num_tables = be_u16(data, 4)

    for index in range(num_tables):
        record = 12 + (index * 16)
        tag = data[record : record + 4].decode("ascii")
        tables[tag] = (be_u32(data, record + 8), be_u32(data, record + 12))

    return tables


def cmap_subtables(data: bytes, cmap_offset: int) -> list[int]:
    offsets: list[tuple[int, int]] = []
    count = be_u16(data, cmap_offset + 2)

    for index in range(count):
        record = cmap_offset + 4 + (index * 8)
        platform = be_u16(data, record)
        encoding = be_u16(data, record + 2)
        subtable = cmap_offset + be_u32(data, record + 4)
        fmt = be_u16(data, subtable)
        priority = 0
        if fmt == 12 and ((platform == 3 and encoding == 10) or platform == 0):
            priority = 3
        elif fmt == 4 and ((platform == 3 and encoding in (1, 10)) or platform == 0):
            priority = 2
        elif fmt in (4, 12):
            priority = 1
        if priority != 0:
            offsets.append((priority, subtable))

    return [offset for _, offset in sorted(offsets, reverse=True)]


def cmap_lookup_format12(data: bytes, offset: int, codepoint: int) -> int:
    groups = be_u32(data, offset + 12)

    for index in range(groups):
        group = offset + 16 + (index * 12)
        start = be_u32(data, group)
        end = be_u32(data, group + 4)
        if start <= codepoint <= end:
            return be_u32(data, group + 8) + (codepoint - start)
        if codepoint < start:
            break
    return 0


def cmap_lookup_format4(data: bytes, offset: int, codepoint: int) -> int:
    seg_count = be_u16(data, offset + 6) // 2
    end_codes = offset + 14
    start_codes = end_codes + (2 * seg_count) + 2
    deltas = start_codes + (2 * seg_count)
    range_offsets = deltas + (2 * seg_count)

    for index in range(seg_count):
        end = be_u16(data, end_codes + (2 * index))
        start = be_u16(data, start_codes + (2 * index))
        if start <= codepoint <= end:
            delta = be_i16(data, deltas + (2 * index))
            range_offset_pos = range_offsets + (2 * index)
            range_offset = be_u16(data, range_offset_pos)
            if range_offset == 0:
                return (codepoint + delta) & 0xFFFF
            glyph_pos = range_offset_pos + range_offset + (2 * (codepoint - start))
            glyph = be_u16(data, glyph_pos)
            return ((glyph + delta) & 0xFFFF) if glyph != 0 else 0
        if codepoint < start:
            break
    return 0


def cmap_lookup(data: bytes, subtables: list[int], codepoint: int) -> int:
    for offset in subtables:
        fmt = be_u16(data, offset)
        glyph = (
            cmap_lookup_format12(data, offset, codepoint)
            if fmt == 12
            else cmap_lookup_format4(data, offset, codepoint)
        )
        if glyph != 0:
            return glyph
    return 0


def parse_font_metrics(font: Path) -> FontMetrics:
    data = font.read_bytes()
    tables = table_directory(data)
    head = tables["head"][0]
    hhea = tables["hhea"][0]
    hmtx = tables["hmtx"][0]
    cmap = tables["cmap"][0]
    units_per_em = be_u16(data, head + 18)
    metric_count = be_u16(data, hhea + 34)
    subtables = cmap_subtables(data, cmap)
    advances: dict[int, int] = {}

    for codepoint in CODEPOINTS:
        glyph = cmap_lookup(data, subtables, codepoint)
        if glyph == 0 and codepoint != 0x20:
            raise RuntimeError(f"font has no glyph for U+{codepoint:04X}")
        metric_index = min(glyph, metric_count - 1)
        advance_units = be_u16(data, hmtx + (4 * metric_index))
        advances[codepoint] = max(1, ((advance_units * PIXEL_SIZE) + (units_per_em // 2)) // units_per_em)

    return FontMetrics(
        units_per_em=units_per_em,
        ascender=be_i16(data, hhea + 4),
        descender=be_i16(data, hhea + 6),
        line_gap=be_i16(data, hhea + 8),
        advances=advances,
    )


def render_grid(font: Path) -> tuple[bytes, int, int]:
    rows = (len(CODEPOINTS) + GRID_COLUMNS - 1) // GRID_COLUMNS
    width = GRID_COLUMNS * CELL_WIDTH
    height = rows * CELL_HEIGHT
    text_elements = []

    for index, codepoint in enumerate(CODEPOINTS):
        column = index % GRID_COLUMNS
        row = index // GRID_COLUMNS
        x = (column * CELL_WIDTH) + ORIGIN_X
        y = (row * CELL_HEIGHT) + BASELINE_Y
        text_elements.append(
            f'  <text x="{x}" y="{y}" font-family="SubtitleFont, Lato" '
            f'font-size="{PIXEL_SIZE}" font-weight="600" fill="white">'
            f"{html.escape(chr(codepoint))}</text>"
        )

    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">\n'
        "  <style>\n"
        f"    @font-face {{ font-family: SubtitleFont; src: url('{html.escape(font.resolve().as_uri())}'); }}\n"
        "  </style>\n"
        f'  <rect width="{width}" height="{height}" fill="black"/>\n'
        + "\n".join(text_elements)
        + "\n</svg>\n"
    )

    with tempfile.TemporaryDirectory(prefix="subtitle-font-") as temporary_directory:
        svg_path = Path(temporary_directory) / "glyphs.svg"
        svg_path.write_text(svg, encoding="utf-8")
        result = subprocess.run(
            (
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(svg_path),
                "-frames:v",
                "1",
                "-pix_fmt",
                "gray",
                "-f",
                "rawvideo",
                "pipe:1",
            ),
            check=True,
            stdout=subprocess.PIPE,
        )

    if len(result.stdout) != width * height:
        raise RuntimeError(f"renderer returned {len(result.stdout)} bytes; expected {width * height}")
    return result.stdout, width, height


def pack_glyph(gray: bytes, image_width: int, index: int, advance: int) -> Glyph:
    cell_x = (index % GRID_COLUMNS) * CELL_WIDTH
    cell_y = (index // GRID_COLUMNS) * CELL_HEIGHT
    visible: list[tuple[int, int]] = []

    for y in range(CELL_HEIGHT):
        for x in range(CELL_WIDTH):
            if gray[((cell_y + y) * image_width) + cell_x + x] >= THRESHOLD:
                visible.append((x, y))

    if not visible:
        return Glyph(CODEPOINTS[index], 0, 0, 0, 0, advance, b"")

    min_x = min(x for x, _ in visible)
    max_x = max(x for x, _ in visible)
    min_y = min(y for _, y in visible)
    max_y = max(y for _, y in visible)
    width = max_x - min_x + 1
    height = max_y - min_y + 1
    stride = (width + 7) // 8
    bitmap = bytearray(stride * height)

    for x, y in visible:
        local_x = x - min_x
        local_y = y - min_y
        bitmap[(local_y * stride) + (local_x // 8)] |= 1 << (7 - (local_x % 8))

    return Glyph(
        codepoint=CODEPOINTS[index],
        width=width,
        height=height,
        x_offset=min_x - ORIGIN_X,
        y_offset=min_y - BASELINE_Y,
        advance=advance,
        bitmap=bytes(bitmap),
    )


def format_byte_array(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{value:02X}U" for value in chunk) + ",")
    return "\n".join(lines)


def generate_source(font: Path) -> str:
    font_digest = hashlib.sha256(font.read_bytes()).hexdigest()
    metrics = parse_font_metrics(font)
    gray, image_width, _ = render_grid(font)
    glyphs = [
        pack_glyph(gray, image_width, index, metrics.advances[codepoint])
        for index, codepoint in enumerate(CODEPOINTS)
    ]
    arrays = []
    records = []

    for glyph in glyphs:
        name = f"glyph_{glyph.codepoint:04x}"
        bitmap_name = "NULL"
        if glyph.bitmap:
            arrays.append(
                f"static uint8_t const {name}[] = {{\n{format_byte_array(glyph.bitmap)}\n}};"
            )
            bitmap_name = name
        records.append(
            f"    {{0x{glyph.codepoint:04X}U, {glyph.width}U, {glyph.height}U, "
            f"{glyph.x_offset}, {glyph.y_offset}, {glyph.advance}U, {bitmap_name}}},"
        )

    line_units = metrics.ascender - metrics.descender + metrics.line_gap
    line_height = max(PIXEL_SIZE, ((line_units * PIXEL_SIZE) + metrics.units_per_em - 1) // metrics.units_per_em)
    fallback_index = CODEPOINTS.index(ord("?"))
    arrays_text = "\n\n".join(arrays)
    records_text = "\n".join(records)

    return f"""/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file subtitle_font.c
/// @brief Auto-generated Lato Semibold 34 px glyph atlas and proportional metrics
///

// Generated by tools/fontgen/generate_subtitle_font.py
// Source font: {font.name} (SHA-256: {font_digest})

// === Headers files inclusions ==================================================================================== //

#include "subtitle_font.h"

#include <stddef.h>

// === Macros definitions ========================================================================================== //

#define SUBTITLE_FONT_FALLBACK_INDEX ({fallback_index}U)

// === Private variable definitions ================================================================================ //

{arrays_text}

static subtitle_font_glyph_t const glyphs[] = {{
{records_text}
}};

// === Public function implementation ============================================================================== //

uint32_t subtitle_font_line_height(void)
{{
    return {line_height}U;
}}

subtitle_font_glyph_t const* subtitle_font_lookup(uint32_t const codepoint)
{{
    size_t low = 0U;
    size_t high = sizeof(glyphs) / sizeof(glyphs[0]);

    while (low < high)
    {{
        size_t const middle = low + ((high - low) / 2U);

        if (glyphs[middle].codepoint < codepoint)
        {{
            low = middle + 1U;
        }}
        else if (glyphs[middle].codepoint > codepoint)
        {{
            high = middle;
        }}
        else
        {{
            return &glyphs[middle];
        }}
    }}

    return &glyphs[SUBTITLE_FONT_FALLBACK_INDEX];
}}

// === End of documentation ======================================================================================== //
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT, help="TrueType/OpenType font file")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="generated C source")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.font.is_file():
        raise SystemExit(f"font not found: {args.font}")

    source = generate_source(args.font)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source, encoding="utf-8")
    print(f"wrote {args.output} ({len(CODEPOINTS)} glyphs, {PIXEL_SIZE} px)")


if __name__ == "__main__":
    main()
