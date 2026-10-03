"""Generate VLW (Processing/TFT_eSPI/LovyanGFX smooth font) data from an OTF/TTF.

Usage from Python: build_vlw(path, px, chars) -> (bytes, glyph_dict)
glyph_dict: unicode -> dict(w,h,adv,dx,dy,bmp(bytes))  (for preview rendering)
"""
import struct
import freetype


def build_vlw(path, px, chars):
    face = freetype.Face(path)
    face.set_pixel_sizes(0, px)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT

    glyphs = {}
    for ch in sorted(set(chars), key=ord):
        face.load_char(ch, flags)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        buf = bytes(bm.buffer)
        # bitmap.pitch may differ from width
        if bm.pitch != w:
            buf = b"".join(buf[r * bm.pitch:r * bm.pitch + w] for r in range(h))
        adv = (g.advance.x + 32) >> 6
        assert w < 256 and adv < 256, f"glyph {ch!r} too wide for VLW ({w}, {adv})"
        glyphs[ord(ch)] = dict(w=w, h=h, adv=adv, dx=g.bitmap_left,
                               dy=g.bitmap_top, bmp=buf)

    asc = (face.size.ascender + 32) >> 6
    desc = abs((face.size.descender - 32) >> 6)

    out = bytearray()
    out += struct.pack(">6i", len(glyphs), 11, px, 0, asc, desc)
    for u in sorted(glyphs):
        g = glyphs[u]
        out += struct.pack(">7i", u, g["h"], g["w"], g["adv"], g["dy"], g["dx"], 0)
    for u in sorted(glyphs):
        out += glyphs[u]["bmp"]
    # Processing appends font name strings; loaders ignore them, keep a tiny trailer
    out += b"\x00"
    return bytes(out), glyphs


def to_c_array(name, data, comment=""):
    lines = [f"// {comment}" if comment else "",
             f"// {len(data)} bytes",
             f"static const uint8_t {name}[] PROGMEM = {{"]
    for i in range(0, len(data), 20):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 20]) + ",")
    lines.append("};\n")
    return "\n".join(l for l in lines if l is not None)
