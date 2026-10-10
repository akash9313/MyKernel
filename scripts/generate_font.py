import re
import os

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE_DIR, 'third_party', 'font_8x16.c')
DST = os.path.join(BASE_DIR, 'kernel', 'drivers', 'font.c')

with open(SRC, 'r', encoding='latin-1') as f:
    lines = f.readlines()

data_bytes = []
for line in lines:
    line_clean = line.strip()
    m = re.match(r'^(0x[0-9a-fA-F]{2}),', line_clean)
    if m:
        data_bytes.append(m.group(1))

assert len(data_bytes) == 4096, f"Expected 4096 bytes, got {len(data_bytes)}"

with open(DST, 'w', encoding='ascii') as out:
    out.write('/* 8x16 Bitmap Font Data */\n')
    out.write('/* Sourced from Linux kernel lib/fonts/font_8x16.c (VGA standard glyphs) */\n')
    out.write('#include "font.h"\n\n')
    out.write('const u8 font_bitmap_8x16[256][16] = {\n')
    for i in range(256):
        row = data_bytes[i*16:(i+1)*16]
        out.write(f'    /* 0x{i:02X} */ {{ {", ".join(row)} }},\n')
    out.write('};\n')

print(f"Generated {DST} cleanly with exact 4096 bytes.")
