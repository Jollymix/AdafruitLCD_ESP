import sys
import os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vlwgen import build_vlw, to_c_array
# Bruk: python3 make_fonts.py <mappe med B612-Regular.ttf>  (github.com/polarsys/b612)
D = (sys.argv[1] if len(sys.argv) > 1 else HERE) + "/"
TXT = "".join(chr(c) for c in range(0x20, 0x7F)) + "°−æøåÆØÅ·…"
fonts = [
    ("FONT_BIG",   D + "B612-Regular.ttf",  76, "0123456789−,°C"),  # "−30,0°C" = 271 px, 24 px margin
    ("FONT_SMALL", D + "B612-Regular.ttf",  15, TXT),
    ("FONT_TINY",  D + "B612-Regular.ttf",  11, TXT),
]
out = ["// Generert av make_fonts.py fra B612 (Airbus/Intactile, SIL Open Font License 1.1)",
       "// VLW smooth fonts (anti-aliased) for LovyanGFX",
       "#pragma once", "#include <Arduino.h>", ""]
for name, path, px, chars in fonts:
    data, _ = build_vlw(path, px, chars)
    out.append(to_c_array(name, data, f"{path.split('/')[-1]} {px}px, {len(set(chars))} glyfer"))
    print(name, len(data), "bytes")
open(os.path.join(HERE, "..", "fonts.h"), "w").write("\n".join(out))
