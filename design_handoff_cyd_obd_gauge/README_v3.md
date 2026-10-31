# Handoff v3: CYD OBD-II Smart Gauge — Dark / Light themes

> **This file replaces the screen spec in `README.md` (v2).** Use v2 only for anything not covered here: hold-button behavior, ALERT hysteresis, refresh rates, and the state list. `WEB_DASHBOARD.md` is unchanged.

## Summary
The v2 build was checked on real hardware. Three problems showed up, and v3 fixes them:
1. Labels at 7–8px could not be read.
2. The background was still a blue gradient.
3. The six small tiles were too dense.

v3 also adds **Dark / Light / Auto** themes.

The design files are **HTML references**. Recreate them with the existing firmware drawing code (TFT_eSPI / LovyanGFX / LVGL). Do not change the OBD or BT logic.

- `Smart Gauge CYD v3.html` is the offline visual reference. Open it in a browser. It shows both themes at 2× scale.
- `Smart Gauge CYD v3.dc.html` is the source, with exact 320×240 pixel values.

## Hard rules
- **Minimum font size is 10px.** Labels use 10–11px at weight 600. Secondary numbers use 18px or more.
- Fill the background with **one solid color** using `fillScreen(theme.bg)`. Do not use a gradient.
  - If black still shows as blue, check `tft.invertDisplay()` and the `TFT_RGB_ORDER` / `TFT_INVERSION_*` settings in User_Setup.
- **All colors come from a theme struct.** Do not hard-code RGB565 values anywhere in the drawing code.
  - Switching theme means: redraw all static chrome, then redraw the current values.

```cpp
struct Theme { uint16_t bg, surf, line, track, muted, text, accent, ok, warn, bad, bar, knob, badSoft, alertA, alertB, alertFg, alertBtnBg, alertBtnFg; };
```

## Theme tokens
| Token | Dark hex | Dark 565 | Light hex | Light 565 |
|---|---|---|---|---|
| bg | #000000 | 0x0000 | #ECEAE5 | 0xEF5C |
| surf | #14171B | 0x10A3 | #FFFFFF | 0xFFFF |
| line | #2A2F36 | 0x2966 | #D6D3CC | 0xD699 |
| track | #22262C | 0x2125 | #E2DFD8 | 0xE6FB |
| muted | #9AA0A8 | 0x9D15 | #5C626A | 0x5B0D |
| text | #F2F2EE | 0xF79D | #121418 | 0x10A3 |
| accent | #FFB000 | 0xFD80 | #F5A400 | 0xF520 |
| ok | #3DDC97 | 0x3EF2 | #0E8A57 | 0x0C4A |
| warn | #FFB000 | 0xFD80 | #A86800 | 0xAB40 |
| bad | #FF4D3D | 0xFA67 | #D2281D | 0xD143 |
| bar (tab bg) | #0A0B0D | 0x0841 | #FFFFFF | 0xFFFF |
| badSoft (hold fill) | #3A1210 | 0x3882 | #FBE0DD | 0xFF1B |
| alertA / alertB | #FF4D3D / #D93A2B | | #D2281D / #B01F16 | |
| alertFg | #000000 | | #FFFFFF | |
| alertBtn bg / fg | #000000 / #F2F2EE | | #FFFFFF / #B01F16 | |

Light-theme cards (`surf`) also get a 1px `line` outline (#DEDBD4). Dark-theme cards have no outline.

**Verify the 565 values against `Smart Gauge CYD v3.dc.html`.** That file computes them as `((r>>3)<<11)|((g>>2)<<5)|(b>>3)`. Recompute with the same formula if anything looks off.

### Theme selection
- `CONFIG → Theme` offers DARK, LIGHT and AUTO. Save the choice to NVS (`Preferences`).
- AUTO picks a theme at boot and then every minute:
  - If a light sensor is fitted (the CYD has an LDR on GPIO34), use LIGHT above a threshold and DARK below it. Use hysteresis.
  - Otherwise use the time: 06:00–18:00 LIGHT, other times DARK.
- Suggested backlight: about 70% for Dark and 95% for Light. Let the user override it.

## Global layout
| Region | y | h | Spec |
|---|---|---|---|
| Status bar | 0 | 24 | padding-x 10, bottom line 1px `line` |
| Content | 28–200 | — | side padding 6 |
| Tab bar | 204 | 36 | bg `bar`, top line 1px `line` |

### Status bar
- Left: `OBD-II`, 11px bold `text`.
- In SIM mode, a `SIM` badge follows: 10px bold, black text on `accent`, 14px high, padding-x 5, radius 2.
- Right: a 6px dot and text at 10px weight 600, in the state color:
  - CONNECTING: `warn`; the dot blinks at 1 Hz (between `warn` and `line`).
  - CONNECTED: `ok`.
  - LOST: `bad`.

### Tab bar
- Three equal tabs. Labels are 12px bold with letter-spacing 1.
- Active tab: `text` color with a **3px `accent` bar on the top edge**.
- Inactive tabs: `muted`.
- The FAULT tab shows a count badge: 10px, white text on `bad`, 14px high, pill shape.

## 01 DASH
- **Gauge** (x 0–160): the SVG origin is at y=24. In screen coordinates the arc center is (80,128).
  - Sweep: 270°, from 135° to 405°.
  - Track: r 62, 11px wide, `track`.
  - Value: same arc, filled 0→rpm/8000, in `accent` (or `bad` when ≥ 6500).
  - Redline: r 71, 3px wide, `bad`, from 6500 to 8000.
  - Ticks: 0–8k, drawn from r 46 to r 52, 1.5px wide, `muted` (7k and 8k ticks in `bad`).
  - Speed: 50px weight 600, centered at x 80, top at y 78. Below it, `km/h` at 11px `muted`.
  - RPM: 18px bold, top at y 160, with thousands separator, followed by `RPM` at 10px `muted`. In Dark the number is `accent`; in Light it is `text`. In both themes it turns `bad` at redline.
- **Tiles** (x 162–314, y 30): a 2×2 grid with 4px gaps. Each tile is about 74×58, `surf` background, radius 4, padding 6/7/7.
  - Label: 10px weight 600 `muted`, nowrap.
  - Value: 24px weight 600.
  - Bar: 4px high, `track` background, radius 2, fill in the status color.
  - The value is `text` when normal, and takes the status color when watch or danger.
  - Tile contents:
    - `COOL °C`: 60–120. ≥ limit → `bad`; ≥ limit−8 → `warn`; otherwise `ok`.
    - `BATTERY V`: 11–15. < 12.4 → `bad`; < 12.8 → `warn`; otherwise `ok`.
    - `INTAKE °C`: 0–80. Neutral (bar in `muted`).
    - `FUEL %`: 0–100. < 10 → `bad`; < 20 → `warn`; otherwise `ok`.
- **Bars card** (x 162–314, y 154, h 46): `surf` background, padding 5×7.
  - Two rows, `THROTTLE` and `LOAD`. Each row has grid columns 44 | 1fr | 26.
  - Label: 10px weight 600 `muted`. Bar: 4px, fill in `text`. Value: 11px bold, right-aligned, e.g. `24%`.

## 02 FAULT
- **Header** (y 28, h 22):
  - Left: the count (20px bold `bad`) and `Fault codes` (11px weight 600).
  - Right: `MIL ON` badge, 10px bold black on `accent`, 16px high.
- **Rows** (start y 53): height 34, 3px gap, 3 rows per page.
  - Each row: `surf` background, radius 4, with columns 3px strip | 58px | 1fr and an 8px gap.
  - Strip: `bad` for critical codes (P03xx), `warn` for the rest.
  - Code: 14px bold.
  - Description: English at 11px weight 600, Thai below it at 10px `muted`. Line height 14px. Ellipsis on overflow.
  - Sample copy: P0301 Cylinder 1 misfire / สูบ 1 จุดระเบิดไม่สมบูรณ์ · P0420 Catalyst efficiency B1 / แคตาไลติกประสิทธิภาพต่ำ · P0171 Mixture too lean B1 / ส่วนผสมบางเกินไป
- **Actions** (y 164, h 34): columns 86 | 1fr, 6px gap. Labels are 12px bold.
  - `READ`: `surf` background with a `line` border.
  - `CLEAR HOLD 2s`: 1.5px `bad` border, `bad` text; the hint is 10px `muted`. While held, the inside fills with `badSoft` from left to right.
- The pager (‹ ›) appears only when there are more than 3 codes. Put it in the header, at the right next to MIL ON.

## 03 CONFIG
- **Settings list card** (y 30): `surf` background, radius 4, rows separated by 1px `line`.
  - Each row: label at 11px weight 600 `muted` on the left, value on the right.
  1. `Adapter`: `OBDII` (12px bold) and `›`. Row height 31.
  2. `PIN`: `••••` and `›`. Row height 31.
  3. `Theme`: a segmented control on a `track` background with 2px padding. It has three options, 44×20 each, 10px bold. The active option is `surf` with `text`; the others are `muted`. Row height 31.
  4. `Backlight`: grid columns 62 | 1fr | 32. Slider track 4px, fill `accent`, knob 14px `knob` with a 1px `line` ring. The value shows on the right at 12px bold. Row height 33.
- **Buttons** (y 164, h 34): two equal columns.
  - `CONNECT`: `accent` fill with black text.
  - `RECAL HOLD 2s`: `surf` with a `line` border.
- Hit areas: a full row is the touch target. Tapping Adapter or PIN opens the existing editor.

## 04 ALERT
- The full screen alternates between `alertA` and `alertB` at 1 Hz. All content is centered in `alertFg`.
- Contents, top to bottom:
  1. Warning triangle (34×30, 3px stroke).
  2. `OVERHEAT`, 30px bold, letter-spacing 2.
  3. `COOLANT` at 11px bold, then the value at 24px bold.
  4. `ลดความเร็ว และจอดพักเครื่องยนต์`, 12px weight 600.
  5. Button `แตะเพื่อรับทราบ`: 12px bold, padding 10×22, in alertBtn colors.

## Font
Chakra Petch at weights 600 and 700, with 500 optional. Generate only these sizes: 10, 11, 12, 14, 18, 20, 24, 30, 50. Thai glyphs are needed only at 10px and 12px.

## Files
- `Smart Gauge CYD v3.html`, `Smart Gauge CYD v3.dc.html`: v3 screens (current).
- `README.md`, `Smart Gauge CYD v2.*`: v2 (superseded for screens).
- `WEB_DASHBOARD.md`, `Web Dashboard.*`: web page served by the ESP32.
