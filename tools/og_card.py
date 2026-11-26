#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Draws the site's social card once: `uv run --with pillow --with cairosvg
python3 tools/og_card.py`. The committed site/og.png is the artifact;
nothing at build time depends on Pillow. The mark is the site's favicon,
so the two cannot drift. JetBrains Mono Regular/Bold and Inter Regular
need to be installed under ~/.local/share/fonts."""

from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image, ImageDraw, ImageFont

W, H = 1200, 630
PAPER = "#fcfbf7"
INK = "#262420"
SOFT = "#635e52"
RULE = "#e5e2da"
KEYWORD = "#a626a4"
FUNCTION = "#1a5fa8"
NUMBER = "#b25b00"
STRING = "#0a7a3d"
MONO = Path.home() / ".local/share/fonts/JetBrainsMono-Regular.ttf"
MONO_BOLD = Path.home() / ".local/share/fonts/JetBrainsMono-Bold.ttf"
SANS = Path.home() / ".local/share/fonts/Inter-Regular.ttf"

image = Image.new("RGB", (W, H), PAPER)
draw = ImageDraw.Draw(image)

MARGIN = 96
WORD_LIFT = 8


def place(x, y, text, font, fill):
    """Draws text at the same baseline as its neighbours; returns its width."""
    draw.text((x, y), text, font=font, fill=fill)
    return draw.textlength(text, font=font)


def segments(x, y, parts):
    """Draws one line made of separately coloured runs."""
    for text, font, fill in parts:
        x += place(x, y, text, font, fill)


# The mark is the site's favicon, drawn at card scale from the SVG.
tile = 88
x, y = MARGIN, 76
scale = 4
favicon = Path(__file__).resolve().parent.parent / "site/favicon.svg"
svg_bytes = cairosvg.svg2png(
    url=str(favicon), output_width=tile * scale, output_height=tile * scale
)
mark = Image.open(BytesIO(svg_bytes)).convert("RGBA")
mark = mark.resize((tile, tile), Image.Resampling.LANCZOS)
image.paste(mark, (x, y), mark)
word = ImageFont.truetype(MONO_BOLD, 96)
a_bbox = word.getbbox("A")
place(x + tile + 34, y + tile - a_bbox[3] - WORD_LIFT, "Alcy", word, INK)

draw.rectangle((MARGIN, 202, W - MARGIN, 204), fill=RULE)

# The code is the subject of the card. Its colours are the site's syntax
# palette, so the card reads as a page the reader has already seen.
code = ImageFont.truetype(MONO, 50)
bold = ImageFont.truetype(MONO_BOLD, 50)
code_x = MARGIN + 4
segments(
    code_x,
    236,
    [("fn ", bold, KEYWORD), ("main", bold, FUNCTION), ("() {", code, SOFT)],
)
segments(
    code_x,
    306,
    [
        ("  total", code, INK),
        (" := ", code, SOFT),
        ("1", code, NUMBER),
        (" + ", code, SOFT),
        ("1", code, NUMBER),
    ],
)
segments(
    code_x,
    376,
    [
        ("  println", code, FUNCTION),
        ("(", code, SOFT),
        ('"total = {}"', code, STRING),
        (", ", code, SOFT),
        ("total", code, INK),
        (")", code, SOFT),
    ],
)
segments(code_x, 446, [("}", code, SOFT)])

tag = ImageFont.truetype(SANS, 30)
place(MARGIN, 540, "A statically-typed systems language.", tag, SOFT)

image.save("site/og.png", optimize=True)
print(f"site/og.png {image.size}")
