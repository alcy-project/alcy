#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Draws the site's social card once: `uv run --with pillow python3 tools/og_card.py`.
The committed site/og.png is the artifact; nothing at build time depends
on Pillow."""

from PIL import Image, ImageDraw, ImageFont

W, H = 1200, 630
PAPER = "#fcfbf7"
INK = "#262420"
SOFT = "#635e52"
CRIMSON = "#e6002d"
MONO = "/usr/share/fonts/gsfonts/NimbusMonoPS-Bold.otf"

image = Image.new("RGB", (W, H), PAPER)
draw = ImageDraw.Draw(image)

# The monogram tile, the favicon at card scale.
size = 132
x, y = 96, 150
draw.rounded_rectangle((x, y, x + size, y + size), radius=30, fill=CRIMSON)
mark = ImageFont.truetype(MONO, 84)
bbox = draw.textbbox((0, 0), "a", font=mark)
mark_w = bbox[2] - bbox[0]
mark_h = bbox[3] - bbox[1]
draw.text(
    (x + (size - mark_w) / 2 - bbox[0], y + (size - mark_h) / 2 - bbox[1]),
    "a",
    font=mark,
    fill="#ffffff",
)

# The wordmark, set on the tile's centre line.
word = ImageFont.truetype(MONO, 150)
bbox = draw.textbbox((0, 0), "alcy", font=word)
word_h = bbox[3] - bbox[1]
word_x = x + size + 56
word_y = y + size / 2 - word_h / 2 - bbox[1]
draw.text((word_x, word_y), "alcy", font=word, fill=INK)

# The tagline.
tag = ImageFont.truetype(MONO, 40)
tagline = "A statically-typed systems language."
draw.text((x + 4, y + size + 72), tagline, font=tag, fill=SOFT)

image.save("site/og.png", optimize=True)
print(f"site/og.png {image.size}")
