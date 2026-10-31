# Handoff: CYD OBD-II Smart Gauge — UI Redesign v2

## Overview
Redesign of an existing ESP32 "Cheap Yellow Display" (ESP32-2432S028, ILI9341 2.8" 320×240, resistive touch XPT2046) OBD-II gauge. The firmware already has 3 tabs — **DASH, FAULT, CONFIG** — plus status bar and bottom tab bar. This redesign keeps all existing functions and data, changes only the visual layer, and adds one new screen: a full-screen **ALERT** overlay.

## About the Design Files
The files here are **design references made in HTML**. They show the intended look and behavior. They are not code to ship. Recreate the screens in the existing firmware (TFT_eSPI / LovyanGFX / LVGL — whichever the project uses), following its existing patterns. Do not rewrite the OBD/BT logic. Only replace the drawing code.

- `Smart Gauge CYD v2.html` — standalone and offline. Open it in a browser. Screens render at 2× scale with live simulated values.
- `Smart Gauge CYD v2.dc.html` — source of the same design. Coordinates in the markup are real 320×240 pixels (inside a `transform: scale(2)` wrapper).

## Fidelity
**High-fidelity.** All coordinates, sizes, and colors below are final, in physical screen pixels (landscape, rotation so width = 320).

---

## Global layout (all tabs)
| Region | y | height | Notes |
|---|---|---|---|
| Status bar | 0 | 20 | bottom border 1px `#14171B` |
| Content | 22–204 | ~182 | per tab |
| Tab bar | 206 | 34 | top border 1px `#2A2F36` |

Background is solid `#000000` everywhere. Remove the old blue gradient and the nested rounded frames.

### Status bar (h 20, padding-x 8)
- Left: `CYD OBD-II`, 9px bold, `#F2F2EE`. After a 6px gap, a **SIM** badge appears only in simulation mode: 7px bold, black text on `#FFB000`, padding 1×4, radius 2.
- Right: BT status as a 5px round dot, a 4px gap, then 9px text. The dot and text share one color:
  - `BT CONNECTING` → `#FFB000`, dot blinks 1 Hz (alternates with `#2A2F36`)
  - `BT CONNECTED` → `#3DDC97`
  - `BT LOST` → `#FF4D3D`

### Tab bar (y 206, h 34, 3 equal columns ≈106px)
- Labels `DASH` / `FAULT` / `CONFIG`: 10px semibold, letter-spacing 1px, centered.
- Inactive tab: text `#8A9099`, no background.
- Active tab: text `#F2F2EE`, background `#14171B`, and a **2px `#FFB000` line on its top edge**.
- The FAULT tab shows a count badge after its label (when count > 0): 7px bold, black text on `#FF4D3D`, height 11, padding-x 4, pill shape.
- Touch: the whole 106×34 cell is the hit area.

---

## Screen 01 — DASH
**Left gauge area (x 0–164)**
- Arc center (82,108). It sweeps 270°, from 135° to 405° (clockwise, opening at the bottom).
  - Track: r 64, stroke 9, `#14171B`.
  - Value: same arc, stroke 9, filled 0→RPM/8000. Color `#FFB000`, or `#FF4D3D` when RPM ≥ 6500.
  - Redline: r 72, stroke 2.5, `#FF4D3D`, covering 6500–8000 (last 18.75% of the sweep).
  - Ticks: at 0,1,…,8 k. Radial line from r 50 to r 55, width 1.2. `#8A9099`; 7 k and 8 k ticks in `#FF4D3D`.
  - Labels: `0` at (44,170) `#8A9099`; `8k` at (120,170) `#FF4D3D`. 7px, centered.
- Speed: centered at x 82, top y 72. 46px semibold, letter-spacing −1, `#F2F2EE`. Below it, `km/h` at 9px `#8A9099`, letter-spacing 1.
- RPM: centered at x 82, y≈158. Value 16px semibold in the same color as the arc. Label `RPM` 8px `#8A9099`.

**Right tile grid (x 164–316, y 24–202)**
- 2 columns × 3 rows, gap 4. Each tile is about 74×56, background `#14171B`, radius 3, padding 5×7.
- Inside each tile (column, space-between):
  1. Top row: label on the left (7px, `#8A9099`, letter-spacing 1) and unit on the right (7px `#8A9099`).
  2. Value: 22px semibold. Color `#F2F2EE` when normal; when WATCH/DANGER it takes the status color.
  3. Bar: 3px high, track `#2A2F36`, fill width = % of range, radius 2.

| Tile | Unit | Bar range | Status color rule |
|---|---|---|---|
| COOLANT | °C | 60–120 | ≥ limit → `#FF4D3D`; ≥ limit−8 → `#FFB000`; else `#3DDC97` (limit default 105) |
| THROTTLE | % | 0–100 | always neutral `#8A9099` |
| LOAD | % | 0–100 | neutral |
| BATTERY | V (1 decimal) | 11–15 | < 12.4 red; < 12.8 amber; else green |
| INTAKE | °C | 0–80 | neutral |
| FUEL | % | 0–100 | < 10 red; < 20 amber; else green |

Minimum bar width is 4%.

## Screen 02 — FAULT
- Summary row (x 8–312, y 24, h 24):
  - Left: count `3` at 20px bold `#FF4D3D`, then a 5px gap, then `FAULT CODES` at 9px `#F2F2EE`, letter-spacing 1.
  - Right: `MIL ON` badge (8px bold, black on `#FFB000`, padding 2×5, radius 2), then a 6px gap, then page `1/1` at 8px `#8A9099`.
- List (x 4–316, starting y 52): rows 32px high with 3px gap; 3 rows fit per page. Each row has background `#14171B` and radius 3, with columns `[3px severity strip | 52px code | text]` and gap 8:
  - Severity strip: `#FF4D3D` for misfire (P03xx) and other critical codes; `#FFB000` for others.
  - Code: 12px bold, letter-spacing .5, `#F2F2EE`.
  - Text: English description 9px `#F2F2EE` on top, Thai description 8px `#8A9099` below. Single line each, ellipsis on overflow.
  - Sample copy:
    - P0301 Cylinder 1 misfire / สูบ 1 จุดระเบิดไม่สมบูรณ์
    - P0420 Catalyst efficiency B1 / แคตาไลติกประสิทธิภาพต่ำ
    - P0171 Mixture too lean B1 / ส่วนผสมบางเกินไป
- Action row (x 4–316, y 164, h 34), columns `72 | 1fr | 34 | 34`, gap 4, all 9px semibold, letter-spacing 1:
  - `READ`: 1px `#2A2F36` border, radius 3, text `#F2F2EE`.
  - `CLEAR` + `HOLD 2s` (7px `#8A9099`): 1px `#FF4D3D` border, text `#FF4D3D`. **Hold feedback:** while pressed, a fill grows left→right over 2 s (`#FF4D3D` at ~22% opacity, or a dark red such as `#3A1210` in RGB565). It fires at 100%. Releasing early resets it.
  - `‹` / `›`: background `#14171B`. Arrow 12px, `#2A2F36` when disabled, `#F2F2EE` when enabled.

## Screen 03 — CONFIG
- Row 1 (y 24, h 36), columns `1fr | 92`, gap 4. Each field: background `#14171B`, radius 3, padding-x 8. Inside: label at 7px `#8A9099` letter-spacing 1 above a value at 12px semibold, with a chevron `›` at 12px `#8A9099` on the right.
  - `OBD DEVICE · NAME / MAC` → `OBDII`
  - `PIN` → shown masked as `••••` (letter-spacing 2)
- Row 2 (y 66, h 34), 2 equal columns, gap 4, 10px bold, letter-spacing 1:
  - `CONNECT`: **primary**, fill `#FFB000`, text `#000`.
  - `RECAL` + `HOLD 2s`: 1px `#2A2F36` border, text `#F2F2EE`. Uses the same hold-fill behavior as CLEAR.
- Backlight card (y 106, h 44): background `#14171B`, padding 6×10. `BACKLIGHT` label at 7px, value `95%` at 12px semibold on the right. Slider: 16px-high touch row; track 4px `#2A2F36`; fill `#FFB000`; knob a 16px circle `#F2F2EE` with a 3px black ring.
- Info row (y 156, h 44), 3 equal tiles, gap 4, background `#14171B`. Each has a 7px label and a 12px value:
  - `UNITS` → `°C · km/h`
  - `COOL ALERT` → `105°C` in `#FF4D3D`
  - `FW` → firmware version (placeholder value)

## Screen 04 — ALERT (new)
- Full-screen overlay, drawn over any tab when coolant ≥ limit.
- Background alternates `#FF4D3D` and `#D93A2B` at 1 Hz. All content is black and centered vertically, gap 6:
  1. Warning triangle icon, 34×30, 3px black stroke, with a "!" mark.
  2. `OVERHEAT`: 30px bold, letter-spacing 2.
  3. `COOLANT` (10px semibold) followed by the value (22px bold, e.g. `108°C`).
  4. `ลดความเร็ว และจอดพักเครื่องยนต์`: 11px.
  5. Button `แตะเพื่อรับทราบ`: black fill, `#F2F2EE` text 11px semibold, padding 9×20, radius 3.
- Tapping anywhere acknowledges the alert and returns to the previous tab. Do not show it again until the value drops below limit−3 and then crosses the limit again (hysteresis).

---

## Interactions & Behavior
- Tab bar switches screens. Optionally, a horizontal swipe switches tabs too.
- Hold buttons (CLEAR, RECAL) need 2 s of continuous press and show a progress fill while held.
- BT dot blinks only while connecting.
- Refresh rates: numeric values at 5 Hz, using partial redraw. Redraw only a value's own rectangle when it changes, and the arc only between the old and new angle. Static chrome is drawn once per tab change.

## State
`activeTab`, `btState (connecting|connected|lost)`, `simMode`, `rpm`, `speed`, `coolant`, `throttle`, `load`, `battery`, `intake`, `fuel`, `dtcList[]`, `dtcPage`, `milOn`, `backlight`, `units (C|F)`, `coolantLimit`, `alertActive`, `alertAcked`, `holdStartMs`.

## Design Tokens
| Token | Hex | RGB565 |
|---|---|---|
| Background | #000000 | 0x0000 |
| Surface | #14171B | 0x10A3 |
| Line / disabled | #2A2F36 | 0x2966 |
| Muted text | #8A9099 | 0x8C93 |
| Text | #F2F2EE | 0xF79D |
| Accent / Watch | #FFB000 | 0xFD80 |
| Normal | #3DDC97 | 0x3EF2 |
| Danger | #FF4D3D | 0xFA67 |
| Alert alt | #D93A2B | 0xD9C5 |

- Radius: 2 (badges), 3 (tiles, buttons, fields).
- Spacing: 4px gutters between tiles; 6–8px screen padding.
- Type scale (px): 46 speed · 30 alert title · 22 tile value · 20 count · 16 RPM · 12 field/code · 10 tabs/buttons · 9 status/body · 8 secondary · 7 labels.

## Font
**Chakra Petch** (Google Fonts, OFL; covers Thai). Weights: 500, 600, 700.
- Convert with the LVGL Font Converter, or generate TFT_eSPI smooth fonts (`.vlw`), at each size used.
- To save flash, include only these glyphs: `0-9 . , : % ° / ‹ › • A-Z a-z`, plus the Thai glyphs actually used, at the 8–11px sizes only.
- If flash is too tight, fall back to the built-in FreeSans fonts. Keep the same sizes and weights where possible.

## Assets
No bitmap images. The warning triangle is drawn with primitives: `drawTriangle` with a 3px stroke, plus `fillRect` for the "!".

## Files
- `Smart Gauge CYD v2.html` — offline visual reference (open in a browser)
- `Smart Gauge CYD v2.dc.html` — design source with exact pixel values
