"""Generate TFT_eSPI smooth fonts (.vlw format, stored as C arrays) from Chakra Petch.

Usage (from the project root):
    python tools/gen_fonts.py                 # writes include/ui_fonts.h
    python tools/gen_fonts.py --preview a.png # also renders sample strings the same way TFT_eSPI does

Each font instance below matches one size/weight from the design handoff
(design_handoff_cyd_obd_gauge/README.md). TFT_eSPI smooth fonts have no letter-spacing
setting, so the design's letter-spacing is baked into each glyph's xAdvance.

Thai glyphs are taken only from the Thai characters used in include/ui_text_th.h.
TFT_eSPI does not shape text: Thai vowel/tone marks are drawn at their default
(zero-advance, negative x-offset) position over the previous consonant.
"""

import argparse
import os
import re
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "tools", "fonts")
OUT_PATH = os.path.join(ROOT, "include", "ui_fonts.h")
THAI_SOURCE = os.path.join(ROOT, "include", "ui_text_th.h")

ASCII = "".join(chr(c) for c in range(0x21, 0x7F))
DIGITS = "0123456789"


def thai_chars():
    with open(THAI_SOURCE, encoding="utf-8") as f:
        text = f.read()
    return "".join(sorted({c for c in text if 0x0E00 <= ord(c) <= 0x0E7F}))


# name, weight, size(px), letter-spacing(px), characters
def font_specs():
    th = thai_chars()
    # v3/v4 (design_handoff_cyd_obd_gauge/README_v4.md): smallest size is 10px, sizes 10/11/12/14/18/20/24/30/50 only,
    # weights 600/700 (500 for Thai body text), Thai glyphs only at 10px and 12px
    return [
        ("FONT_10S", "SemiBold", 10, 0, ASCII + "°·–−"),   # labels, status text, HOLD 2s, RPM label, 0–100, −60 s
        ("FONT_10B", "Bold", 10, 0, ASCII),                # SIM / MIL ON badges, tab count, theme options
        ("FONT_10TH", "Medium", 10, 0, ASCII + th),        # Thai DTC description
        ("FONT_11S", "SemiBold", 11, 0, ASCII + "·°"),     # Fault codes, settings labels, km/h, English DTC text, FOCUS unit
        ("FONT_11B", "Bold", 11, 0, ASCII + "–"),          # OBD-II title, tabs, stats values, toast, COOLANT (alert)
        ("FONT_12B", "Bold", 12, 1, ASCII),                # tabs, buttons (letter-spacing 1)
        ("FONT_12V", "Bold", 12, 0, ASCII + "•"),          # settings values, PIN dots, keyboard keys
        ("FONT_12TH", "SemiBold", 12, 0, ASCII + th),      # alert message and button
        ("FONT_14B", "Bold", 14, 0, ASCII + "‹›−"),        # DTC code, chevrons, pager arrows, − + steppers
        ("FONT_18B", "Bold", 18, 0, DIGITS + ",-"),        # RPM value
        ("FONT_20B", "Bold", 20, 0, DIGITS + ":.,-"),      # fault count, TRIP values
        ("FONT_24S", "SemiBold", 24, 0, DIGITS + ".-"),    # tile values
        ("FONT_24B", "Bold", 24, 0, DIGITS + "-°CF"),      # alert coolant value
        ("FONT_30B", "Bold", 30, 2, "OVERHEAT" + DIGITS + ".-"),  # alert title, 0-100 time
        ("FONT_50S", "SemiBold", 50, -1.5, DIGITS + "-,."),  # speed, FOCUS value
    ]


def load(weight, size):
    path = os.path.join(FONT_DIR, f"ChakraPetch-{weight}.ttf")
    return ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)


def build_glyphs(font, chars, spacing):
    glyphs = []
    for ch in sorted(set(chars), key=ord):
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        img = Image.new("L", (max(1, w), max(1, h)), 0)
        if w and h:
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, anchor="ls", fill=255)
        bitmap = img.tobytes() if (w and h) else b""
        advance = font.getlength(ch)
        # combining marks (zero advance) must stay at zero, otherwise Thai marks would push text apart
        adv = 0 if advance == 0 else max(0, int(round(advance + spacing)))
        glyphs.append({"code": ord(ch), "w": w, "h": h, "adv": adv, "dy": -y0, "dx": x0, "bitmap": bitmap})
    return glyphs


def to_vlw(font, size, glyphs):
    ascent, descent = font.getmetrics()
    out = bytearray(struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent))
    for g in glyphs:
        out += struct.pack(">7i", g["code"], g["h"], g["w"], g["adv"], g["dy"], g["dx"], 0)
    for g in glyphs:
        out += g["bitmap"]
    name = b"ChakraPetch"
    out += bytes([len(name)]) + name + b"\0" + bytes([len(name)]) + name + b"\0" + b"\1"
    return bytes(out)


def c_array(name, data):
    lines = [f"const uint8_t {name}[] PROGMEM = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def render_preview(path, fonts, samples):
    """Draw sample strings the way TFT_eSPI drawGlyph does (cursor + dX, baseline - dY)."""
    img = Image.new("RGB", (640, 40 + 40 * len(samples)), (0, 0, 0))
    y = 40
    for name, text in samples:
        glyphs = {g["code"]: g for g in fonts[name]}
        x = 10
        for ch in text:
            if ch == " ":
                x += 3
                continue
            g = glyphs.get(ord(ch))
            if g is None:
                continue
            if g["w"] and g["h"]:
                mask = Image.frombytes("L", (g["w"], g["h"]), g["bitmap"])
                img.paste((242, 242, 238), (x + g["dx"], y - g["dy"]), mask)
            x += g["adv"]
        y += 40
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="write a PNG preview of sample strings")
    args = parser.parse_args()

    parts = [
        "#pragma once",
        "",
        "// =====================================================================",
        "//  ui_fonts.h -- ฟอนต์ Chakra Petch (SIL Open Font License, tools/fonts/OFL.txt) แบบ smooth (anti-aliased)",
        "//  ไฟล์นี้สร้างอัตโนมัติด้วย  python tools/gen_fonts.py  ห้ามแก้ด้วยมือ",
        "//  ใช้กับ tft.loadFont(FONT_xx) ของ TFT_eSPI",
        "// =====================================================================",
        "",
        "#include <Arduino.h>",
        "",
    ]
    total = 0
    built = {}
    for name, weight, size, spacing, chars in font_specs():
        font = load(weight, size)
        glyphs = build_glyphs(font, chars, spacing)
        built[name] = glyphs
        data = to_vlw(font, size, glyphs)
        total += len(data)
        parts.append(f"// {weight} {size}px, letter-spacing {spacing}px, {len(glyphs)} glyphs, {len(data)} bytes")
        parts.append(c_array(name, data))
        parts.append("")

    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))
    print(f"wrote {OUT_PATH}: {len(built)} fonts, {total} bytes of font data")

    if args.preview:
        render_preview(args.preview, built, [
            ("FONT_10TH", "P0301 สูบ 1 จุดระเบิดไม่สมบูรณ์"),
            ("FONT_10TH", "เซ็นเซอร์อุณหภูมิน้ำสัญญาณสูง"),
            ("FONT_12TH", "ลดความเร็ว และจอดพักเครื่องยนต์"),
            ("FONT_12TH", "แตะเพื่อรับทราบ"),
            ("FONT_11S", "Cylinder 1 misfire  km/h  Fault codes"),
            ("FONT_10S", "COOL °C  BATTERY V  THROTTLE  HOLD 2s"),
            ("FONT_50S", "188"),
            ("FONT_18B", "4,360"),
        ])
        print(f"wrote preview {args.preview}")


if __name__ == "__main__":
    main()
