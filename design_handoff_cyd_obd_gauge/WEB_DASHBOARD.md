# Handoff: CYD OBD-II — Web Dashboard (served by the ESP32)

## Overview
This is a redesign of the web page that the ESP32 serves at `http://<device-ip>/`. It uses the same visual language as the on-device UI described in `README.md`: the same colors, the same font (Chakra Petch), and the same status-color rules (green = normal, amber = watch, red = danger). All existing functions are kept:
- live values
- reading and clearing DTCs
- Bluetooth adapter name/MAC and PIN, with save & reconnect

One addition: a backlight slider.

## About the Design Files
`Web Dashboard.html` is a **design reference**. It runs offline and simulates live values. Do not ship it as-is; it contains a design runtime. Rebuild the page as one lightweight HTML file with plain CSS and vanilla JS. Embed it in the firmware with `PROGMEM` / LittleFS. Keep the existing HTTP endpoints, and change only the page markup, CSS, and JS.

**Fidelity: high.** Match the colors, sizes, and spacing below exactly.

## Page layout
- Body background `#000`. Content uses `max-width: 1120px`, centered, with padding `28px 20px 48px`. Sections are a vertical stack with a 16px gap.
- The page is responsive. All rows use `flex-wrap: wrap`, so cards stack on mobile.

### 1. Header
- Bottom border: 1px `#14171B`, padding-bottom 18px. Left and right sides wrap onto separate lines when narrow.
- Left side: the title `CYD OBD-II` (24px, weight 700, letter-spacing .5px). In sim mode a `SIM` badge follows it: 11px, weight 700, black text on `#FFB000`, padding 2×7, radius 3.
- Right side: status chips, 8px apart. Each chip has a 1px `#2A2F36` border, radius 4, padding 6×12, and 13px text. Inside a chip: a 7px colored dot, a muted label (`#8A9099`), then the value.
  - `เครือข่าย WiFi`: dot `#3DDC97`
  - `OBD <STATE>`: `CONNECTING` is amber with a dot blinking at 1 Hz; `CONNECTED` is green; `DISCONNECTED` is red
  - `IP 192.168.1.13`: no dot

### 2. Live row (flex wrap, gap 12)
- **Gauge card**
  - Flex `1 1 300px`, background `#14171B`, radius 6, padding 16, min-height 280.
  - Caption: `ความเร็ว / รอบเครื่อง` at the left and `LIVE` at the right, 12px `#8A9099`, letter-spacing 1.
  - RPM arc: same geometry as the device arc, drawn as an SVG 240×236 with `viewBox="10 22 144 144"`.
    - Track: 9px, `#000`.
    - Value: 9px, `#FFB000`; turns `#FF4D3D` when RPM ≥ 6500.
    - Redline: 2.5px `#FF4D3D`, covering 6500–8000 rpm.
    - Ticks: 0–8k; the 7k and 8k ticks are red.
  - Speed: 72px, weight 600, letter-spacing −2, centered. Below it `km/h` at 13px `#8A9099`.
  - RPM: 24px, weight 600, in the arc color, with thousands separator. `RPM` label at 12px.
- **Tiles**
  - Container: flex `2 1 480px`. It is a grid with `repeat(auto-fill, minmax(150px, 1fr))` and gap 12.
  - Tile: background `#14171B`, radius 6, padding 14×16, min-height 112. Contents are a column with space-between and gap 14:
    1. Thai label (13px `#8A9099`) at the left, unit (12px `#8A9099`) at the right.
    2. Value (38px, weight 600) at the left; status word at the right (10px, weight 700, letter-spacing 1, in the status color): `ปกติ` / `เฝ้าระวัง` / `อันตราย`.
    3. Bar: 4px high, track `#2A2F36`, fill in the status color, minimum fill 4%.
  - Value color is `#F2F2EE` when normal. On WATCH or DANGER the value takes the status color.
  - Tile order and rules:

    | Label | Unit | Bar range | Rule |
    |---|---|---|---|
    | น้ำหล่อเย็น | °C | 60–120 | ≥ limit red; ≥ limit−8 amber; else green (limit default 105) |
    | แบตเตอรี่ | V | 11–15 | < 12.4 red; < 12.8 amber; else green |
    | น้ำมัน | % | 0–100 | < 10 red; < 20 amber; else green |
    | คันเร่ง | % | 0–100 | neutral: bar `#8A9099`, no status word |
    | โหลดเครื่อง | % | 0–100 | neutral |
    | อากาศเข้า | °C | 0–80 | neutral |

### 3. Bottom row (flex wrap, gap 12, align start)
Both cards: flex `1 1 420px`, background `#14171B`, radius 6, padding 18.

**DTC card** (column, gap 14)
- Header row:
  - Left: title `รหัสข้อผิดพลาด` (17px, weight 600) and `DTC` (12px `#8A9099`, letter-spacing 1).
  - Right: the code count (22px, weight 700; red if > 0, green if 0), the word `รหัส` (12px muted), and a `MIL ON` badge (black text on amber) when there are codes.
- Each code row:
  - Background `#000`, radius 4, min-height 52, overflow hidden.
  - Grid columns `4px | 72px | 1fr`, gap 12.
  - The 4px strip is the severity color: misfire codes (P03xx) red, others amber.
  - Code: 16px, weight 700. Then the English description (14px `#F2F2EE`) with the Thai description under it (12px `#8A9099`).
- Empty state: a row with a green 8px dot and the text `ไม่พบรหัสข้อผิดพลาด`.
- Buttons (height 44, radius 4, 14px, weight 600):
  - `อ่านรหัส`: 1px `#2A2F36` border; on hover the border becomes `#8A9099`. While waiting for the reply, its label is `กำลังอ่าน…` and the count shows `–`.
  - `ลบรหัส · กดค้าง 2 วินาที`: flex `1 1 200px`, 1px `#FF4D3D` border, red text, and the hint in `#8A9099` at 11px.
    - Hold to confirm: use `pointerdown` / `pointerup` / `pointerleave`, plus `touch-action: none`.
    - While held, a fill grows from left to right (`#FF4D3D` at 25% opacity) over 2000 ms. When it reaches 100%, send the clear request. Releasing early resets the fill.
- In sim mode only, show the footnote `ข้อมูลจำลอง (SIM)` (12px muted).

**Config card** (column, gap 16)
- Title: `ตั้งค่าอะแดปเตอร์ OBD` (17px, weight 600) and `BLUETOOTH` (12px muted).
- Text fields:
  - Label: 12px `#8A9099`, 6px above the input.
  - Input: height 44, background `#000`, 1px `#2A2F36` border, radius 4, padding 0×14, 16px `#F2F2EE` text. On focus the border becomes `#FFB000`, with no outline.
  - `ชื่อ หรือ MAC · AA:BB:CC:DD:EE:FF`
  - `PIN · เว้นว่างถ้าไม่มี`: `type=password`, placeholder `••••`.
- Backlight:
  - Label `ความสว่างหน้าจอ` with the current value (15px, weight 600) at the right.
  - `<input type=range min=5 max=100>` with `accent-color: #FFB000`.
  - Send the value to the device as the slider moves, debounced to about 150 ms.
- Primary button `บันทึกและเชื่อมต่อใหม่`: height 44, padding 0×22, background `#FFB000` (hover `#FFC94D`), black text, 14px, weight 700. After saving, show `บันทึกแล้ว · กำลังเชื่อมต่อ <name>` in `#3DDC97` (13px) next to the button.

### 4. Footer
12px `#8A9099`, wraps when narrow. Left: `อัปเดตอัตโนมัติทุก 0.6 วินาที`. Right: the firmware version.

## Data / behavior
- Poll `/api/live` (or the existing endpoint) every ~600 ms, or push over WebSocket. Update only the text nodes, bar widths, and the arc's `stroke-dasharray` (`<pct> 100` with `pathLength=100`). Do not rebuild the DOM on each update.
- Live fields: `rpm, speed, coolant, throttle, load, battery, intake, fuel, btState, sim, dtc[], mil`.

## Tokens
Use the same tokens as `README.md`:

| Color | Hex |
|---|---|
| Background | `#000000` |
| Surface | `#14171B` |
| Line | `#2A2F36` |
| Muted | `#8A9099` |
| Text | `#F2F2EE` |
| Accent | `#FFB000` |
| Normal | `#3DDC97` |
| Danger | `#FF4D3D` |

- Radius: 3 for badges, 4 for inputs/buttons/chips, 6 for cards.
- Font: Chakra Petch 400–700 from Google Fonts. Fall back to `system-ui` when the browser is offline; the ESP32 AP mode has no internet.

## Files
- `Web Dashboard.html`: offline visual reference.
- `Web Dashboard.dc.html`: design source with exact values.
