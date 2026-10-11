#!/usr/bin/env python3
"""Generate bundled bitmap glyphs from DejaVu Sans Mono (license in assets)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import sys
font = ImageFont.truetype(sys.argv[1] if len(sys.argv)>1 else '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf',14)
lines=['/* Generated from DejaVu Sans Mono; see assets/FONT-LICENSE.txt. */','static const unsigned char font[95][16] = {']
for ch in range(32,127):
    image=Image.new('1',(9,16)); draw=ImageDraw.Draw(image)
    draw.text((0,-1),chr(ch),font=font,fill=1,stroke_width=0)
    # Two bytes per row, 9 columns packed little endian into uint16.
    rows=[sum((1<<x) for x in range(8) if image.getpixel((x,y))) for y in range(16)]
    lines.append('{'+','.join(str(r) for r in rows)+'},')
lines.append('};')
Path(__file__).resolve().parents[1].joinpath('kernel/font.h').write_text('\n'.join(lines)+'\n')
