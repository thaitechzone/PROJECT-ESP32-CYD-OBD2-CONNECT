// OBD-II Bluetooth Smart Gauge  (ESP32-2432S028 "CYD", Classic Bluetooth SPP + ELM327)  -- UI v4
//
// UI วาดด้วย TFT_eSPI + จอสัมผัส XPT2046 (อ่านเองผ่าน SPI) ดีไซน์: design_handoff_cyd_obd_gauge/README_v4.md
//   หน้า DRIVE : shift light + มาตรวัดรอบ 270° (ตัวเลขหน้าปัด, peak) + ความเร็วตัวเลขใหญ่ + การ์ด COOL / BATT / FUEL / THR
//                แตะการ์ด/มาตรวัด = ดูละเอียดในหน้า FOCUS, กดค้างมาตรวัด 2 วินาที = ล้าง peak
//   หน้า FOCUS : ค่าเดียวขนาดใหญ่ (8 ค่า) + MIN/AVG/MAX + กราฟ 60 วินาที
//   หน้า TRIP  : เวลา, ระยะทาง, ความเร็วเฉลี่ย/สูงสุด, รอบสูงสุด + จับเวลา 0-100 km/h
//   หน้า FAULT : อ่านรหัสข้อผิดพลาด (DTC, Mode 03) และลบรหัส (Mode 04, กดค้าง 2 วินาที)
//   หน้า SET   : รายการตั้งค่าที่ลากเลื่อนได้ (อะแดปเตอร์, PIN, ธีม, ความสว่าง, เสียง, shift light, เกณฑ์น้ำร้อน, calibrate)
//   หน้า SCAN  : ค้นหาอุปกรณ์ Bluetooth รอบตัว แสดงชื่อ/MAC/ความแรงสัญญาณ อะแดปเตอร์ OBD ขึ้นก่อน แตะเพื่อเลือกและเชื่อมต่อ
//   ปัดซ้าย/ขวาเปลี่ยนหน้า DRIVE / FOCUS / TRIP / FAULT, แถบเตือน (toast) ด้านบนเมื่อแบต/น้ำมันต่ำหรือสัญญาณหลุด
//   แสดงเฉพาะค่าจริงจากรถ อ่านไม่ได้ = "--", ECU ไม่รองรับ = "N/A"
//
// โครงสร้างโปรแกรม
//   - OBD state machine ทำงานใน FreeRTOS task แยก (core 0): NoConfig -> BtStart -> Connecting -> Init -> Online (-> Backoff เมื่อหลุด)
//     คำสั่ง AT/PID ทุกคำสั่งเป็นแบบ "ส่งแล้วรอคำตอบจนเจอ '>' หรือหมดเวลา" ไม่บล็อก UI
//     (มีแค่ขั้นตอน SerialBT.connect() ที่ค้นหาอุปกรณ์ ใช้เวลาหลายวินาที ซึ่งอยู่ใน task นี้ จอสัมผัสยังลื่นอยู่)
//   - loop() ทำหน้าที่ UI + เว็บ: อ่านจอสัมผัส -> ปุ่ม/หน้าจอ -> วาดเฉพาะส่วนที่เปลี่ยน; สถิติ/trip/กราฟคำนวณจากค่าที่อ่านได้
//   - soundTask เล่นเสียงจากคิว
//
// จอสัมผัส (XPT2046): SCK GPIO25, MISO GPIO39, MOSI GPIO32, CS GPIO33 (VSPI) ค่า calibrate จำถาวรใน Preferences
// ขา LED: แดง = ยังไม่เชื่อมต่อ, น้ำเงิน = กำลังเชื่อมต่อ/เริ่มต้น ELM327, เหลือง = ต่อแล้วแต่ ECU ไม่ตอบ, เขียว = อ่านค่าจริง (REAL)
//
// คำสั่ง Serial: ดู printHelp() (h = help)

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <esp_gap_bt_api.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>

#include "app_config.h"
#include "board_config.h"
#include "ui_fonts.h"
#include "ui_text_th.h"
#include "web_page.h"

// ถ้ายังไม่มี secrets.h โปรแกรมก็ทำงานได้ แค่จะไม่ลองต่อ WiFi และเปิด AP ของตัวบอร์ดทันที
#if __has_include("secrets.h")
#include "secrets.h"
#else
inline constexpr char WIFI_SSID[] = "";
inline constexpr char WIFI_PASSWORD[] = "";
#endif

// รหัส AP ของตัวบอร์ด: ตั้งเองได้ใน secrets.h ด้วย #define CYD_AP_PASSWORD "..." (ไม่อย่างนั้นใช้ค่าใน app_config.h)
#ifdef CYD_AP_PASSWORD
static_assert(sizeof(CYD_AP_PASSWORD) >= 9 && sizeof(CYD_AP_PASSWORD) <= 64, "CYD_AP_PASSWORD must be 8-63 characters");
inline constexpr const char *AP_PASS = CYD_AP_PASSWORD;
inline constexpr bool AP_PASS_IS_DEFAULT = false;
#else
inline constexpr const char *AP_PASS = app::AP_PASSWORD;
inline constexpr bool AP_PASS_IS_DEFAULT = true;
#endif

namespace {

// =====================================================================
//  ธีมสีและตำแหน่งบนจอ (ดีไซน์ v3: design_handoff_cyd_obd_gauge/README_v3.md)
//  ทุกสีในโค้ดวาดจอมาจากธีมปัจจุบัน (T) เท่านั้น ห้ามใส่ค่าสีตรงๆ ในฟังก์ชันวาด
// =====================================================================
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct Theme {
    uint16_t bg, surf, line, track, muted, text, accent, ok, warn, bad, bar, knob, badSoft, alertA, alertB, alertFg, alertBtnBg, alertBtnFg;
    // ส่วนเพิ่มจาก README_v3 (สีที่ดีไซน์ใช้แต่ไม่อยู่ในตาราง token)
    uint16_t onAccent;   // ข้อความบนพื้น accent: ป้าย MIL ON, ปุ่ม CONNECT
    uint16_t onBad;      // ข้อความบนพื้น bad: ป้ายจำนวนบนแท็บ FAULT
    uint16_t rpmNormal;  // สีตัวเลข RPM ก่อนถึง redline (Dark = accent, Light = text)
    uint16_t cardLine;   // เส้นขอบการ์ด 1px
    uint16_t shiftOff;   // v4: ช่อง shift light ที่ยังไม่ติด
    uint16_t accentSoft; // v4: พื้นป้าย READY ของ 0-100 ตอนกะพริบ
    bool cardOutline;    // Light มีเส้นขอบการ์ด, Dark ไม่มี
    const char *name;
};

constexpr Theme THEME_DARK = {
    rgb565(0x00, 0x00, 0x00),  // bg
    rgb565(0x14, 0x17, 0x1B),  // surf
    rgb565(0x2A, 0x2F, 0x36),  // line
    rgb565(0x22, 0x26, 0x2C),  // track
    rgb565(0x9A, 0xA0, 0xA8),  // muted
    rgb565(0xF2, 0xF2, 0xEE),  // text
    rgb565(0xFF, 0xB0, 0x00),  // accent
    rgb565(0x3D, 0xDC, 0x97),  // ok
    rgb565(0xFF, 0xB0, 0x00),  // warn
    rgb565(0xFF, 0x4D, 0x3D),  // bad
    rgb565(0x0A, 0x0B, 0x0D),  // bar (พื้นแถบแท็บ)
    rgb565(0xF2, 0xF2, 0xEE),  // knob
    rgb565(0x3A, 0x12, 0x10),  // badSoft (แถบเติมของปุ่มกดค้าง)
    rgb565(0xFF, 0x4D, 0x3D),  // alertA
    rgb565(0xD9, 0x3A, 0x2B),  // alertB
    rgb565(0x00, 0x00, 0x00),  // alertFg
    rgb565(0x00, 0x00, 0x00),  // alertBtnBg
    rgb565(0xF2, 0xF2, 0xEE),  // alertBtnFg
    rgb565(0x00, 0x00, 0x00),  // onAccent
    rgb565(0xFF, 0xFF, 0xFF),  // onBad
    rgb565(0xFF, 0xB0, 0x00),  // rpmNormal = accent
    rgb565(0x2A, 0x2F, 0x36),  // cardLine (ไม่ใช้)
    rgb565(0x1A, 0x1D, 0x22),  // shiftOff
    rgb565(0x3A, 0x2A, 0x00),  // accentSoft
    false,
    "DARK",
};

constexpr Theme THEME_LIGHT = {
    rgb565(0xEC, 0xEA, 0xE5),  // bg
    rgb565(0xFF, 0xFF, 0xFF),  // surf
    rgb565(0xD6, 0xD3, 0xCC),  // line
    rgb565(0xE2, 0xDF, 0xD8),  // track
    rgb565(0x5C, 0x62, 0x6A),  // muted
    rgb565(0x12, 0x14, 0x18),  // text
    rgb565(0xF5, 0xA4, 0x00),  // accent
    rgb565(0x0E, 0x8A, 0x57),  // ok
    rgb565(0xA8, 0x68, 0x00),  // warn
    rgb565(0xD2, 0x28, 0x1D),  // bad
    rgb565(0xFF, 0xFF, 0xFF),  // bar
    rgb565(0xFF, 0xFF, 0xFF),  // knob
    rgb565(0xFB, 0xE0, 0xDD),  // badSoft
    rgb565(0xD2, 0x28, 0x1D),  // alertA
    rgb565(0xB0, 0x1F, 0x16),  // alertB
    rgb565(0xFF, 0xFF, 0xFF),  // alertFg
    rgb565(0xFF, 0xFF, 0xFF),  // alertBtnBg
    rgb565(0xB0, 0x1F, 0x16),  // alertBtnFg
    rgb565(0x00, 0x00, 0x00),  // onAccent
    rgb565(0xFF, 0xFF, 0xFF),  // onBad
    rgb565(0x12, 0x14, 0x18),  // rpmNormal = text
    rgb565(0xDE, 0xDB, 0xD4),  // cardLine
    rgb565(0xDC, 0xD9, 0xD2),  // shiftOff
    rgb565(0xFC, 0xEB, 0xC4),  // accentSoft
    true,
    "LIGHT",
};

// ตรวจค่า RGB565 กับตารางใน README_v3 (สูตรเดียวกับ Smart Gauge CYD v3.dc.html)
static_assert(THEME_LIGHT.bg == 0xEF5C && THEME_LIGHT.line == 0xD699 && THEME_LIGHT.muted == 0x5B0D && THEME_LIGHT.ok == 0x0C4A &&
                  THEME_LIGHT.warn == 0xAB40 && THEME_LIGHT.bad == 0xD143 && THEME_LIGHT.badSoft == 0xFF1B,
              "light theme RGB565 values differ from README_v3");
static_assert(THEME_DARK.track == 0x2125 && THEME_DARK.muted == 0x9D15 && THEME_DARK.bar == 0x0841 && THEME_DARK.badSoft == 0x3882,
              "dark theme RGB565 values differ from README_v3");

enum class ThemeMode : uint8_t { Dark, Light, Auto };  // ค่าที่บันทึกใน NVS (key "theme")
constexpr const char *THEME_MODE_NAMES[3] = {"DARK", "LIGHT", "AUTO"};

const Theme *T = &THEME_DARK;  // ธีมที่ใช้วาดอยู่ตอนนี้

constexpr int16_t SCREEN_W = 320;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t STATUS_H = 22;  // แถบสถานะ y 0-21 เส้นล่างที่ y 22 (v4)

struct Rect {
    int16_t x, y, w, h;
};

constexpr bool contains(const Rect &r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ---- แท็บล่าง 5 ช่อง: เส้นบนที่ y 207, สูง 32, ทั้งช่องคือพื้นที่แตะ ----
constexpr int16_t TAB_LINE_Y = 207;
constexpr int TAB_COUNT = 5;
constexpr Rect tabRect(int i)
{
    return {static_cast<int16_t>(i * 64), 208, 64, 32};
}
constexpr const char *TAB_NAMES[TAB_COUNT] = {"DRIVE", "FOCUS", "TRIP", "FAULT", ""};  // ช่องสุดท้าย = ไอคอนเฟือง (SET)
constexpr int16_t CONTENT_TOP = STATUS_H + 2;  // พื้นที่เนื้อหา (ปัดเปลี่ยนหน้าได้) y 24-206
constexpr int16_t CONTENT_BOTTOM = TAB_LINE_Y;

// ---- หน้า DRIVE (ดีไซน์ OBD2 Smart Gauge: มาตรวัดซ้าย + การ์ดค่า 3 ใบขวา) ----
constexpr Rect SHIFT_BAR = {6, 26, 308, 5};
constexpr int SHIFT_SEGS = 12;
constexpr int GAUGE_CX = 160;         // กึ่งกลางหัวข้อของหน้า FOCUS
constexpr int DRV_CX = 90;            // จุดศูนย์กลางมาตรวัดหน้า DRIVE
constexpr int DRV_CY = 122;
constexpr int GAUGE_R_OUT = 79;       // วงโค้งหลัก หนา 12 (r 67-79)
constexpr int GAUGE_R_IN = 67;
constexpr int GAUGE_START_DEG = 45;   // มุมของ drawArc (0 = ทิศ 6 นาฬิกา ตามเข็ม) = 135° ของ SVG
constexpr int GAUGE_SWEEP_DEG = 270;
constexpr Rect GAUGE_HIT = {6, 34, 168, 172};
// พื้นที่ล้างตัวเลข (อยู่ในวงใน ไม่ลบเส้นโค้ง; ขีดและตัวเลขหน้าปัดวาดซ้ำหลังล้าง)
constexpr int SPEED_BASE_Y = DRV_CY + 24;
constexpr Rect SPEED_BOX = {DRV_CX - 46, SPEED_BASE_Y - 42, 92, 44};
constexpr Rect RPM_BOX = {DRV_CX - 64, 182, 128, 20};
constexpr int HOLD_RING_R = 52;       // วงแหวนความคืบหน้าตอนกดค้างมาตรวัด (อยู่ระหว่างตัวเลขหน้าปัดกับขีด)
constexpr int TILE_COUNT = 3;
constexpr int TILE_W = 132;
constexpr int TILE_H = 53;

constexpr Rect tileRect(int i)  // การ์ดเรียงลงมาด้านขวา
{
    return {182, static_cast<int16_t>(35 + i * 57), TILE_W, TILE_H};
}

// ---- หน้า FOCUS ----
constexpr Rect FOCUS_PREV_BTN = {6, 28, 36, 24};
constexpr Rect FOCUS_NEXT_BTN = {278, 28, 36, 24};
constexpr Rect FOCUS_VALUE_BOX = {6, 58, 194, 66};
constexpr Rect FOCUS_STATS = {204, 60, 110, 48};
constexpr Rect FOCUS_GRAPH = {6, 126, 308, 52};
constexpr Rect FOCUS_RESET_BTN = {214, 182, 100, 22};
constexpr int HISTORY_LEN = 60;  // กราฟ 60 วินาที จุดละ 1 วินาที

// ---- หน้า TRIP ----
constexpr Rect tripCardRect(int i)
{
    return {static_cast<int16_t>(6 + (i % 2) * 97), static_cast<int16_t>(28 + (i / 2) * 50), 93, 46};
}
constexpr Rect TRIP_RESET_BTN = {6, 180, 190, 24};
constexpr Rect ZERO_CARD = {200, 28, 114, 176};
constexpr Rect ZERO_ARM_BTN = {208, 164, 98, 32};

// ---- หน้า FAULT ----
constexpr int DTC_ROWS = 3;
constexpr int DTC_ROW_Y = 53;
constexpr int DTC_ROW_PITCH = 37;  // สูง 34 + ช่องว่าง 3
constexpr Rect READ_BTN = {6, 170, 86, 34};
constexpr Rect CLEAR_BTN = {98, 170, 216, 34};
constexpr Rect PREV_BTN = {238, 28, 24, 22};  // ตัวเปลี่ยนหน้า แสดงเมื่อมีรหัสเกิน 3
constexpr Rect NEXT_BTN = {288, 28, 24, 22};

// ---- หน้า SET: รายการเลื่อนได้ แถวละ 31 ในพื้นที่ y 24-206 ----
constexpr Rect SET_AREA = {0, 24, 320, 183};
constexpr int SET_ROW_H = 31;
enum SetRow : uint8_t { SR_ADAPTER, SR_PIN, SR_CONNECT, SR_THEME, SR_BACKLIGHT, SR_SOUND, SR_SHIFT, SR_COOL,
                        SR_INVERT, SR_SWAPRB, SR_FLIP, SR_RECAL, SR_FW, SR_COUNT };
constexpr int SET_CONTENT_H = SR_COUNT * SET_ROW_H + 8;
constexpr int SET_MAX_SCROLL = SET_CONTENT_H - SET_AREA.h > 0 ? SET_CONTENT_H - SET_AREA.h : 0;
constexpr int THEME_SEG_W = 44;   // ตัวเลือก DARK / LIGHT / AUTO
constexpr int SOUND_SEG_W = 38;   // ตัวเลือก OFF / LOW / MID / HIGH
constexpr int BL_TRACK_X = 92;
constexpr int BL_TRACK_W = 172;

// ---- หน้า SCAN: รายการอุปกรณ์ 5 แถว + แถบปุ่มล่าง (แทนแท็บ) ----
constexpr int16_t SCAN_BAR_Y = 203;
constexpr int SCAN_ROWS = 5;
constexpr int SCAN_ROW_Y = 31;
constexpr int SCAN_ROW_PITCH = 34;  // สูง 32 + ช่องว่าง 2
constexpr Rect SCAN_LIST = {0, 26, 320, 176};
constexpr Rect SCAN_BACK_BTN = {4, 205, 64, 32};
constexpr Rect SCAN_PREV_BTN = {72, 205, 36, 32};
constexpr Rect SCAN_NEXT_BTN = {112, 205, 36, 32};
constexpr Rect SCAN_MANUAL_BTN = {152, 205, 76, 32};
constexpr Rect SCAN_RESCAN_BTN = {232, 205, 84, 32};

constexpr Rect scanRowRect(int k)
{
    return {6, static_cast<int16_t>(SCAN_ROW_Y + k * SCAN_ROW_PITCH), 308, 32};
}

// ---- แถบเตือน (toast) ----
constexpr Rect TOAST_RECT = {6, 32, 308, 23};

// ---- คีย์บอร์ดบนจอ ----
constexpr Rect KB_TEXT_BOX = {4, 32, 312, 26};
constexpr int KEY_COUNT = 44;
constexpr int KEY_SHIFT = 39, KEY_SPACE = 40, KEY_DEL = 41, KEY_ESC = 42, KEY_OK = 43;
constexpr char KEY_CHARS[] = "1234567890qwertyuiopasdfghjklzxcvbnm-_.";  // 39 ตัวอักษร (ดัชนี 0-38)

constexpr Rect keyRect(int i)
{
    if (i < 10) return {static_cast<int16_t>(1 + i * 32), 62, 30, 32};
    if (i < 20) return {static_cast<int16_t>(1 + (i - 10) * 32), 96, 30, 32};
    if (i < 29) return {static_cast<int16_t>(17 + (i - 20) * 32), 130, 30, 32};
    if (i < 39) return {static_cast<int16_t>(1 + (i - 29) * 32), 164, 30, 32};
    if (i == KEY_SHIFT) return {4, 200, 56, 36};
    if (i == KEY_SPACE) return {64, 200, 72, 36};
    if (i == KEY_DEL) return {140, 200, 52, 36};
    if (i == KEY_ESC) return {196, 200, 52, 36};
    return {252, 200, 64, 36};  // OK
}

// =====================================================================
//  ชนิดข้อมูล
// =====================================================================
// ลำดับ 5 หน้าแรกตรงกับแท็บ
enum class Page : uint8_t { Drive, Focus, Trip, Faults, Config, Keyboard, Calibrate, Scan };
constexpr int SWIPE_PAGE_COUNT = 4;  // ปัดวนได้ใน DRIVE / FOCUS / TRIP / FAULT

constexpr bool hasTabs(Page p)
{
    return static_cast<int>(p) < TAB_COUNT;
}

constexpr uint8_t KEY_BASE = 100;  // ค่า Widget ตั้งแต่ 100 ขึ้นไป = ปุ่มบนคีย์บอร์ด (KEY_BASE + ดัชนีปุ่ม)
enum class Widget : uint8_t {
    None,
    TabDrive, TabFocus, TabTrip, TabFault, TabSet,     // ลำดับตรงกับ Page
    Tile0, Tile1, Tile2, Tile3, Gauge,                 // หน้า DRIVE
    FocusPrev, FocusNext, FocusReset,                  // หน้า FOCUS
    TripReset, ZeroArm,                                // หน้า TRIP
    BtnRead, BtnClear, BtnPrev, BtnNext,               // หน้า FAULT
    FieldName, FieldPin, BtnConnect, BtnRecal, BlSlider,  // หน้า SET
    ThemeDark, ThemeLight, ThemeAuto,                  // ลำดับตรงกับ ThemeMode
    Sound0, Sound1, Sound2, Sound3,                    // ความดัง OFF / LOW / MID / HIGH
    ShiftMinus, ShiftPlus, CoolMinus, CoolPlus,
    DispInvert, DispSwapRB, DispFlip,                  // แก้การแสดงผลของจอแต่ละรุ่น (แตะทั้งแถว = สลับ)
    ScanRow0, ScanRow1, ScanRow2, ScanRow3, ScanRow4,  // แถวรายการอุปกรณ์ในหน้า SCAN (ลำดับต่อกัน)
    ScanPrev, ScanNext, ScanBack, ScanManual, ScanRescan,
    Toast
};

constexpr Widget operator+(Widget w, int n) { return static_cast<Widget>(static_cast<int>(w) + n); }
constexpr int operator-(Widget a, Widget b) { return static_cast<int>(a) - static_cast<int>(b); }
constexpr bool isKey(Widget w) { return static_cast<uint8_t>(w) >= KEY_BASE; }
constexpr int keyIndex(Widget w) { return static_cast<int>(static_cast<uint8_t>(w)) - KEY_BASE; }
constexpr Widget keyWidget(int i) { return static_cast<Widget>(KEY_BASE + i); }

// ---- OBD ----
enum class LinkState : uint8_t { NoConfig, BtStart, Connecting, Backoff, Init, Online, Scanning };

// ---- ค้นหาอุปกรณ์ Bluetooth (หน้า SCAN) ----
enum class ScanState : uint8_t { Idle, Waiting, Inquiry, Names, Done, Error };
struct ScanDevice {
    uint8_t mac[6];
    char name[33];  // "" = ยังไม่รู้ชื่อ
    int8_t rssi;    // dBm (ไม่ทราบ = -127)
    uint32_t cod;   // Class of Device
    bool obd;       // ชื่อเข้าข่ายอะแดปเตอร์ OBD (ELM327, OBDII, Vgate, ...)
};

enum PidIdx : uint8_t { P_RPM, P_SPEED, P_COOL, P_THR, P_LOAD, P_IAT, P_FUEL, P_VOLT, P_COUNT };

struct PidDef {
    const char *cmd;     // คำสั่งที่ส่งให้ ELM327
    const char *prefix;  // ส่วนหัวของคำตอบ (Mode 01 + 0x40)
    uint8_t bytes;       // จำนวนไบต์ข้อมูลที่ต้องอ่าน
};
constexpr PidDef PID_DEFS[P_COUNT] = {
    {"010C", "410C", 2},  // RPM      = (256*A + B) / 4
    {"010D", "410D", 1},  // SPEED    = A km/h
    {"0105", "4105", 1},  // COOLANT  = A - 40
    {"0111", "4111", 1},  // THROTTLE = A * 100 / 255
    {"0104", "4104", 1},  // LOAD     = A * 100 / 255
    {"010F", "410F", 1},  // INTAKE   = A - 40
    {"012F", "412F", 1},  // FUEL     = A * 100 / 255
    {"ATRV", "", 0},      // แรงดันแบตเตอรี่ (อะแดปเตอร์วัดเอง ไม่ต้องพึ่ง ECU)
};
// ลำดับการถามค่า: RPM ถามบ่อยสุด (ทุกรอบ) ค่าที่เปลี่ยนช้าถามสลับกัน
constexpr uint8_t POLL_SEQUENCE[] = {P_RPM, P_SPEED, P_RPM, P_THR, P_RPM, P_LOAD, P_RPM, P_COOL, P_RPM, P_IAT, P_RPM, P_FUEL, P_RPM, P_VOLT};

struct PidValue {
    volatile float value = NAN;
    volatile uint32_t at = 0;  // เวลา (millis) ที่ได้ค่าล่าสุด 0 = ยังไม่เคยได้
};

enum class DtcState : uint8_t { Idle, Busy, Done, Error };
enum class DtcMsg : uint8_t { None, Cleared, NoEcu, ClearFailed, Timeout, NoLink };
struct DtcData {
    DtcState state = DtcState::Idle;
    DtcMsg msg = DtcMsg::None;
    bool mil = false;
    uint8_t count = 0;
    char codes[app::MAX_DTC][6] = {};
};

enum class DtcRequest : uint8_t { None, Read, Clear };

struct Rect2View {  // ค่าที่ใช้แสดงผล (ค่าจริงจากรถ)
    float v[P_COUNT];
    bool na[P_COUNT];  // ECU บอกว่าไม่รองรับค่านี้ (แสดง N/A) ใช้เฉพาะตอนแสดงค่าจริง
    bool real;
};

enum class PidSupport : uint8_t { Unknown, Yes, No };

// ---- จอสัมผัส ----
struct TouchCalibration {
    bool valid = false;
    bool swapXY = false;
    float xa = 0.0f, xb = 0.0f;
    float ya = 0.0f, yb = 0.0f;
};

struct RawTouch {
    int16_t x = 0, y = 0;
    int16_t z = 0;
};

struct TouchState {
    bool down = false;
    bool pressEdge = false;
    bool releaseEdge = false;
    bool touchingNow = false;
    int16_t rawX = 0, rawY = 0, z = 0;
    int x = 0, y = 0;
    int lastX = 0, lastY = 0;
    uint32_t downSince = 0;
    uint8_t pressCount = 0, releaseCount = 0;
    bool smoothReady = false;
};

// =====================================================================
//  ตัวแปรร่วม
// =====================================================================
TFT_eSPI tft;
SPIClass touchSpi(VSPI);
Preferences prefs;
BluetoothSerial SerialBT;

// ---- ค่าที่แชร์ระหว่าง OBD task (core 0) กับ UI (loop) ----
volatile LinkState link = LinkState::NoConfig;
volatile bool reconnectReq = false;
volatile DtcRequest dtcReq = DtcRequest::None;
volatile uint32_t lastGoodAt = 0;  // เวลาที่ได้ค่าจริงล่าสุด
PidValue pids[P_COUNT];
DtcData g_dtc;
portMUX_TYPE dtcMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE cfgMux = portMUX_INITIALIZER_UNLOCKED;
char cfgName[33] = {0};
char cfgPin[17] = {0};
char cfgMac[18] = {0};  // MAC ของอะแดปเตอร์ที่เลือกจากหน้า SCAN ("" = เชื่อมต่อด้วยชื่อ)

// ---- ผลการค้นหา Bluetooth: GAP callback (task ของ Bluetooth) เขียน, หน้า SCAN อ่าน ----
ScanDevice g_scanDev[app::MAX_SCAN_DEVICES];
uint8_t g_scanCount = 0;
volatile uint16_t scanVersion = 0;  // เพิ่มทุกครั้งที่รายการเปลี่ยน (หน้าจอวาดใหม่เมื่อค่าเปลี่ยน)
volatile ScanState scanState = ScanState::Idle;
volatile bool scanHold = false;  // เปิดหน้า SCAN อยู่: OBD task หยุดเชื่อมต่อ
volatile bool scanReq = false;   // ขอเริ่มค้นหารอบใหม่
volatile uint32_t scanStartedAt = 0;
portMUX_TYPE scanMux = portMUX_INITIALIZER_UNLOCKED;

// ---- UI ----
Page page = Page::Drive;
TouchCalibration cal;
TouchState touch;
uint32_t lastTouchPollAt = 0;
Widget pressTarget = Widget::None;
Widget pressedWidget = Widget::None;
bool longPressDone = false;
int drawnLongPress = -1;
int backlightPercent = app::BACKLIGHT_DEFAULT_PERCENT;
int downX = 0, downY = 0;          // จุดที่เริ่มแตะ (ใช้ตรวจการปัด / ลาก)
bool dragging = false;             // หน้า SET: กำลังลากเลื่อนรายการ
int dragStartScroll = 0;
int blBeforeDrag = 0;              // ความสว่างก่อนแตะแถบเลื่อน (คืนค่าถ้ากลายเป็นการลากเลื่อน)

int dtcListPage = 0;

// ---- ค่าที่ปรับได้ในหน้า SET (บันทึกใน NVS) ----
int coolLimit = app::COOLANT_LIMIT_C;   // key "coollim"
int shiftRpm = app::SHIFT_RPM_DEFAULT;  // key "shift"

// ---- แก้การแสดงผลของจอแต่ละรุ่น (หน้า SET → Display) จำแยกตามชิปจอ ----
//   bit0 = กลับสี (INVON), bit1 = สลับแดง/น้ำเงิน (MADCTL BGR), bit2 = หมุน 180° (rotation 3 แทน 1)
constexpr uint8_t DISP_INVERT = 1, DISP_SWAP_RB = 2, DISP_FLIP = 4;
#if defined(ST7789_DRIVER)
inline constexpr char DISPLAY_CHIP[] = "ST7789";
inline constexpr char DISP_NVS_KEY[] = "dsp7789";
#else
inline constexpr char DISPLAY_CHIP[] = "ILI9341";
inline constexpr char DISP_NVS_KEY[] = "dsp9341";
#endif
uint8_t dispFix = 0;

// ---- สถิติ / กราฟ / trip: คำนวณใน loop() จากค่าจริง ไม่เก็บถาวร ----
struct PidStats {
    float min = NAN, max = NAN;
    double sum = 0;
    uint32_t n = 0;
};
PidStats stats[P_COUNT];              // ลำดับตาม PidIdx
float history[P_COUNT][HISTORY_LEN];        // กราฟ 60 วินาที (ring buffer)
int historyCount = 0;                 // จำนวนจุดที่มี (ไม่เกิน HISTORY_LEN)
int historyHead = 0;                  // ตำแหน่งที่จะเขียนจุดถัดไป
uint32_t historyVersion = 0;          // เพิ่มทุกครั้งที่เพิ่มจุด (หน้า FOCUS วาดกราฟใหม่)
uint32_t lastHistoryAt = 0;
uint32_t lastStatsAt = 0;
float peakRpm = 0;                    // รอบสูงสุดตั้งแต่ล้างครั้งล่าสุด (เครื่องหมายบนมาตรวัด)
int focusPid = 0;                     // หน้า FOCUS แสดงค่าไหน (ดัชนีใน FOCUS_ORDER)

struct TripData {
    float timeS = 0, movingS = 0, distKm = 0, maxSpeed = 0, maxRpm = 0;
};
TripData trip;

enum class ZeroState : uint8_t { Off, Armed, Ready, Run, Done };  // จับเวลา 0-100 km/h
ZeroState zeroState = ZeroState::Off;
uint32_t zeroStartAt = 0;
float zeroResult = NAN, zeroBest = NAN;  // วินาที (best จำเฉพาะรอบที่เปิดเครื่อง)

// ---- แถบเตือน (toast) ----
enum class ToastKind : uint8_t { Ok, Warn, Bad };
struct ToastData {
    bool active = false;
    ToastKind kind = ToastKind::Ok;
    char text[24] = "";
    char sub[16] = "";
    uint32_t at = 0;
};
ToastData toast;
bool toastDirty = false;  // เนื้อหาใต้ toast ถูกวาดใหม่: ต้องวาด toast ทับอีกครั้ง

// ---- หน้า SET ----
int setScroll = 0;  // ระยะเลื่อนรายการ 0..SET_MAX_SCROLL

// คีย์บอร์ด
int kbField = 0;  // 0 = ชื่อ, 1 = PIN
char kbBuf[34] = {0};
bool kbShift = true;

// calibrate
int calStep = 0;
int calAccepted = 0;
int calSamples = 0;
long calSumX = 0, calSumY = 0;
bool calWaitRelease = false;
int calRawX[4] = {0, 0, 0, 0};
int calRawY[4] = {0, 0, 0, 0};
uint32_t calMessageUntil = 0;
bool calResultOk = false;

// สิ่งที่วาดบนจอไปล่าสุด (วาดเฉพาะส่วนที่เปลี่ยน)
constexpr int NEVER = -32768;
char netLabel[24] = "";  // IP สำหรับเปิดหน้าเว็บ ที่แถบสถานะแสดง (ส่วนเครือข่ายเขียน) "" = ยังไม่มี IP
int drawnStatus = NEVER;
bool drawnConn = false;
int drawnFaultBadge = NEVER;
int drawnSpeed = NEVER;
int drawnRpmText = NEVER;
int drawnSweep = NEVER;
int drawnArcColor = NEVER;
int drawnPeak = NEVER;
int drawnShift = NEVER;
int drawnTile[TILE_COUNT] = {NEVER, NEVER, NEVER};
int drawnDtcKey = NEVER;
int drawnFocusKey = NEVER;
int drawnFocusStats = NEVER;
uint32_t drawnFocusGraph = 0xFFFFFFFF;
int drawnTripKey = NEVER;
int drawnZeroKey = NEVER;
int drawnSetKey = NEVER;
int drawnToast = NEVER;

uint32_t lastDisplayAt = 0;
uint32_t lastStatusAt = 0;
uint32_t lastShiftDrawAt = 0;

// ---- หน้า ALERT (น้ำหล่อเย็นร้อนเกิน) ----
bool alertActive = false;
bool alertArmed = true;  // พร้อมเตือน: กลับเป็น true เมื่ออุณหภูมิลดต่ำกว่า (เกณฑ์ - COOLANT_ALERT_HYST_C)
Page alertReturnPage = Page::Drive;
int drawnAlert = NEVER;

// ---- ธีม: โหมดที่เลือก (บันทึกใน NVS) และความสว่างจอแยกตามธีม ----
ThemeMode themeMode = ThemeMode::Dark;
bool autoIsLight = false;  // ผลการเลือกของโหมด AUTO ล่าสุด
uint32_t lastAutoThemeAt = 0;
int backlightDark = app::BACKLIGHT_DARK_DEFAULT;
int backlightLight = app::BACKLIGHT_LIGHT_DEFAULT;

// =====================================================================
//  ตัวช่วยวาด
//  ข้อความในหน้าหลักใช้ฟอนต์ Chakra Petch แบบ smooth (include/ui_fonts.h) วางตามเส้นฐาน (baseline)
//  หน้า calibrate ยังใช้ฟอนต์ในตัวของ TFT_eSPI (drawText)
// =====================================================================
const uint8_t *loadedFont = nullptr;

void useFont(const uint8_t *font)
{
    if (font == loadedFont) return;
    tft.loadFont(font);  // loadFont ปลดฟอนต์เดิมให้เอง
    loadedFont = font;
}

void useBuiltinFont()
{
    if (loadedFont == nullptr) return;
    tft.unloadFont();
    loadedFont = nullptr;
}

int16_t textWidthIn(const uint8_t *font, const char *text)
{
    useFont(font);
    return static_cast<int16_t>(tft.textWidth(text));
}

// วาดข้อความโดย (x, baselineY) คือจุดบนเส้นฐาน; datum = L_BASELINE / C_BASELINE / R_BASELINE
// bg ต้องเป็นสีพื้นจริงใต้ข้อความ (ใช้ผสมขอบตัวอักษรแบบ anti-aliased)
int16_t textAt(const uint8_t *font, const char *text, int x, int baselineY, uint8_t datum, uint16_t fg, uint16_t bg)
{
    useFont(font);
    tft.setTextColor(fg, bg);
    tft.setTextDatum(datum);
    const int16_t w = tft.drawString(text, x, baselineY);
    tft.setTextDatum(TL_DATUM);
    return w;
}

// ตัดข้อความให้กว้างไม่เกิน maxW พิกเซล ต่อท้ายด้วย "..." (ตัดทีละตัวอักษร UTF-8 จึงใช้กับภาษาไทยได้)
void fitText(const uint8_t *font, const char *in, int maxW, char *out, size_t size)
{
    strlcpy(out, in, size);
    useFont(font);
    if (tft.textWidth(out) <= maxW) return;
    const int dotsW = tft.textWidth("...");
    size_t n = strlen(out);
    while (n > 0) {
        do {
            n--;
        } while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xC0) == 0x80);
        out[n] = '\0';
        if (tft.textWidth(out) + dotsW <= maxW) break;
    }
    strlcat(out, "...", size);
}

// ตัวเลขมีตัวคั่นหลักพัน เช่น 4360 -> "4,360"
void formatThousands(int value, char *out, size_t size)
{
    if (value < 1000) {
        snprintf(out, size, "%d", value);
    } else {
        snprintf(out, size, "%d,%03d", value / 1000, value % 1000);
    }
}

// ฟอนต์ในตัว (ใช้ในหน้า calibrate)
void drawText(const char *text, int16_t x, int16_t y, uint8_t font, uint16_t fg, uint16_t bg, uint16_t paddingW = 0)
{
    useBuiltinFont();
    tft.setTextColor(fg, bg);
    tft.setTextPadding(paddingW);
    tft.drawString(text, x, y, font);
    tft.setTextPadding(0);
}

// การ์ดพื้น surf มุมมน 4 (ธีม Light มีเส้นขอบ 1px)
void drawCardBox(const Rect &r)
{
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, T->surf);
    if (T->cardOutline) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, T->cardLine);
}

// แถบระดับ 4px: ราง track + ส่วนเติมยาวตาม % (อย่างน้อย 4%)
void drawLevelBar(int x, int y, int w, float fraction, uint16_t color)
{
    tft.fillRoundRect(x, y, w, 4, 2, T->track);
    if (isnan(fraction)) return;
    const int fw = max(2, static_cast<int>(lroundf(constrain(fraction, 0.04f, 1.0f) * w)));
    tft.fillRoundRect(x, y, fw, 4, 2, color);
}

// ปุ่มที่ต้องกดค้าง (CLEAR, RECAL, RESET): ข้อความ + hint ("HOLD 2s" หรือ "" = ไม่มี) และแถบเติมซ้าย->ขวาตามเวลาที่กด
void drawHoldButton(const Rect &r, const char *label, uint16_t fill, uint16_t border, uint8_t borderPx, uint16_t textColor, uint16_t holdColor,
                    int progressPercent, const char *hint = "HOLD 2s")
{
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, fill);
    if (progressPercent > 0) {
        const int16_t w = static_cast<int16_t>((r.w - 2 * borderPx) * progressPercent / 100);
        tft.fillRect(r.x + borderPx, r.y + borderPx, w, r.h - 2 * borderPx, holdColor);
    }
    for (uint8_t i = 0; i < borderPx; i++) tft.drawRoundRect(r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i, 4 - i, border);
    const uint16_t textBg = progressPercent >= 50 ? holdColor : fill;  // แถบเติมผ่านกลางปุ่มแล้ว
    const int16_t lw = textWidthIn(FONT_12B, label);
    const int16_t hw = hint[0] != '\0' ? textWidthIn(FONT_10S, hint) + 6 : 0;
    const int16_t x0 = r.x + (r.w - (lw + hw)) / 2;
    const int16_t base = r.y + r.h / 2 + 4;
    textAt(FONT_12B, label, x0, base, L_BASELINE, textColor, textBg);
    if (hw > 0) textAt(FONT_10S, hint, x0 + lw + 6, base - 1, L_BASELINE, T->muted, textBg);
}

// ---- ความสว่างจอ: แยกค่าของธีม Dark / Light (ค่าแนะนำ 70% / 95% ผู้ใช้ปรับเองได้) ----
void saveBacklight()
{
    if (T == &THEME_LIGHT) {
        backlightLight = backlightPercent;
        prefs.putUChar("bl_l", static_cast<uint8_t>(backlightPercent));
    } else {
        backlightDark = backlightPercent;
        prefs.putUChar("bl_d", static_cast<uint8_t>(backlightPercent));
    }
    Serial.printf("[UI] backlight %d%% saved for %s theme\n", backlightPercent, T->name);
}

// =====================================================================
//  ค่าที่ตั้งไว้ (ชื่อ/PIN) และความสว่างจอ
// =====================================================================
void getConfig(char *name, size_t nameSize, char *pin, size_t pinSize)
{
    portENTER_CRITICAL(&cfgMux);
    strlcpy(name, cfgName, nameSize);
    strlcpy(pin, cfgPin, pinSize);
    portEXIT_CRITICAL(&cfgMux);
}

// เปลี่ยนชื่อ (หน้าเว็บ) = ลืม MAC ที่เลือกจากหน้า SCAN; เปลี่ยนแค่ PIN = MAC เดิมยังใช้ได้ (MANUAL ใช้ setAdapter(name, ""))
void setConfig(const char *name, const char *pin)
{
    portENTER_CRITICAL(&cfgMux);
    if (strcmp(name, cfgName) != 0) cfgMac[0] = '\0';
    strlcpy(cfgName, name, sizeof(cfgName));
    strlcpy(cfgPin, pin, sizeof(cfgPin));
    portEXIT_CRITICAL(&cfgMux);
}

// อะแดปเตอร์ที่เลือกจากหน้า SCAN: ชื่อไว้แสดง, MAC ไว้เชื่อมต่อ (เร็วกว่าเชื่อมด้วยชื่อมาก ไม่ต้องค้นหาก่อน)
void setAdapter(const char *name, const char *mac)
{
    portENTER_CRITICAL(&cfgMux);
    strlcpy(cfgName, name, sizeof(cfgName));
    strlcpy(cfgMac, mac, sizeof(cfgMac));
    portEXIT_CRITICAL(&cfgMux);
}

void getConfigMac(char *mac, size_t size)
{
    portENTER_CRITICAL(&cfgMux);
    strlcpy(mac, cfgMac, size);
    portEXIT_CRITICAL(&cfgMux);
}

void applyBacklight(int percent)
{
    backlightPercent = constrain(percent, app::BACKLIGHT_MIN_PERCENT, 100);
    const uint32_t value = (static_cast<uint32_t>(backlightPercent) * app::PWM_MAX_VALUE + 50) / 100;
    ledcWrite(app::BACKLIGHT_CHANNEL, value);
}

void backlightBegin()
{
    // ต้องทำหลัง tft.init() (ไลบรารีจอเปิดไฟหลังจอด้วย digitalWrite ไว้ก่อน)
    ledcSetup(app::BACKLIGHT_CHANNEL, app::PWM_FREQUENCY_HZ, app::PWM_RESOLUTION_BITS);
    ledcAttachPin(cyd::TFT_PIN_BACKLIGHT, app::BACKLIGHT_CHANNEL);
    applyBacklight(backlightPercent);
}

// =====================================================================
//  เสียง: ลำโพงที่ GPIO26 ขับด้วย PWM (LEDC) คลื่นสี่เหลี่ยม
//  playSound() ส่งชื่อเสียงเข้าคิว แล้ว soundTask (core 1) เล่นตามจังหวะด้วย vTaskDelay จึงไม่หน่วง UI
//  เสียงที่สำคัญกว่า (prio มากกว่าหรือเท่ากัน) ตัดเสียงที่กำลังเล่น ส่วนเสียงที่สำคัญน้อยกว่าที่มาระหว่างเล่นจะถูกทิ้ง
// =====================================================================
struct Note {
    uint16_t hz;  // 0 = เงียบ
    uint16_t ms;
};

// ความถี่ตัวโน้ต (Hz): C6 1047, E6 1319, G6 1568, C7 2093 ...
constexpr Note SND_BOOT[] = {{1047, 70}, {0, 20}, {1319, 70}, {0, 20}, {1568, 130}};         // เปิดเครื่อง: ไล่ขึ้น 3 โน้ต
constexpr Note SND_CLICK[] = {{2400, 8}};                                                   // แตะปุ่ม: ติ๊กสั้น
constexpr Note SND_CONFIRM[] = {{1568, 60}, {2093, 90}};                                    // กดค้างครบ / เปลี่ยนความดัง
constexpr Note SND_BT_ON[] = {{784, 80}, {0, 30}, {1175, 130}};                             // ต่ออะแดปเตอร์ได้: ขึ้น
constexpr Note SND_BT_OFF[] = {{1175, 80}, {0, 30}, {587, 180}};                            // หลุด: ลง
constexpr Note SND_DTC_NONE[] = {{1047, 70}, {1568, 120}};                                  // อ่านรหัสแล้ว ไม่มีปัญหา
constexpr Note SND_DTC_FOUND[] = {{880, 110}, {0, 70}, {880, 110}, {0, 70}, {880, 110}};     // พบรหัสข้อผิดพลาด: 3 ครั้ง
constexpr Note SND_DTC_CLEARED[] = {{1047, 60}, {1319, 60}, {1568, 60}, {2093, 130}};       // ลบรหัสสำเร็จ
constexpr Note SND_ERROR[] = {{330, 250}};                                                  // ทำไม่สำเร็จ: เสียงต่ำยาว
constexpr Note SND_REDLINE[] = {{2794, 45}, {0, 35}, {2794, 45}};                           // รอบถึง shift light: ปี๊บสั้นสองครั้ง
constexpr Note SND_SCAN_DONE[] = {{1568, 70}};                                              // ค้นหา Bluetooth เสร็จ
constexpr Note SND_TOAST[] = {{1319, 60}, {0, 40}, {1319, 60}};                             // แถบเตือน (แบต/น้ำมันต่ำ)
constexpr Note SND_ALERT[] = {{1760, 180}, {1319, 180}, {1760, 180}, {1319, 180}};          // น้ำหล่อเย็นร้อนเกิน: สองเสียงสลับ

enum class Sound : uint8_t { None, Boot, Click, Confirm, BtOn, BtOff, DtcNone, DtcFound, DtcCleared, Error, Redline, ScanDone, Toast, Alert, Stop, Count };

struct SoundDef {
    const char *name;
    uint8_t prio;
    const Note *notes;
    uint8_t count;
};
#define SND(name, prio, notes) {name, prio, notes, sizeof(notes) / sizeof(Note)}
const SoundDef SOUND_DEFS[] = {  // ลำดับตรงกับ enum Sound
    {"none", 0, nullptr, 0},
    SND("boot", 2, SND_BOOT),
    SND("click", 1, SND_CLICK),
    SND("confirm", 2, SND_CONFIRM),
    SND("bt-on", 3, SND_BT_ON),
    SND("bt-off", 3, SND_BT_OFF),
    SND("dtc-none", 3, SND_DTC_NONE),
    SND("dtc-found", 3, SND_DTC_FOUND),
    SND("dtc-cleared", 3, SND_DTC_CLEARED),
    SND("error", 3, SND_ERROR),
    SND("redline", 4, SND_REDLINE),
    SND("scan-done", 2, SND_SCAN_DONE),
    SND("toast", 3, SND_TOAST),
    SND("alert", 5, SND_ALERT),
    {"stop", 6, nullptr, 0},  // หยุดเสียงทันที (เช่น แตะรับทราบหน้า ALERT)
};
#undef SND
static_assert(sizeof(SOUND_DEFS) / sizeof(SOUND_DEFS[0]) == static_cast<size_t>(Sound::Count), "SOUND_DEFS must match enum Sound");

constexpr const char *VOLUME_NAMES[4] = {"OFF", "LOW", "MID", "HIGH"};
volatile uint8_t soundVolume = app::VOLUME_DEFAULT;  // 0-3 บันทึกใน NVS (key "vol")
QueueHandle_t soundQueue = nullptr;

void playSound(Sound s)
{
    if (soundQueue == nullptr || (soundVolume == 0 && s != Sound::Stop)) return;
    xQueueSend(soundQueue, &s, 0);  // คิวเต็ม = ทิ้ง (ไม่รอ)
}

void soundTone(uint16_t hz)
{
    if (hz == 0 || soundVolume == 0) {
        ledcWrite(app::AUDIO_CHANNEL, 0);
        return;
    }
    ledcChangeFrequency(app::AUDIO_CHANNEL, hz, app::AUDIO_RESOLUTION_BITS);
    ledcWrite(app::AUDIO_CHANNEL, app::VOLUME_DUTY[soundVolume]);
}

void soundTask(void *)
{
    Sound next = Sound::None;
    for (;;) {
        if (next == Sound::None) xQueueReceive(soundQueue, &next, portMAX_DELAY);
        const Sound cur = next;
        next = Sound::None;
        const SoundDef &def = SOUND_DEFS[static_cast<int>(cur)];
        for (uint8_t i = 0; i < def.count && next == Sound::None; i++) {
            soundTone(def.notes[i].hz);
            // รอจนครบเวลาของโน้ต แต่ถ้ามีเสียงที่สำคัญกว่าหรือเท่ากันเข้ามา ให้ตัดไปเล่นเสียงนั้น
            const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(def.notes[i].ms);
            for (TickType_t t = xTaskGetTickCount(); t < end; t = xTaskGetTickCount()) {
                Sound in;
                if (xQueueReceive(soundQueue, &in, end - t) == pdTRUE && SOUND_DEFS[static_cast<int>(in)].prio >= def.prio) {
                    next = in;
                    break;
                }
            }
        }
        soundTone(0);
    }
}

void setVolume(uint8_t v, bool save)
{
    soundVolume = v % 4;
    if (save) prefs.putUChar("vol", soundVolume);
    Serial.printf("[SOUND] volume %s\n", VOLUME_NAMES[soundVolume]);
}

void soundBegin()
{
    soundVolume = min<uint8_t>(prefs.getUChar("vol", app::VOLUME_DEFAULT), 3);
    ledcSetup(app::AUDIO_CHANNEL, 2000, app::AUDIO_RESOLUTION_BITS);
    ledcAttachPin(cyd::AUDIO_PIN, app::AUDIO_CHANNEL);
    ledcWrite(app::AUDIO_CHANNEL, 0);
    soundQueue = xQueueCreate(6, sizeof(Sound));
    xTaskCreatePinnedToCore(soundTask, "sound", 2048, nullptr, 2, nullptr, 1);  // สูงกว่า loop() (1) จังหวะเสียงจึงตรง ส่วนใหญ่หลับรอคิว
}

// ทดสอบเสียงจาก Serial ("p"): เล่นทีละเสียงตามลำดับ
void playNextDemoSound()
{
    static int i = 0;
    i = i % (static_cast<int>(Sound::Stop) - 1) + 1;  // Boot .. Alert
    Serial.printf("[SOUND] %s (volume %s)\n", SOUND_DEFS[i].name, VOLUME_NAMES[soundVolume]);
    playSound(static_cast<Sound>(i));
}

// =====================================================================
//  OBD: ฟังก์ชันช่วยแปลงข้อความ
// =====================================================================
constexpr size_t RX_MAX = 384;

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// ตัดช่องว่าง/ขึ้นบรรทัด/'>' ออก และแปลงเป็นตัวพิมพ์ใหญ่ เพื่อค้นหาส่วนหัวของคำตอบได้ง่าย
void cleanText(const char *in, char *out, size_t size)
{
    size_t j = 0;
    for (; *in != '\0' && j + 1 < size; in++) {
        const char c = *in;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '>') continue;
        out[j++] = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    out[j] = '\0';
}

bool parseHexBytes(const char *s, int count, uint8_t *out)
{
    for (int i = 0; i < count; i++) {
        const int hi = hexVal(s[2 * i]);
        if (hi < 0) return false;
        const int lo = hexVal(s[2 * i + 1]);
        if (lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// ถอดค่า PID จากคำตอบดิบ คืน false ถ้าไม่ใช่คำตอบที่ถูกต้อง (NO DATA, ผิดรูปแบบ ฯลฯ)
bool decodePid(int idx, const char *rx, float &out)
{
    char c[RX_MAX];
    cleanText(rx, c, sizeof(c));
    if (idx == P_VOLT) {  // ตัวอย่างคำตอบ "12.6V"
        const char *p = c;
        while (*p != '\0' && !(*p >= '0' && *p <= '9')) p++;
        if (*p == '\0') return false;
        const float v = static_cast<float>(atof(p));
        if (v < 3.0f || v > 40.0f) return false;
        out = v;
        return true;
    }
    const PidDef &d = PID_DEFS[idx];
    const char *p = strstr(c, d.prefix);
    if (p == nullptr) return false;
    uint8_t b[2] = {0, 0};
    if (!parseHexBytes(p + 4, d.bytes, b)) return false;
    switch (idx) {
        case P_RPM: out = (256.0f * b[0] + b[1]) / 4.0f; break;
        case P_SPEED: out = b[0]; break;
        case P_COOL:
        case P_IAT: out = static_cast<float>(b[0]) - 40.0f; break;
        case P_THR:
        case P_LOAD:
        case P_FUEL: out = b[0] * 100.0f / 255.0f; break;
        default: return false;
    }
    return true;
}

// ถอดรหัสข้อผิดพลาด 2 ไบต์ เช่น 0x01 0x33 -> "P0133"
void formatDtc(uint8_t a, uint8_t b, char *out)
{
    static const char LETTERS[4] = {'P', 'C', 'B', 'U'};
    snprintf(out, 6, "%c%d%X%02X", LETTERS[(a >> 6) & 3], (a >> 4) & 3, a & 0x0F, b);
}

void addDtc(DtcData &d, uint8_t a, uint8_t b)
{
    if (a == 0 && b == 0) return;
    char code[6];
    formatDtc(a, b, code);
    for (int i = 0; i < d.count; i++) {
        if (strcmp(d.codes[i], code) == 0) return;  // ซ้ำ (ECU หลายตัวตอบเหมือนกัน)
    }
    if (d.count < app::MAX_DTC) {
        strlcpy(d.codes[d.count], code, sizeof(d.codes[0]));
        d.count++;
    }
}

// แยกข้อมูลของคำตอบ Mode 03 หนึ่งสาย: 43 [จำนวน(เฉพาะ CAN)] ตามด้วยรหัสทีละ 2 ไบต์
void decodeDtcStream(const uint8_t *s, int len, bool can, DtcData &d)
{
    if (len < 1 || s[0] != 0x43) return;
    int i = 1;
    if (can) i++;  // ไบต์บอกจำนวนรหัส
    for (; i + 1 < len; i += 2) addDtc(d, s[i], s[i + 1]);
}

// คำตอบ Mode 03 จาก ELM327 (ATH0 + ATS0) อาจเป็นหลายบรรทัด: "0:4306..." "1:..." ต่อกันเป็นสายเดียว
void parseDtcResponse(const char *rx, bool can, DtcData &d)
{
    uint8_t stream[96];
    int sl = 0;
    auto flush = [&]() {
        if (sl > 0) decodeDtcStream(stream, sl, can, d);
        sl = 0;
    };

    char line[120];
    const char *p = rx;
    while (*p != '\0') {
        size_t n = 0;
        while (*p != '\0' && *p != '\r' && *p != '\n') {
            if (*p != ' ' && *p != '>' && n + 1 < sizeof(line)) line[n++] = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
            p++;
        }
        while (*p == '\r' || *p == '\n') p++;
        line[n] = '\0';
        if (n == 0) continue;

        const bool numbered = n >= 2 && line[1] == ':';
        const char *hex = numbered ? line + 2 : line;
        const size_t hexLen = strlen(hex);
        if (hexLen < 2 || (hexLen & 1) != 0) continue;  // บรรทัดบอกความยาว (3 หลัก) หรือข้อความอื่น
        bool allHex = true;
        for (size_t i = 0; i < hexLen; i++) {
            if (hexVal(hex[i]) < 0) allHex = false;
        }
        if (!allHex) continue;

        if (!numbered || line[0] == '0') flush();
        for (size_t i = 0; i + 1 < hexLen && sl < static_cast<int>(sizeof(stream)); i += 2) {
            uint8_t b;
            parseHexBytes(hex + i, 1, &b);
            stream[sl++] = b;
        }
        if (!numbered) flush();
    }
    flush();
}

bool respHas(const char *clean, const char *token)
{
    return strstr(clean, token) != nullptr;
}

bool respIsNoEcu(const char *clean)
{
    return respHas(clean, "NODATA") || respHas(clean, "UNABLETOCONNECT") || respHas(clean, "BUSINIT") || respHas(clean, "CANERROR") ||
           respHas(clean, "BUSBUSY") || respHas(clean, "BUFFERFULL");
}

// "AA:BB:CC:DD:EE:FF" -> 6 ไบต์ (ให้ใส่ MAC แทนชื่อได้)
bool parseMac(const char *s, uint8_t *mac)
{
    if (strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        const int hi = hexVal(s[i * 3]);
        const int lo = hexVal(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-') return false;
        mac[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// =====================================================================
//  OBD state machine (ทำงานใน task แยก)
//
//   LinkState:  NoConfig -> BtStart -> Connecting -> Init -> Online
//                   ^                       |  (ล้มเหลว/หลุด)         |
//                   +------------------ Backoff <----------------------+
//
//   ในสถานะ Init/Online ใช้ "ตัวส่งคำสั่ง" (Job) ส่งทีละคำสั่งแล้วรอคำตอบจนเจอ '>' หรือหมดเวลา
// =====================================================================
enum class Job : uint8_t { None, Init, Poll, DtcStatus, DtcRead, DtcClear, Probe };

struct InitCmd {
    const char *cmd;
    uint32_t timeoutMs;
};
constexpr InitCmd INIT_CMDS[] = {
    {"ATZ", app::CMD_TIMEOUT_INIT_MS},   // รีเซ็ต
    {"ATE0", app::CMD_TIMEOUT_MS},       // ปิด echo
    {"ATL0", app::CMD_TIMEOUT_MS},       // ปิดขึ้นบรรทัดใหม่แบบ LF
    {"ATS0", app::CMD_TIMEOUT_MS},       // ปิดช่องว่างในคำตอบ
    {"ATH0", app::CMD_TIMEOUT_MS},       // ปิด header
    {"ATAT1", app::CMD_TIMEOUT_MS},      // ปรับ timeout อัตโนมัติ
    {"ATSP0", app::CMD_TIMEOUT_MS},      // เลือก protocol อัตโนมัติ
    {"0100", app::CMD_TIMEOUT_PROTO_MS}, // ถาม PID ที่รองรับ = ให้ ELM327 ค้นหา protocol
    {"ATDPN", app::CMD_TIMEOUT_MS},      // ถามเลข protocol (ไว้ดูว่าเป็น CAN หรือไม่)
    {"0120", app::CMD_TIMEOUT_MS},       // ถาม PID 21-40 ที่รองรับ (เช่น 2F ระดับน้ำมัน)
};
constexpr int INIT_COUNT = sizeof(INIT_CMDS) / sizeof(INIT_CMDS[0]);
constexpr int INIT_IDX_PROBE = 7;  // "0100"
constexpr int INIT_IDX_DPN = 8;    // "ATDPN"
constexpr int INIT_IDX_PIDS2 = 9;  // "0120"

struct Engine {
    Job job = Job::None;
    int arg = 0;
    uint32_t sentAt = 0;
    uint32_t timeoutMs = 0;
    char rx[RX_MAX] = {0};
    size_t rxLen = 0;
};
Engine eng;

bool btStarted = false;
uint32_t backoffAt = 0;
uint32_t pauseUntil = 0;
int initIdx = 0;
int initFails = 0;
int pollIdx = 0;
int consecutiveTimeouts = 0;
bool canProtocol = true;
bool ecuSeen = false;

// PID ที่ ECU รองรับ (จากคำตอบ 0100 = PID 01-20, 0120 = PID 21-40) บิตสูงสุด = PID แรกของช่วง
uint32_t pidMask[2] = {0, 0};
bool pidMaskKnown[2] = {false, false};
bool reprobePids = false;  // ตอนเชื่อมต่อ ECU ยังไม่ตอบ (ไม่ได้ติดเครื่อง) แต่ตอนนี้ตอบแล้ว: ถามใหม่
volatile PidSupport pidSupport[P_COUNT] = {};  // UI อ่าน

// หมายเลข PID ของ Mode 01 (0 = ไม่ใช่คำสั่ง Mode 01 เช่น ATRV)
int pidNumber(int idx)
{
    const char *cmd = PID_DEFS[idx].cmd;
    uint8_t b;
    if (cmd[0] != '0' || cmd[1] != '1' || !parseHexBytes(cmd + 2, 1, &b)) return 0;
    return b;
}

void updatePidSupport()
{
    for (int i = 0; i < P_COUNT; i++) {
        const int n = pidNumber(i);
        const int range = (n - 1) / 32;
        if (n == 0) {
            pidSupport[i] = PidSupport::Yes;  // อะแดปเตอร์ตอบเอง
        } else if (range > 1 || !pidMaskKnown[range]) {
            pidSupport[i] = PidSupport::Unknown;
        } else {
            pidSupport[i] = (pidMask[range] >> (31 - (n - 1) % 32)) & 1 ? PidSupport::Yes : PidSupport::No;
        }
    }
}

void resetPidSupport()
{
    pidMask[0] = pidMask[1] = 0;
    pidMaskKnown[0] = pidMaskKnown[1] = false;
    reprobePids = false;
    updatePidSupport();
}

// คำตอบ "41 00 AA BB CC DD" (หรือ "41 20 ...") อาจมาจากหลาย ECU: รวมบิตของทุก ECU
bool parsePidMask(const char *clean, int range, uint32_t &mask)
{
    const char *header = range == 0 ? "4100" : "4120";
    mask = 0;
    bool found = false;
    for (const char *p = strstr(clean, header); p != nullptr; p = strstr(p + 4, header)) {
        uint8_t b[4];
        if (!parseHexBytes(p + 4, 4, b)) continue;
        mask |= (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
        found = true;
    }
    return found;
}

void logPidSupport()
{
    Serial.print("[OBD] supported:");
    for (int i = 0; i < P_COUNT; i++) {
        const PidSupport st = pidSupport[i];
        Serial.printf(" %s=%s", PID_DEFS[i].cmd, st == PidSupport::Yes ? "yes" : (st == PidSupport::No ? "NO" : "?"));
    }
    Serial.println();
}

// ใช้คำตอบของ 0100 / 0120: ได้คำตอบ = รู้แน่, NO DATA ของ 0120 = ไม่มี PID ในช่วงนั้นเลย
void applyPidMaskResponse(int range, const char *clean, bool timedOut)
{
    uint32_t mask = 0;
    if (!timedOut && parsePidMask(clean, range, mask)) {
        pidMask[range] = mask;
        pidMaskKnown[range] = true;
    } else if (range == 1 && !timedOut && pidMaskKnown[0] && respHas(clean, "NODATA")) {
        pidMask[1] = 0;
        pidMaskKnown[1] = true;
    }
    updatePidSupport();
    if (range == 1 || !pidMaskKnown[0]) logPidSupport();
}

// ถาม 0120 เฉพาะเมื่อ 0100 บอกว่ามีช่วง 21-40 (บิตของ PID 20); ไม่มี = รู้เลยว่าไม่รองรับทั้งช่วง
bool needPidMask2()
{
    if (!pidMaskKnown[0]) return false;
    if ((pidMask[0] & 1) == 0) {
        pidMask[1] = 0;
        pidMaskKnown[1] = true;
        updatePidSupport();
        logPidSupport();
        return false;
    }
    return true;
}
DtcRequest dtcAction = DtcRequest::None;  // งานที่กำลังทำอยู่ (อ่านหรือลบ)
DtcData dtcWork;
bool dtcClearedOk = false;

const char *linkName(LinkState s)
{
    switch (s) {
        case LinkState::NoConfig: return "NO NAME";
        case LinkState::BtStart: return "BT START";
        case LinkState::Connecting: return "CONNECTING";
        case LinkState::Backoff: return "OFFLINE";
        case LinkState::Init: return "INIT ELM";
        case LinkState::Online: return "ONLINE";
        case LinkState::Scanning: return "SCANNING";
    }
    return "?";
}

void setLink(LinkState next)
{
    if (link == next) return;
    Serial.printf("[OBD] %s -> %s\n", linkName(link), linkName(next));
    link = next;
}

void publishDtc(const DtcData &d)
{
    portENTER_CRITICAL(&dtcMux);
    g_dtc = d;
    portEXIT_CRITICAL(&dtcMux);
}

DtcData snapshotDtc()
{
    DtcData d;
    portENTER_CRITICAL(&dtcMux);
    d = g_dtc;
    portEXIT_CRITICAL(&dtcMux);
    return d;
}

void engSend(const char *cmd, Job job, int arg, uint32_t timeoutMs)
{
    while (SerialBT.available() > 0) SerialBT.read();  // ทิ้งข้อมูลเก่าที่ค้าง
    char buf[24];
    const int n = snprintf(buf, sizeof(buf), "%s\r", cmd);
    SerialBT.write(reinterpret_cast<const uint8_t *>(buf), n);
    eng.job = job;
    eng.arg = arg;
    eng.sentAt = millis();
    eng.timeoutMs = timeoutMs;
    eng.rxLen = 0;
    eng.rx[0] = '\0';
}

// 0 = ยังรอ, 1 = ได้คำตอบครบ (เจอ '>'), 2 = หมดเวลา
int engPoll(uint32_t now)
{
    bool prompt = false;
    while (SerialBT.available() > 0) {
        const int c = SerialBT.read();
        if (c < 0) break;
        if (c == '>') {
            prompt = true;
        } else if (eng.rxLen + 1 < sizeof(eng.rx)) {
            eng.rx[eng.rxLen++] = static_cast<char>(c);
        }
    }
    eng.rx[eng.rxLen] = '\0';
    if (prompt) return 1;
    return (now - eng.sentAt >= eng.timeoutMs) ? 2 : 0;
}

void toBackoff(uint32_t now)
{
    eng.job = Job::None;
    dtcReq = DtcRequest::None;
    if (SerialBT.connected()) SerialBT.disconnect();
    const DtcData d = snapshotDtc();
    if (d.state == DtcState::Busy) {  // งานอ่าน/ลบค้างอยู่ แต่ลิงก์หลุดแล้ว
        DtcData e = d;
        e.state = DtcState::Error;
        e.msg = DtcMsg::NoEcu;
        publishDtc(e);
    }
    backoffAt = now;
    setLink(LinkState::Backoff);
}

void finishDtc(bool ok, DtcMsg msg)
{
    DtcData d = dtcWork;
    d.state = ok ? DtcState::Done : DtcState::Error;
    d.msg = msg;
    publishDtc(d);
    dtcAction = DtcRequest::None;
    Serial.printf("[DTC] %s: %d code(s), MIL=%s\n", ok ? "done" : "error", d.count, d.mil ? "ON" : "off");
}

void startDtcRead()
{
    dtcWork = DtcData();
    dtcAction = DtcRequest::Read;
    engSend("0101", Job::DtcStatus, 0, app::CMD_TIMEOUT_DTC_MS);
}

void handleInitDone(uint32_t now, bool timedOut)
{
    char c[RX_MAX];
    cleanText(eng.rx, c, sizeof(c));
    const int idx = eng.arg;
    if (timedOut) {
        if (idx == INIT_IDX_PROBE || idx == INIT_IDX_PIDS2) {
            Serial.printf("[OBD] %s timed out: continuing anyway\n", INIT_CMDS[idx].cmd);
            if (idx == INIT_IDX_PIDS2) applyPidMaskResponse(1, c, true);
        } else if (++initFails >= 3) {
            Serial.printf("[OBD] adapter does not answer '%s': disconnecting\n", INIT_CMDS[idx].cmd);
            toBackoff(now);
            return;
        } else {
            eng.job = Job::None;  // ลองคำสั่งเดิมอีกครั้ง
            return;
        }
    } else if (idx == INIT_IDX_PROBE) {
        ecuSeen = respHas(c, "4100");
        Serial.printf("[OBD] protocol probe: %s\n", ecuSeen ? "ECU answered" : "no ECU answer yet (ignition off?)");
        applyPidMaskResponse(0, c, false);
    } else if (idx == INIT_IDX_DPN) {
        const char *p = c;
        if (*p == 'A') p++;  // "A6" = ค้นหาอัตโนมัติแล้วได้ protocol 6
        canProtocol = (*p >= '6' && *p <= '9');
        Serial.printf("[OBD] protocol number %s -> %s\n", c, canProtocol ? "CAN" : "legacy (K-line/J1850)");
    } else if (idx == INIT_IDX_PIDS2) {
        applyPidMaskResponse(1, c, false);
    }
    initFails = 0;
    eng.job = Job::None;
    initIdx++;
    if (initIdx >= INIT_COUNT) {
        pollIdx = 0;
        consecutiveTimeouts = 0;
        setLink(LinkState::Online);
    }
}

void handlePollDone(uint32_t now, bool timedOut)
{
    const int idx = eng.arg;
    float value = 0;
    eng.job = Job::None;
    if (!timedOut && decodePid(idx, eng.rx, value)) {
        pids[idx].value = value;
        pids[idx].at = now;
        lastGoodAt = now;
        consecutiveTimeouts = 0;
        if (!pidMaskKnown[0] && idx != P_VOLT) reprobePids = true;  // ECU เพิ่งเริ่มตอบ (เพิ่งติดเครื่อง)
        return;
    }
    if (timedOut) {
        if (++consecutiveTimeouts >= 5) {
            Serial.println("[OBD] adapter stopped answering: reconnecting");
            toBackoff(now);
            return;
        }
    } else {
        consecutiveTimeouts = 0;
    }
    pauseUntil = now + app::ERROR_PAUSE_MS;  // NO DATA / ไม่มี ECU ตอบ: พักสักครู่ก่อนถามต่อ
}

void handleDtcDone(uint32_t now, bool timedOut)
{
    (void)now;
    char c[RX_MAX];
    cleanText(eng.rx, c, sizeof(c));
    const Job job = eng.job;
    eng.job = Job::None;

    if (job == Job::DtcStatus) {
        // 41 01 A B C D : A bit7 = MIL, bit0-6 = จำนวนรหัส
        const char *p = strstr(c, "4101");
        uint8_t b[1];
        if (!timedOut && p != nullptr && parseHexBytes(p + 4, 1, b)) {
            dtcWork.mil = (b[0] & 0x80) != 0;
        } else if (timedOut || respIsNoEcu(c)) {
            finishDtc(false, timedOut ? DtcMsg::Timeout : DtcMsg::NoEcu);
            return;
        }
        engSend("03", Job::DtcRead, 0, app::CMD_TIMEOUT_DTC_MS);
        return;
    }

    if (job == Job::DtcRead) {
        if (timedOut) {
            finishDtc(false, DtcMsg::Timeout);
            return;
        }
        parseDtcResponse(eng.rx, canProtocol, dtcWork);
        if (dtcWork.count == 0 && respIsNoEcu(c) && !respHas(c, "NODATA")) {
            finishDtc(false, DtcMsg::NoEcu);  // NO DATA = ไม่มีรหัสค้าง (ถือว่าสำเร็จ) ส่วนอย่างอื่น = ECU ไม่ตอบ
            return;
        }
        finishDtc(true, dtcClearedOk ? DtcMsg::Cleared : DtcMsg::None);
        dtcClearedOk = false;
        return;
    }

    if (job == Job::DtcClear) {
        if (!timedOut && respHas(c, "44")) {
            Serial.println("[DTC] clear accepted by ECU: reading codes again");
            dtcClearedOk = true;
            startDtcRead();
        } else {
            dtcWork = DtcData();
            finishDtc(false, timedOut ? DtcMsg::Timeout : DtcMsg::ClearFailed);
        }
    }
}

// ถามรายการ PID ที่รองรับซ้ำหลังเชื่อมต่อแล้ว (arg 0 = 0100, 1 = 0120)
void handleProbeDone(bool timedOut)
{
    char c[RX_MAX];
    cleanText(eng.rx, c, sizeof(c));
    const int range = eng.arg;
    eng.job = Job::None;
    applyPidMaskResponse(range, c, timedOut);
    if (range == 0 && needPidMask2()) engSend("0120", Job::Probe, 1, app::CMD_TIMEOUT_MS);
}

void engineRun(uint32_t now)
{
    if (!SerialBT.connected()) {
        Serial.println("[BT] link lost");
        toBackoff(now);
        return;
    }

    if (eng.job != Job::None) {
        const int r = engPoll(now);
        if (r == 0) return;
        const bool timedOut = (r == 2);
        switch (eng.job) {
            case Job::Init: handleInitDone(now, timedOut); break;
            case Job::Poll: handlePollDone(now, timedOut); break;
            case Job::DtcStatus:
            case Job::DtcRead:
            case Job::DtcClear: handleDtcDone(now, timedOut); break;
            case Job::Probe: handleProbeDone(timedOut); break;
            case Job::None: break;
        }
        return;
    }

    if (now < pauseUntil) return;

    if (link == LinkState::Init) {
        if (initIdx == INIT_IDX_PIDS2 && !needPidMask2()) {  // ไม่ต้องถาม 0120: ข้ามไปขั้นถัดไป
            initIdx++;
            if (initIdx >= INIT_COUNT) {
                pollIdx = 0;
                consecutiveTimeouts = 0;
                setLink(LinkState::Online);
            }
            return;
        }
        engSend(INIT_CMDS[initIdx].cmd, Job::Init, initIdx, INIT_CMDS[initIdx].timeoutMs);
        return;
    }

    // Online: งานจากหน้า FAULT มาก่อน แล้วค่อยถามค่าปกติต่อ
    const DtcRequest req = dtcReq;
    if (req != DtcRequest::None) {
        dtcReq = DtcRequest::None;
        dtcClearedOk = false;
        Serial.printf("[DTC] %s requested\n", req == DtcRequest::Read ? "read" : "clear");
        if (req == DtcRequest::Read) {
            startDtcRead();
        } else {
            dtcWork = DtcData();
            dtcAction = DtcRequest::Clear;
            engSend("04", Job::DtcClear, 0, app::CMD_TIMEOUT_DTC_MS);
        }
        return;
    }

    if (reprobePids) {
        reprobePids = false;
        Serial.println("[OBD] ECU is answering now: asking which PIDs it supports");
        engSend("0100", Job::Probe, 0, app::CMD_TIMEOUT_PROTO_MS);
        return;
    }

    // ข้าม PID ที่ ECU บอกว่าไม่รองรับ (ไม่เสียเวลารอ NO DATA + พัก)
    uint8_t idx = POLL_SEQUENCE[pollIdx];
    for (size_t tries = 0; tries < sizeof(POLL_SEQUENCE); tries++) {
        idx = POLL_SEQUENCE[pollIdx];
        pollIdx = (pollIdx + 1) % static_cast<int>(sizeof(POLL_SEQUENCE));
        if (pidSupport[idx] != PidSupport::No) break;
    }
    engSend(PID_DEFS[idx].cmd, Job::Poll, idx, app::CMD_TIMEOUT_MS);
}

// =====================================================================
//  ค้นหาอุปกรณ์ Bluetooth (หน้า SCAN)
//
//  ไม่ใช้ SerialBT.discover() เพราะไลบรารีไม่ยอมค้นหาหลังเคยเชื่อมต่อด้วยชื่อ/MAC และอุปกรณ์รุ่นเก่าหลายตัว
//  ไม่บอกชื่อตอนค้นหา: ระหว่างค้นหาจึงลงทะเบียน GAP callback ของเราเอง (ได้ชื่อ, RSSI, ถามชื่อเพิ่มได้)
//  เสร็จแล้ว end() + begin() SerialBT ใหม่ เพื่อคืน callback ของไลบรารี (จำเป็นสำหรับการจับคู่/PIN ตอนเชื่อมต่อ)
// =====================================================================
volatile bool scanInquiryDone = false;
volatile bool scanNameDone = false;

bool nameLooksObd(const char *name)
{
    static constexpr const char *HINTS[] = {"OBD", "ELM", "VLINK", "V-LINK", "VGATE", "ICAR", "VEEPEAK", "KONNWEI", "KW9", "SCAN", "CARISTA", "LELINK"};
    char up[33];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(up) - 1; i++) up[i] = static_cast<char>(toupper(static_cast<uint8_t>(name[i])));
    up[i] = '\0';
    for (const char *h : HINTS) {
        if (strstr(up, h) != nullptr) return true;
    }
    return false;
}

void formatMac(const uint8_t *mac, char *out, size_t size)
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ฟอนต์บนจอมีแค่ ASCII: ตัวอื่น (เช่นชื่อภาษาไทย/อีโมจิ) แสดงเป็น '?' (ไบต์ต่อเนื่องของ UTF-8 ข้ามไป)
void copyAsciiName(char *dst, size_t size, const uint8_t *src, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && src[i] != 0 && n < size - 1; i++) {
        const uint8_t c = src[i];
        if (c >= 0x20 && c < 0x7F) dst[n++] = static_cast<char>(c);
        else if ((c & 0xC0) != 0x80) dst[n++] = '?';
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

// เพิ่ม/อัปเดตอุปกรณ์ในรายการ (name = "" = ไม่เปลี่ยนชื่อเดิม, rssi -127 / cod 0 = ไม่เปลี่ยน)
void scanUpsert(const uint8_t *mac, const char *name, int8_t rssi, uint32_t cod)
{
    portENTER_CRITICAL(&scanMux);
    int i = 0;
    while (i < g_scanCount && memcmp(g_scanDev[i].mac, mac, 6) != 0) i++;
    if (i == g_scanCount) {
        if (g_scanCount >= app::MAX_SCAN_DEVICES) {
            portEXIT_CRITICAL(&scanMux);
            return;
        }
        ScanDevice &d = g_scanDev[g_scanCount++];
        memcpy(d.mac, mac, 6);
        d.name[0] = '\0';
        d.rssi = -127;
        d.cod = 0;
        d.obd = false;
    }
    ScanDevice &d = g_scanDev[i];
    if (name[0] != '\0') {
        strlcpy(d.name, name, sizeof(d.name));
        d.obd = nameLooksObd(d.name);
    }
    if (rssi != -127) d.rssi = rssi;
    if (cod != 0) d.cod = cod;
    scanVersion = scanVersion + 1;
    portEXIT_CRITICAL(&scanMux);
}

void scanGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char name[33] = "";
            int8_t rssi = -127;
            uint32_t cod = 0;
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
                switch (prop.type) {
                    case ESP_BT_GAP_DEV_PROP_BDNAME:
                        copyAsciiName(name, sizeof(name), static_cast<const uint8_t *>(prop.val), prop.len);
                        break;
                    case ESP_BT_GAP_DEV_PROP_EIR:
                        if (name[0] == '\0') {
                            uint8_t len = 0;
                            uint8_t *eir = static_cast<uint8_t *>(prop.val);
                            uint8_t *n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                            if (n == nullptr) n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                            if (n != nullptr) copyAsciiName(name, sizeof(name), n, len);
                        }
                        break;
                    case ESP_BT_GAP_DEV_PROP_RSSI:
                        rssi = *static_cast<int8_t *>(prop.val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_COD:
                        memcpy(&cod, prop.val, min<size_t>(prop.len, sizeof(cod)));
                        break;
                    default:
                        break;
                }
            }
            scanUpsert(param->disc_res.bda, name, rssi, cod);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) scanInquiryDone = true;
            break;
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                char name[33];
                copyAsciiName(name, sizeof(name), param->read_rmt_name.rmt_name, sizeof(param->read_rmt_name.rmt_name));
                scanUpsert(param->read_rmt_name.bda, name, -127, 0);
            }
            scanNameDone = true;
            break;
        default:
            break;
    }
}

bool scanCancelled()
{
    return !scanHold;  // ผู้ใช้ออกจากหน้า SCAN แล้ว
}

// รอ flag (ตั้งโดย callback) จนกว่าจะเป็น true, หมดเวลา หรือผู้ใช้ยกเลิก
void scanWait(volatile bool &flag, uint32_t timeoutMs)
{
    const uint32_t t0 = millis();
    while (!flag && !scanCancelled() && millis() - t0 < timeoutMs) vTaskDelay(pdMS_TO_TICKS(20));
}

// คืน Bluetooth ให้ไลบรารี SerialBT (callback ของไลบรารีกลับมาทำงาน พร้อมเชื่อมต่อ)
void restoreSerialBt()
{
    char name[33], pin[17];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    SerialBT.end();
    vTaskDelay(pdMS_TO_TICKS(100));
    SerialBT.setPin(pin);
    btStarted = SerialBT.begin(app::BT_LOCAL_NAME, true);
    Serial.printf("[SCAN] Bluetooth restarted for SPP: %s\n", btStarted ? "ok" : "FAILED");
}

// ทำงานใน OBD task (บล็อกได้ ไม่กระทบจอ): ค้นหา ~10 วินาที แล้วถามชื่ออุปกรณ์ที่ยังไม่รู้ชื่อ
void runScan()
{
    scanReq = false;
    eng.job = Job::None;
    if (btStarted && SerialBT.connected()) SerialBT.disconnect();
    setLink(LinkState::Scanning);

    portENTER_CRITICAL(&scanMux);
    g_scanCount = 0;
    scanVersion = scanVersion + 1;
    portEXIT_CRITICAL(&scanMux);
    scanStartedAt = millis();
    scanState = ScanState::Inquiry;

    if (!btStarted) {
        char name[33], pin[17];
        getConfig(name, sizeof(name), pin, sizeof(pin));
        SerialBT.setPin(pin);
        btStarted = SerialBT.begin(app::BT_LOCAL_NAME, true);
    }
    scanInquiryDone = false;
    if (!btStarted || esp_bt_gap_register_callback(scanGapCb) != ESP_OK ||
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, app::SCAN_INQ_UNITS, 0) != ESP_OK) {
        Serial.println("[SCAN] could not start Bluetooth discovery");
        if (btStarted) restoreSerialBt();
        scanState = ScanState::Error;
        return;
    }
    Serial.println("[SCAN] discovering Bluetooth devices ...");
    scanWait(scanInquiryDone, app::SCAN_INQ_UNITS * 1280UL + 3000);
    esp_bt_gap_cancel_discovery();

    // ถามชื่อของอุปกรณ์ที่ไม่บอกชื่อตอนค้นหา (ทีละตัว)
    scanState = ScanState::Names;
    const uint32_t namesStart = millis();
    for (int i = 0; i < app::MAX_SCAN_DEVICES && !scanCancelled() && millis() - namesStart < app::SCAN_NAMES_TOTAL_MS; i++) {
        uint8_t mac[6];
        bool need = false;
        portENTER_CRITICAL(&scanMux);
        if (i < g_scanCount && g_scanDev[i].name[0] == '\0') {
            memcpy(mac, g_scanDev[i].mac, 6);
            need = true;
        }
        portEXIT_CRITICAL(&scanMux);
        if (!need) continue;
        scanNameDone = false;
        if (esp_bt_gap_read_remote_name(mac) == ESP_OK) scanWait(scanNameDone, app::SCAN_NAME_TIMEOUT_MS);
    }

    restoreSerialBt();
    Serial.printf("[SCAN] %s: %d device(s) in %lu ms\n", scanCancelled() ? "cancelled" : "done", g_scanCount,
                  static_cast<unsigned long>(millis() - scanStartedAt));
    scanState = scanCancelled() ? ScanState::Idle : ScanState::Done;
}

void btConnectStep(uint32_t now)
{
    char name[33], pin[17], macText[18];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    getConfigMac(macText, sizeof(macText));
    Serial.printf("[BT] connecting to \"%s\" %s (PIN %s) ...\n", name, macText, pin[0] != '\0' ? "set" : "none");
    SerialBT.setPin(pin);

    // มี MAC (เลือกจากหน้า SCAN หรือพิมพ์ MAC เป็นชื่อ) = เชื่อมตรง ไม่เกิน ~10 วินาที
    // มีแต่ชื่อ = ไลบรารีต้องค้นหาชื่อก่อน (ถ้าไม่เจอ รอนานสุด ~30 วินาที)
    uint8_t mac[6];
    const bool byMac = parseMac(macText, mac) || parseMac(name, mac);
    const bool ok = byMac ? SerialBT.connect(mac) : SerialBT.connect(String(name));
    if (reconnectReq) return;  // ผู้ใช้เปลี่ยนค่าระหว่างรอ: ให้รอบถัดไปจัดการ
    if (ok) {
        Serial.println("[BT] SPP connected");
        initIdx = 0;
        initFails = 0;
        ecuSeen = false;
        resetPidSupport();
        eng.job = Job::None;
        pauseUntil = 0;
        setLink(LinkState::Init);
    } else {
        Serial.printf("[BT] connect failed to \"%s\" %s (adapter off / out of range / wrong name?)\n", name,
                      byMac ? (macText[0] != '\0' ? macText : name) : "(by name)");
        backoffAt = now;
        setLink(LinkState::Backoff);
    }
}

void obdStep(uint32_t now)
{
    if (scanHold) {  // เปิดหน้า SCAN อยู่: ไม่เชื่อมต่อ รอคำสั่งค้นหา
        if (scanReq) {
            runScan();
        } else if (link != LinkState::Scanning) {
            eng.job = Job::None;
            if (btStarted && SerialBT.connected()) SerialBT.disconnect();
            setLink(LinkState::Scanning);
        }
        return;
    }
    if (link == LinkState::Scanning) reconnectReq = true;  // ออกจากหน้า SCAN: กลับไปเชื่อมต่อตามค่าที่ตั้งไว้

    if (reconnectReq) {
        reconnectReq = false;
        eng.job = Job::None;
        if (btStarted && SerialBT.connected()) SerialBT.disconnect();
        char name[33], pin[17];
        getConfig(name, sizeof(name), pin, sizeof(pin));
        setLink(name[0] == '\0' ? LinkState::NoConfig : (btStarted ? LinkState::Connecting : LinkState::BtStart));
        return;
    }

    switch (link) {
        case LinkState::NoConfig: {
            char name[33], pin[17];
            getConfig(name, sizeof(name), pin, sizeof(pin));
            if (name[0] != '\0') setLink(LinkState::BtStart);
            break;
        }
        case LinkState::BtStart: {
            char name[33], pin[17];
            getConfig(name, sizeof(name), pin, sizeof(pin));
            SerialBT.setPin(pin);
            btStarted = SerialBT.begin(app::BT_LOCAL_NAME, true);  // true = master (เราเป็นฝ่ายเชื่อมต่อไปหาอะแดปเตอร์)
            Serial.printf("[BT] Bluetooth Classic %s\n", btStarted ? "started (master)" : "FAILED to start");
            if (btStarted) {
                setLink(LinkState::Connecting);
            } else {
                backoffAt = now;
                setLink(LinkState::Backoff);
            }
            break;
        }
        case LinkState::Connecting:
            if (!btStarted) {
                setLink(LinkState::BtStart);
            } else {
                btConnectStep(now);
            }
            break;
        case LinkState::Backoff:
            if (now - backoffAt >= app::BT_RETRY_MS) setLink(btStarted ? LinkState::Connecting : LinkState::BtStart);
            break;
        case LinkState::Init:
        case LinkState::Online:
            engineRun(now);
            break;
        case LinkState::Scanning:
            break;
    }
}

void obdTask(void *)
{
    for (;;) {
        obdStep(millis());
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

// =====================================================================
//  ค่าที่ใช้แสดงผล: ค่าจริงจากรถเท่านั้น (ไม่มีค่า/ค่าเก่า = NaN แสดง "--")
// =====================================================================
// Bluetooth ต่อกับอะแดปเตอร์อยู่ (กำลังตั้งค่า ELM327 หรือออนไลน์)
bool btConnected()
{
    const LinkState ls = link;
    return ls == LinkState::Init || ls == LinkState::Online;
}

// ได้ค่าจริงใหม่จาก ECU ภายใน DATA_FRESH_MS
bool isRealData(uint32_t now)
{
    const uint32_t good = lastGoodAt;
    return link == LinkState::Online && good != 0 && (now - good) < app::DATA_FRESH_MS;
}

Rect2View getView(uint32_t now)
{
    Rect2View w;
    w.real = isRealData(now);
    const bool online = link == LinkState::Online;
    for (int i = 0; i < P_COUNT; i++) {
        const uint32_t at = pids[i].at;
        w.na[i] = online && pidSupport[i] == PidSupport::No;
        w.v[i] = (online && at != 0 && (now - at) < app::VALUE_STALE_MS) ? pids[i].value : NAN;
    }
    return w;
}

// =====================================================================
//  รหัสข้อผิดพลาด: คำอธิบายสั้นๆ
// =====================================================================
struct DtcInfo {
    const char *code;
    const char *text;
};
constexpr DtcInfo DTC_TABLE[] = {
    {"P0100", "MAF circuit"},         {"P0101", "MAF range/performance"}, {"P0102", "MAF circuit low"},
    {"P0103", "MAF circuit high"},    {"P0110", "Intake temp circuit"},   {"P0115", "Coolant temp circuit"},
    {"P0117", "Coolant temp low"},    {"P0118", "Coolant temp high"},     {"P0120", "Throttle pos circuit"},
    {"P0125", "Slow engine warm-up"}, {"P0128", "Thermostat too cool"},   {"P0130", "O2 sensor B1S1 circuit"},
    {"P0133", "O2 sensor slow B1S1"}, {"P0135", "O2 heater B1S1"},        {"P0171", "Mixture too lean B1"},
    {"P0172", "Mixture too rich B1"}, {"P0174", "Mixture too lean B2"},   {"P0300", "Random misfire"},
    {"P0301", "Cylinder 1 misfire"},  {"P0302", "Cylinder 2 misfire"},   {"P0303", "Cylinder 3 misfire"},
    {"P0304", "Cylinder 4 misfire"},  {"P0325", "Knock sensor circuit"},  {"P0335", "Crank sensor circuit"},
    {"P0340", "Cam sensor circuit"},  {"P0420", "Catalyst efficiency B1"}, {"P0430", "Catalyst efficiency B2"},
    {"P0440", "EVAP system"},         {"P0442", "EVAP small leak"},       {"P0455", "EVAP large leak"},
    {"P0500", "Vehicle speed sensor"}, {"P0505", "Idle control system"},  {"P0562", "System voltage low"},
    {"P0563", "System voltage high"}, {"P0600", "Serial comm link"},      {"P0700", "Transmission control"},
    {"U0100", "Lost comm with ECM"},
};

const char *dtcText(const char *code)
{
    for (const DtcInfo &i : DTC_TABLE) {
        if (strcmp(i.code, code) == 0) return i.text;
    }
    return code[0] == 'P' && code[1] >= '1' && code[1] <= '3' ? "Manufacturer specific" : "Generic code";
}

void requestDtc(DtcRequest kind)
{
    if (snapshotDtc().state == DtcState::Busy) return;
    DtcData d = snapshotDtc();
    d.state = DtcState::Busy;
    d.msg = DtcMsg::None;
    dtcListPage = 0;
    if (link == LinkState::Online) {
        publishDtc(d);
        dtcReq = kind;
    } else {  // ยังไม่ได้ต่ออะแดปเตอร์: แจ้งเลย ไม่ต้องรอ
        d.state = DtcState::Error;
        d.msg = DtcMsg::NoLink;
        publishDtc(d);
    }
}

// =====================================================================
//  จอสัมผัส XPT2046 (อ่านเองผ่าน SPI)
// =====================================================================
int16_t bestTwoAverage(int16_t a, int16_t b, int16_t c)
{
    const int16_t dab = abs(a - b);
    const int16_t dac = abs(a - c);
    const int16_t dbc = abs(b - c);
    if (dab <= dac && dab <= dbc) return (a + b) >> 1;
    if (dac <= dab && dac <= dbc) return (a + c) >> 1;
    return (b + c) >> 1;
}

bool readRawTouch(RawTouch &out)
{
    int16_t data[6] = {0, 0, 0, 0, 0, 0};

    touchSpi.beginTransaction(SPISettings(cyd::TOUCH_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    digitalWrite(cyd::TOUCH_PIN_CS, LOW);
    touchSpi.transfer(0xB1);
    const int16_t z1 = static_cast<int16_t>(touchSpi.transfer16(0xC1) >> 3);
    int z = z1 + 4095;
    const int16_t z2 = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);
    z -= z2;
    if (z >= app::TOUCH_Z_MIN) {
        touchSpi.transfer16(0x91);
        data[0] = static_cast<int16_t>(touchSpi.transfer16(0xD1) >> 3);
        data[1] = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);
        data[2] = static_cast<int16_t>(touchSpi.transfer16(0xD1) >> 3);
        data[3] = static_cast<int16_t>(touchSpi.transfer16(0x91) >> 3);
    }
    data[4] = static_cast<int16_t>(touchSpi.transfer16(0xD0) >> 3);
    data[5] = static_cast<int16_t>(touchSpi.transfer16(0) >> 3);
    digitalWrite(cyd::TOUCH_PIN_CS, HIGH);
    touchSpi.endTransaction();

    out.z = static_cast<int16_t>(z < 0 ? 0 : z);
    if (z < app::TOUCH_Z_MIN) return false;
    out.x = bestTwoAverage(data[0], data[2], data[4]);
    out.y = bestTwoAverage(data[1], data[3], data[5]);
    return true;
}

// ค่า calibrate เก็บในพิกัดของจอแบบไม่หมุน; หมุน 180° (Display → Flip) = กลับพิกัดตอนใช้งาน
void mapRawToScreen(int16_t rawX, int16_t rawY, int &sx, int &sy)
{
    const float u = cal.swapXY ? rawY : rawX;
    const float v = cal.swapXY ? rawX : rawY;
    sx = constrain(static_cast<int>(lroundf(cal.xa + cal.xb * u)), 0, SCREEN_W - 1);
    sy = constrain(static_cast<int>(lroundf(cal.ya + cal.yb * v)), 0, SCREEN_H - 1);
    if (dispFix & DISP_FLIP) {
        sx = SCREEN_W - 1 - sx;
        sy = SCREEN_H - 1 - sy;
    }
}

// ตำแหน่งเป้า calibrate บนจอ (ที่วาด)
void calTarget(int index, int &tx, int &ty)
{
    tx = (index == 0 || index == 3) ? app::CAL_MARGIN : SCREEN_W - 1 - app::CAL_MARGIN;
    ty = (index == 0 || index == 1) ? app::CAL_MARGIN : SCREEN_H - 1 - app::CAL_MARGIN;
}

// ตำแหน่งเป้าเดียวกันในพิกัดของจอแบบไม่หมุน (ใช้คำนวณค่า calibrate)
void calTargetUnflipped(int index, int &tx, int &ty)
{
    calTarget(index, tx, ty);
    if (dispFix & DISP_FLIP) {
        tx = SCREEN_W - 1 - tx;
        ty = SCREEN_H - 1 - ty;
    }
}

bool computeCalibration()
{
    const int dxTop = abs(calRawX[1] - calRawX[0]);
    const int dyTop = abs(calRawY[1] - calRawY[0]);
    TouchCalibration c;
    c.swapXY = dyTop > dxTop;

    auto rawU = [&](int i) { return static_cast<float>(c.swapXY ? calRawY[i] : calRawX[i]); };
    auto rawV = [&](int i) { return static_cast<float>(c.swapXY ? calRawX[i] : calRawY[i]); };
    const float leftU = (rawU(0) + rawU(3)) / 2.0f;
    const float rightU = (rawU(1) + rawU(2)) / 2.0f;
    const float topV = (rawV(0) + rawV(1)) / 2.0f;
    const float bottomV = (rawV(2) + rawV(3)) / 2.0f;
    const float spanU = rightU - leftU;
    const float spanV = bottomV - topV;
    if (fabsf(spanU) < app::CAL_MIN_SPAN || fabsf(spanV) < app::CAL_MIN_SPAN) {
        Serial.printf("[CAL] raw span too small (u=%.0f v=%.0f): did you touch the crosses?\n", spanU, spanV);
        return false;
    }

    int leftX, topY, rightX, bottomY;
    calTargetUnflipped(0, leftX, topY);
    calTargetUnflipped(2, rightX, bottomY);
    c.xb = static_cast<float>(rightX - leftX) / spanU;
    c.xa = static_cast<float>(leftX) - c.xb * leftU;
    c.yb = static_cast<float>(bottomY - topY) / spanV;
    c.ya = static_cast<float>(topY) - c.yb * topV;

    float worst = 0.0f;
    for (int i = 0; i < 4; i++) {
        int tx, ty;
        calTargetUnflipped(i, tx, ty);
        const float sx = c.xa + c.xb * rawU(i);
        const float sy = c.ya + c.yb * rawV(i);
        const float err = fmaxf(fabsf(sx - tx), fabsf(sy - ty));
        if (err > worst) worst = err;
    }
    Serial.printf("[CAL] swapXY=%s  x: %.4f*u%+.1f  y: %.4f*v%+.1f  worst target error %.1f px\n", c.swapXY ? "yes" : "no", c.xb, c.xa,
                  c.yb, c.ya, worst);
    if (worst > app::CAL_MAX_ERROR_PX) {
        Serial.println("[CAL] points are not consistent with a rectangle: please try again");
        return false;
    }
    c.valid = true;
    cal = c;
    return true;
}

void saveCalibration()
{
    prefs.putBool("calok", cal.valid);
    prefs.putBool("swap", cal.swapXY);
    prefs.putFloat("xa", cal.xa);
    prefs.putFloat("xb", cal.xb);
    prefs.putFloat("ya", cal.ya);
    prefs.putFloat("yb", cal.yb);
    Serial.println("[CAL] saved to flash (Preferences)");
}

// ใช้ namespace "lab9" เดิม: ค่า calibrate และความสว่างจอจาก Lab 9 ยังใช้ต่อได้ ไม่ต้อง calibrate ใหม่
void loadSettings()
{
    prefs.begin("lab9", false);
    if (prefs.getBool("calok", false)) {
        cal.valid = true;
        cal.swapXY = prefs.getBool("swap", false);
        cal.xa = prefs.getFloat("xa", 0.0f);
        cal.xb = prefs.getFloat("xb", 0.0f);
        cal.ya = prefs.getFloat("ya", 0.0f);
        cal.yb = prefs.getFloat("yb", 0.0f);
        Serial.printf("[CAL] loaded from flash: swapXY=%s x: %.4f*u%+.1f  y: %.4f*v%+.1f\n", cal.swapXY ? "yes" : "no", cal.xb, cal.xa,
                      cal.yb, cal.ya);
    } else {
        Serial.println("[CAL] no saved calibration: the calibration screen will start");
    }
    backlightPercent = prefs.getUChar("bl", app::BACKLIGHT_DEFAULT_PERCENT);
    shiftRpm = constrain(static_cast<int>(prefs.getUShort("shift", app::SHIFT_RPM_DEFAULT)), app::SHIFT_RPM_MIN, app::SHIFT_RPM_MAX);
    dispFix = prefs.getUChar(DISP_NVS_KEY, 0) & (DISP_INVERT | DISP_SWAP_RB | DISP_FLIP);
    Serial.printf("[DISP] %s  invert=%s swapRB=%s flip=%s\n", DISPLAY_CHIP, dispFix & DISP_INVERT ? "on" : "off",
                  dispFix & DISP_SWAP_RB ? "on" : "off", dispFix & DISP_FLIP ? "on" : "off");
    coolLimit = constrain(static_cast<int>(prefs.getUChar("coollim", app::COOLANT_LIMIT_C)), app::COOLANT_LIMIT_MIN_C, app::COOLANT_LIMIT_MAX_C);
    Serial.printf("[CFG] shift light %d rpm, overheat %d C\n", shiftRpm, coolLimit);

    const String name = prefs.getString("obdname", app::DEFAULT_OBD_NAME);
    const String pin = prefs.getString("obdpin", app::DEFAULT_OBD_PIN);
    const String mac = prefs.getString("obdmac", "");
    setAdapter(name.c_str(), mac.c_str());
    setConfig(name.c_str(), pin.c_str());
    Serial.printf("[CFG] OBD name=\"%s\" mac=%s  PIN %s\n", name.c_str(), mac.length() > 0 ? mac.c_str() : "(none)",
                  pin.length() > 0 ? "set" : "(none)");
}

void pollTouch(uint32_t now)
{
    touch.pressEdge = false;
    touch.releaseEdge = false;
    if (now - lastTouchPollAt < app::TOUCH_POLL_MS) return;
    lastTouchPollAt = now;

    RawTouch raw;
    const bool touching = readRawTouch(raw);
    touch.touchingNow = touching;
    touch.z = raw.z;
    if (touching) {
        touch.rawX = raw.x;
        touch.rawY = raw.y;
        if (touch.pressCount < 255) touch.pressCount++;
        touch.releaseCount = 0;
    } else {
        if (touch.releaseCount < 255) touch.releaseCount++;
        touch.pressCount = 0;
    }

    if (!touch.down && touch.pressCount >= app::TOUCH_PRESS_SAMPLES) {
        touch.down = true;
        touch.pressEdge = true;
        touch.downSince = now;
        touch.smoothReady = false;
    }

    if (touch.down && touching && cal.valid) {
        int sx, sy;
        mapRawToScreen(raw.x, raw.y, sx, sy);
        if (!touch.smoothReady) {
            touch.x = sx;
            touch.y = sy;
            touch.smoothReady = true;
        } else {
            touch.x += static_cast<int>(lroundf(app::TOUCH_SMOOTH_ALPHA * static_cast<float>(sx - touch.x)));
            touch.y += static_cast<int>(lroundf(app::TOUCH_SMOOTH_ALPHA * static_cast<float>(sy - touch.y)));
        }
        touch.lastX = touch.x;
        touch.lastY = touch.y;
    }

    if (touch.down && touch.releaseCount >= app::TOUCH_RELEASE_SAMPLES) {
        touch.down = false;
        touch.releaseEdge = true;
    }
}

// =====================================================================
//  ชื่อสำหรับ Serial
// =====================================================================
const char *widgetName(Widget w)
{
    static constexpr const char *NAMES[] = {
        "-", "TabDrive", "TabFocus", "TabTrip", "TabFault", "TabSet", "Tile0", "Tile1", "Tile2", "Tile3", "Gauge",
        "FocusPrev", "FocusNext", "FocusReset", "TripReset", "ZeroArm", "BtnRead", "BtnClear", "BtnPrev", "BtnNext",
        "FieldName", "FieldPin", "BtnConnect", "BtnRecal", "BlSlider", "ThemeDark", "ThemeLight", "ThemeAuto",
        "Sound0", "Sound1", "Sound2", "Sound3", "ShiftMinus", "ShiftPlus", "CoolMinus", "CoolPlus",
        "DispInvert", "DispSwapRB", "DispFlip",
        "ScanRow0", "ScanRow1", "ScanRow2", "ScanRow3", "ScanRow4", "ScanPrev", "ScanNext", "ScanBack", "ScanManual", "ScanRescan",
        "Toast"};
    static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == static_cast<size_t>(Widget::Toast) + 1, "NAMES must match enum Widget");
    if (isKey(w)) return "Key";
    const size_t i = static_cast<size_t>(w);
    return i < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[i] : "?";
}

const char *pageName(Page p)
{
    static constexpr const char *NAMES[] = {"DRIVE", "FOCUS", "TRIP", "FAULT", "SET", "KEYBOARD", "CALIBRATE", "SCAN"};
    return NAMES[static_cast<int>(p)];
}

// =====================================================================
//  ข้อมูลของแต่ละค่า (ชื่อ, หน่วย, สเกล) และเกณฑ์สี
// =====================================================================
struct PidInfo {
    const char *name;   // หัวหน้า FOCUS
    const char *label;  // ป้ายบนการ์ดหน้า DRIVE
    const char *unit;
    float lo, hi;       // สเกลของแถบระดับและกราฟ
    uint8_t decimals;
};
constexpr PidInfo PID_INFO[P_COUNT] = {  // ลำดับตาม PidIdx
    {"ENGINE SPEED", "RPM", "RPM", 0, 8000, 0},
    {"VEHICLE SPEED", "SPEED", "km/h", 0, 200, 0},
    {"COOLANT", "COOL °C", "°C", 60, 120, 0},
    {"THROTTLE", "THR %", "%", 0, 100, 0},
    {"ENGINE LOAD", "LOAD %", "%", 0, 100, 0},
    {"INTAKE AIR", "IAT °C", "°C", 0, 80, 0},
    {"FUEL LEVEL", "FUEL %", "%", 0, 100, 0},
    {"BATTERY", "BATT V", "V", 11, 15, 1},
};
constexpr uint8_t FOCUS_ORDER[P_COUNT] = {P_RPM, P_SPEED, P_COOL, P_VOLT, P_FUEL, P_THR, P_LOAD, P_IAT};
constexpr uint8_t TILE_PID[TILE_COUNT] = {P_COOL, P_VOLT, P_THR};  // บนลงล่าง (FUEL ดูได้ในหน้า FOCUS)
constexpr const char *TILE_TAG[TILE_COUNT] = {"PID 05", "ATRV", "PID 11"};  // ป้ายเล็กมุมขวาบนของการ์ด
static_assert(app::RPM_MAX == 8000.0f, "dial numerals 0-8 assume an 8000 rpm scale");

enum class Level : uint8_t { Neutral, Ok, Warn, Bad };

uint16_t levelColor(Level l)
{
    switch (l) {
        case Level::Ok: return T->ok;
        case Level::Warn: return T->warn;
        case Level::Bad: return T->bad;
        case Level::Neutral: break;
    }
    return T->muted;
}

Level pidLevel(int idx, float v)
{
    if (isnan(v)) return Level::Neutral;
    switch (idx) {
        case P_COOL: return v >= coolLimit ? Level::Bad : (v >= coolLimit - 8 ? Level::Warn : Level::Ok);
        case P_VOLT: return v < 12.4f ? Level::Bad : (v < 12.8f ? Level::Warn : Level::Ok);
        case P_FUEL: return v < 10 ? Level::Bad : (v < 20 ? Level::Warn : Level::Ok);
        case P_RPM: return v >= app::RPM_RED ? Level::Bad : Level::Neutral;
        default: return Level::Neutral;
    }
}

// สีตัวเลข: ปกติ text, เฝ้าดู/อันตรายใช้สีเกณฑ์, ไม่มีค่า muted
uint16_t valueColor(int idx, float v)
{
    if (isnan(v)) return T->muted;
    const Level l = pidLevel(idx, v);
    return (l == Level::Warn || l == Level::Bad) ? levelColor(l) : T->text;
}

void formatPid(int idx, float v, char *out, size_t size)
{
    if (isnan(v)) {
        strlcpy(out, "--", size);
    } else if (PID_INFO[idx].decimals == 1) {
        snprintf(out, size, "%.1f", v);
    } else {
        formatThousands(static_cast<int>(lroundf(v)), out, size);
    }
}

// แนวโน้ม 5 วินาที: 1 = ขึ้น, -1 = ลง, 0 = เปลี่ยนน้อยกว่า 2% ของสเกล
int trendOf(int idx, float v)
{
    if (isnan(v) || historyCount < 6) return 0;
    const float prev = history[idx][(historyHead - 6 + HISTORY_LEN) % HISTORY_LEN];
    if (isnan(prev)) return 0;
    const float th = (PID_INFO[idx].hi - PID_INFO[idx].lo) * 0.02f;
    return v - prev > th ? 1 : (prev - v > th ? -1 : 0);
}

// =====================================================================
//  หน้า SET: ตำแหน่งแถวตามระยะเลื่อน (ใช้ทั้ง hit test และการวาด)
// =====================================================================
int setRowTop(int r)
{
    return SET_AREA.y + 4 + r * SET_ROW_H - setScroll;
}

// ตัวเลือกแบบ segmented ชิดขวาของแถว (พื้นที่แตะสูงเต็มแถว)
Rect setSegRect(int top, int count, int w, int i)
{
    return {static_cast<int16_t>(308 - count * w - 2 + i * w), static_cast<int16_t>(top), static_cast<int16_t>(w), SET_ROW_H};
}

constexpr Rect stepMinusRect(int top) { return {186, static_cast<int16_t>(top + 4), 28, 23}; }
constexpr Rect stepPlusRect(int top) { return {280, static_cast<int16_t>(top + 4), 28, 23}; }

Widget setHitTest(int x, int y)
{
    if (!contains(SET_AREA, x, y)) return Widget::None;
    const int offset = y - SET_AREA.y - 4 + setScroll;
    if (offset < 0) return Widget::None;
    const int r = offset / SET_ROW_H;
    if (r >= SR_COUNT) return Widget::None;
    const int top = setRowTop(r);
    switch (r) {
        case SR_ADAPTER: return Widget::FieldName;
        case SR_PIN: return Widget::FieldPin;
        case SR_CONNECT: return Widget::BtnConnect;
        case SR_THEME:
            for (int i = 0; i < 3; i++) {
                if (contains(setSegRect(top, 3, THEME_SEG_W, i), x, y)) return Widget::ThemeDark + i;
            }
            break;
        case SR_BACKLIGHT:
            if (x >= BL_TRACK_X - 8 && x < BL_TRACK_X + BL_TRACK_W + 8) return Widget::BlSlider;
            break;
        case SR_SOUND:
            for (int i = 0; i < 4; i++) {
                if (contains(setSegRect(top, 4, SOUND_SEG_W, i), x, y)) return Widget::Sound0 + i;
            }
            break;
        case SR_SHIFT:
        case SR_COOL: {
            const Widget minus = r == SR_SHIFT ? Widget::ShiftMinus : Widget::CoolMinus;
            if (x >= 180 && x < 220) return minus;     // พื้นที่แตะกว้างกว่าปุ่ม
            if (x >= 274 && x < 314) return minus + 1;
            break;
        }
        case SR_INVERT: return Widget::DispInvert;
        case SR_SWAPRB: return Widget::DispSwapRB;
        case SR_FLIP: return Widget::DispFlip;
        case SR_RECAL: return Widget::BtnRecal;
        default: break;
    }
    return Widget::None;
}

// =====================================================================
//  hit test
// =====================================================================
Widget hitTest(Page p, int x, int y)
{
    if (hasTabs(p)) {
        if (toast.active && contains(TOAST_RECT, x, y)) return Widget::Toast;
        for (int i = 0; i < TAB_COUNT; i++) {
            if (contains(tabRect(i), x, y)) return Widget::TabDrive + i;
        }
    }
    switch (p) {
        case Page::Drive:
            for (int i = 0; i < TILE_COUNT; i++) {
                if (contains(tileRect(i), x, y)) return Widget::Tile0 + i;
            }
            if (contains(GAUGE_HIT, x, y)) return Widget::Gauge;
            break;
        case Page::Focus:
            if (contains(FOCUS_PREV_BTN, x, y)) return Widget::FocusPrev;
            if (contains(FOCUS_NEXT_BTN, x, y)) return Widget::FocusNext;
            if (contains(FOCUS_RESET_BTN, x, y)) return Widget::FocusReset;
            break;
        case Page::Trip:
            if (contains(TRIP_RESET_BTN, x, y)) return Widget::TripReset;
            if (contains(ZERO_ARM_BTN, x, y)) return Widget::ZeroArm;
            break;
        case Page::Faults:
            if (contains(READ_BTN, x, y)) return Widget::BtnRead;
            if (contains(CLEAR_BTN, x, y)) return Widget::BtnClear;
            if (contains(PREV_BTN, x, y)) return Widget::BtnPrev;
            if (contains(NEXT_BTN, x, y)) return Widget::BtnNext;
            break;
        case Page::Config:
            return setHitTest(x, y);
        case Page::Keyboard:
            for (int i = 0; i < KEY_COUNT; i++) {
                if (contains(keyRect(i), x, y)) return keyWidget(i);
            }
            break;
        case Page::Scan:
            for (int k = 0; k < SCAN_ROWS; k++) {
                if (contains(scanRowRect(k), x, y)) return Widget::ScanRow0 + k;
            }
            if (contains(SCAN_BACK_BTN, x, y)) return Widget::ScanBack;
            if (contains(SCAN_PREV_BTN, x, y)) return Widget::ScanPrev;
            if (contains(SCAN_NEXT_BTN, x, y)) return Widget::ScanNext;
            if (contains(SCAN_MANUAL_BTN, x, y)) return Widget::ScanManual;
            if (contains(SCAN_RESCAN_BTN, x, y)) return Widget::ScanRescan;
            break;
        case Page::Calibrate:
            break;
    }
    return Widget::None;
}

// =====================================================================
//  ข้อมูลที่หน้าจอใช้ (อ่านอย่างเดียว)
// =====================================================================
bool dtcBusy()
{
    return snapshotDtc().state == DtcState::Busy;
}

// จำนวนรหัสที่แสดงบนแท็บ FAULT (ระหว่างอ่านใหม่ยังแสดงจำนวนเดิม)
int faultCount()
{
    const DtcData d = snapshotDtc();
    return (d.state == DtcState::Done || d.state == DtcState::Busy) ? d.count : 0;
}

int dtcPages()
{
    const DtcData d = snapshotDtc();
    return d.state == DtcState::Done ? max(1, (d.count + DTC_ROWS - 1) / DTC_ROWS) : 1;
}

bool blinkPhase(uint32_t now)
{
    return (now / 500) % 2 == 0;  // สลับทุก 0.5 วินาที = กะพริบ 1 Hz
}

// ปุ่มทั่วไป: primary = พื้น accent, enabled = false สีจางและไม่ใช้ accent
void drawButton(const Rect &r, const uint8_t *font, const char *label, bool enabled, bool pressed, bool primary)
{
    uint16_t bg = T->surf, fg = enabled ? T->text : T->line, border = T->line;
    if (primary && enabled) bg = border = T->accent, fg = T->onAccent;
    if (pressed && enabled) bg = border = T->track, fg = T->text;
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    if (border != bg) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, border);
    textAt(font, label, r.x + r.w / 2, r.y + r.h / 2 + (font == FONT_14B ? 5 : 4), C_BASELINE, fg, bg);
}

// =====================================================================
//  สถิติ, trip, กราฟ, จับเวลา 0-100 และแถบเตือน (คำนวณใน loop ทุก 200 ms จากค่าจริง)
// =====================================================================
uint16_t toastColor(ToastKind k)
{
    return k == ToastKind::Ok ? T->ok : (k == ToastKind::Warn ? T->warn : T->bad);
}

void showToast(ToastKind kind, const char *text, const char *sub, bool sound)
{
    toast.active = true;
    toast.kind = kind;
    strlcpy(toast.text, text, sizeof(toast.text));
    strlcpy(toast.sub, sub, sizeof(toast.sub));
    toast.at = millis();
    drawnToast = NEVER;
    if (sound) playSound(Sound::Toast);
    Serial.printf("[TOAST] %s  %s\n", text, sub);
}

void updateZeroTimer(uint32_t now, float speed)
{
    switch (zeroState) {
        case ZeroState::Off:
        case ZeroState::Done:
            break;
        case ZeroState::Armed:
            if (!isnan(speed) && speed < 0.5f) zeroState = ZeroState::Ready;
            break;
        case ZeroState::Ready:
            if (!isnan(speed) && speed >= 0.5f) {
                zeroState = ZeroState::Run;
                zeroStartAt = now;
                Serial.println("[0-100] timer started");
            }
            break;
        case ZeroState::Run:
            if (isnan(speed)) {
                zeroState = ZeroState::Armed;  // ค่าหายระหว่างจับเวลา: ต้องจอดใหม่
                Serial.println("[0-100] no speed data: run cancelled");
            } else if (speed < 0.5f) {
                zeroState = ZeroState::Ready;  // หยุดก่อนถึง 100: เริ่มใหม่เมื่อออกตัวอีกครั้ง
            } else if (speed >= 100.0f) {
                zeroResult = (now - zeroStartAt) / 1000.0f;
                const bool best = isnan(zeroBest) || zeroResult < zeroBest;
                if (best) zeroBest = zeroResult;
                zeroState = ZeroState::Done;
                char text[24], sub[16];
                snprintf(text, sizeof(text), "0–100 IN %.1f s", zeroResult);
                if (best) strlcpy(sub, "NEW BEST", sizeof(sub));
                else snprintf(sub, sizeof(sub), "BEST %.1f", zeroBest);
                showToast(ToastKind::Ok, text, sub, false);
                playSound(Sound::Confirm);
            }
            break;
    }
}

void updateStats(uint32_t now)
{
    const float dt = lastStatsAt == 0 ? 0.0f : min(1.0f, (now - lastStatsAt) / 1000.0f);
    lastStatsAt = now;
    const Rect2View view = getView(now);
    for (int i = 0; i < P_COUNT; i++) {
        const float v = view.v[i];
        if (isnan(v)) continue;
        PidStats &s = stats[i];
        s.min = isnan(s.min) ? v : min(s.min, v);
        s.max = isnan(s.max) ? v : max(s.max, v);
        s.sum += v;
        s.n++;
    }
    const float rpm = view.v[P_RPM], speed = view.v[P_SPEED];
    if (!isnan(rpm) && rpm > peakRpm) peakRpm = rpm;
    if (view.real) {  // trip นับเฉพาะเวลาที่ได้ค่าจริงจากรถ
        trip.timeS += dt;
        if (!isnan(speed)) {
            if (speed > 1.0f) trip.movingS += dt;
            trip.distKm += speed * dt / 3600.0f;
            trip.maxSpeed = max(trip.maxSpeed, speed);
        }
        if (!isnan(rpm)) trip.maxRpm = max(trip.maxRpm, rpm);
    }
    if (now - lastHistoryAt >= 1000) {
        lastHistoryAt = now;
        for (int i = 0; i < P_COUNT; i++) history[i][historyHead] = view.v[i];
        historyHead = (historyHead + 1) % HISTORY_LEN;
        if (historyCount < HISTORY_LEN) historyCount++;
        historyVersion++;
    }
    updateZeroTimer(now, speed);

    // แถบเตือน: เตือนเมื่อข้ามเกณฑ์ลงมา เตือนอีกได้เมื่อค่ากลับขึ้นเหนือเกณฑ์ + ช่วงกันแกว่ง
    static bool battLow = false, fuelLow = false;
    char sub[16];
    const float volt = view.v[P_VOLT], fuel = view.v[P_FUEL];
    if (!isnan(volt)) {
        if (!battLow && volt < app::TOAST_BATTERY_LOW_V) {
            battLow = true;
            snprintf(sub, sizeof(sub), "%.1f V", volt);
            showToast(ToastKind::Warn, "BATTERY LOW", sub, true);
        } else if (battLow && volt >= app::TOAST_BATTERY_LOW_V + 0.3f) {
            battLow = false;
        }
    }
    if (!isnan(fuel)) {
        if (!fuelLow && fuel < app::TOAST_FUEL_LOW_PCT) {
            fuelLow = true;
            snprintf(sub, sizeof(sub), "%d %%", static_cast<int>(lroundf(fuel)));
            showToast(ToastKind::Warn, "FUEL LOW", sub, true);
        } else if (fuelLow && fuel >= app::TOAST_FUEL_LOW_PCT + 3.0f) {
            fuelLow = false;
        }
    }
}

void resetFocusStats(int idx)
{
    stats[idx] = PidStats();
    if (idx == P_RPM) peakRpm = 0;
}

void resetTrip()
{
    trip = TripData();
}

// =====================================================================
//  แถบสถานะ (บน 0-21) และแถบแท็บ (ล่าง 208-239)
// =====================================================================
// ซ้าย: IP ของหน้าเว็บ (ยังไม่มี = OBD-II) + "WAITING FOR ECU" เมื่อต่อ BT แล้วแต่ยังไม่มีค่าจากรถ
// ขวา: ป้ายสถานะ BT แบบ pill  เขียว = BT CONNECTED,  แดง = BT DISCONNECTED
void updateStatusBar(uint32_t now)
{
    // ค่าที่วาดจริงผ่านตัวหน่วง (debounce): สถานะที่สลับไปมาถี่ ๆ (BT ต่อ/หลุดสลับ, ECU ตอบสลับเว้น) ต้องคงที่ >= STATUS_DEBOUNCE_MS
    // จึงจะเปลี่ยนที่จอ ไม่งั้นแถบจะกะพริบทุกครั้งที่สถานะสะดุด
    constexpr uint32_t STATUS_DEBOUNCE_MS = 1000;
    static bool shownConn = false, shownWait = false;
    static uint32_t connSince = 0, waitSince = 0;
    const bool rawConn = btConnected();
    const bool rawWait = rawConn && !isRealData(now);
    const bool force = drawnStatus == NEVER;
    if (force || rawConn == shownConn) {
        shownConn = rawConn;
        connSince = 0;
    } else {
        if (connSince == 0) connSince = now;
        if (now - connSince >= STATUS_DEBOUNCE_MS) { shownConn = rawConn; connSince = 0; }
    }
    if (force || rawWait == shownWait) {
        shownWait = rawWait;
        waitSince = 0;
    } else {
        if (waitSince == 0) waitSince = now;
        if (now - waitSince >= STATUS_DEBOUNCE_MS) { shownWait = rawWait; waitSince = 0; }
    }
    const bool connected = shownConn;
    const bool waiting = shownConn && shownWait;

    uint32_t h = (connected ? 1 : 0) | (waiting ? 2 : 0);
    for (const char *p = netLabel; *p != ' '; p++) h = h * 31 + static_cast<uint8_t>(*p);
    const int key = static_cast<int>(h & 0x3FFFFFFF);
    if (key == drawnStatus) return;
    const bool pillChanged = force || connected != drawnConn;
    drawnStatus = key;
    drawnConn = connected;

    const char *text = connected ? "BT CONNECTED" : "BT DISCONNECTED";
    const uint16_t color = connected ? T->ok : T->bad;
    const int16_t w = textWidthIn(FONT_10B, text);
    const int16_t pillW = w + 22;  // จุด 6 + ช่องว่าง + ข้อความ
    const int16_t x = SCREEN_W - 6 - pillW;

    if (pillChanged) {
        // ล้างทั้งแถบเฉพาะตอนป้าย BT เปลี่ยน (ความกว้างป้ายต่างกัน) ส่วนข้อความซ้ายล้างเฉพาะฝั่งซ้ายด้านล่าง
        tft.fillRect(0, 0, SCREEN_W, STATUS_H, T->bg);
        tft.drawFastHLine(0, STATUS_H, SCREEN_W, T->line);
        tft.fillRoundRect(x, 3, pillW, 16, 8, T->surf);
        tft.drawRoundRect(x, 3, pillW, 16, 8, color);
        tft.fillSmoothCircle(x + 10, 11, 3, color, T->surf);
        textAt(FONT_10B, text, x + 17, 15, L_BASELINE, color, T->surf);
    } else {
        tft.fillRect(0, 0, x - 1, STATUS_H, T->bg);  // ป้าย BT ไม่เปลี่ยน: ไม่แตะ
    }

    const int16_t tw = textAt(FONT_11B, netLabel[0] != ' ' ? netLabel : "OBD-II", 8, 15, L_BASELINE, T->text, T->bg);
    const int waitX = 8 + tw + 8;
    const int waitW = x - 8 - waitX;
    if (waiting && waitW >= 30) {
        char wait[24];
        fitText(FONT_10S, "WAITING FOR ECU", waitW, wait, sizeof(wait));
        textAt(FONT_10S, wait, waitX, 15, L_BASELINE, T->muted, T->bg);
    }
}

// ไอคอนเฟือง (แท็บ SET): ฟัน 8 ซี่รอบวงกลม เจาะรูกลาง
void drawGear(int cx, int cy, uint16_t color, uint16_t bg)
{
    for (int k = 0; k < 8; k++) {
        const float a = k * PI / 4;
        tft.fillRect(cx + static_cast<int>(lroundf(7 * cosf(a))) - 1, cy + static_cast<int>(lroundf(7 * sinf(a))) - 1, 3, 3, color);
    }
    tft.fillSmoothCircle(cx, cy, 6, color, bg);
    tft.fillSmoothCircle(cx, cy, 2, bg, color);
}

// แท็บที่เลือก: สี text และแถบ accent 3px ด้านบน; แท็บ FAULT มีป้ายจำนวนรหัส
void drawTab(int index)
{
    const Rect r = tabRect(index);
    const Widget w = Widget::TabDrive + index;
    const bool active = static_cast<int>(page) == index;
    const uint16_t bg = pressedWidget == w ? T->track : T->bar;
    const uint16_t fg = active ? T->text : T->muted;
    tft.fillRect(r.x, r.y, r.w, r.h, bg);
    if (active) tft.fillRect(r.x, r.y, r.w, 3, T->accent);
    if (index == 4) {
        drawGear(r.x + r.w / 2, r.y + 17, fg, bg);
        return;
    }
    char badge[6] = "";
    if (index == 3) {
        const int n = faultCount();
        if (n > 0) snprintf(badge, sizeof(badge), "%d", n);
    }
    const int16_t lw = textWidthIn(FONT_11B, TAB_NAMES[index]);
    const int16_t bw = badge[0] != '\0' ? max<int16_t>(14, textWidthIn(FONT_10B, badge) + 8) : 0;
    const int16_t x0 = r.x + (r.w - (lw + (bw > 0 ? 4 + bw : 0))) / 2;
    textAt(FONT_11B, TAB_NAMES[index], x0, r.y + 21, L_BASELINE, fg, bg);
    if (bw > 0) {
        const int16_t bx = x0 + lw + 4;
        tft.fillRoundRect(bx, r.y + 10, bw, 14, 7, T->bad);
        textAt(FONT_10B, badge, bx + bw / 2, r.y + 21, C_BASELINE, T->onBad, T->bad);
    }
}

void drawTabBar()
{
    tft.drawFastHLine(0, TAB_LINE_Y, SCREEN_W, T->line);
    for (int i = 0; i < TAB_COUNT; i++) drawTab(i);
    drawnFaultBadge = faultCount();
}

void updateFaultBadge()
{
    const int n = faultCount();
    if (n == drawnFaultBadge) return;
    drawnFaultBadge = n;
    drawTab(3);
}

// =====================================================================
//  แถบเตือน (toast): ทับเนื้อหาที่ y 32-54 ครั้งละ 1 อัน หายเองใน 4 วินาที หรือแตะเพื่อปิด
// =====================================================================
void drawPageStatic();  // กำหนดในส่วน "เปลี่ยนหน้า"

void drawToast()
{
    const Rect &r = TOAST_RECT;
    const uint16_t c = toastColor(toast.kind);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, T->surf);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, c);
    tft.fillRoundRect(r.x, r.y, 4, r.h, 2, c);
    textAt(FONT_11B, toast.text, r.x + 12, r.y + 16, L_BASELINE, c, T->surf);
    if (toast.sub[0] != '\0') textAt(FONT_10S, toast.sub, r.x + r.w - 8, r.y + 15, R_BASELINE, T->muted, T->surf);
}

// ปิด toast แล้ววาดส่วนคงที่ของหน้าใหม่ (ค่าตัวเลขจะวาดเองในรอบถัดไป)
void hideToast()
{
    if (!toast.active) return;
    toast.active = false;
    if (hasTabs(page) && !alertActive) {
        tft.fillRect(TOAST_RECT.x, TOAST_RECT.y, TOAST_RECT.w, TOAST_RECT.h, T->bg);
        drawPageStatic();
    }
}

void updateToast(uint32_t now)
{
    if (!toast.active) return;
    if (now - toast.at >= app::TOAST_SHOW_MS) {
        hideToast();
        return;
    }
    const int key = static_cast<int>(toast.at & 0x3FFFFFFF);
    if (key == drawnToast && !toastDirty) return;
    drawnToast = key;
    toastDirty = false;
    drawToast();
}

// =====================================================================
//  หน้า DRIVE: shift light + มาตรวัดรอบ 270° + ความเร็ว + การ์ด 4 ใบ
// =====================================================================
int drawnPeakText = NEVER;

// มุมแบบ SVG (0° = ทิศ 3 นาฬิกา หมุนตามเข็ม) ของรอบเครื่อง: 0 รอบ = 135°, 8000 รอบ = 405°
float rpmAngleRad(float rpm)
{
    return (135.0f + 270.0f * constrain(rpm / app::RPM_MAX, 0.0f, 1.0f)) * DEG_TO_RAD;
}

void gaugePoint(float a, float r, float &x, float &y)
{
    x = DRV_CX + r * cosf(a);
    y = DRV_CY + r * sinf(a);
}

// shift light: 15 ช่อง ติดจาก (shift - 1500) ถึง shift รอบ, ถึง shift = กะพริบ bad 4 Hz
void updateShiftLight(uint32_t now)
{
    const float rpm = getView(now).v[P_RPM];
    const float start = static_cast<float>(shiftRpm - app::SHIFT_RANGE_RPM);
    const bool flash = !isnan(rpm) && rpm >= shiftRpm;
    const int lit = isnan(rpm) ? 0 : constrain(static_cast<int>(ceilf((rpm - start) / app::SHIFT_RANGE_RPM * SHIFT_SEGS)), 0, SHIFT_SEGS);
    const int key = flash ? 100 + static_cast<int>((now / 125) % 2) : lit;
    if (key == drawnShift) return;
    drawnShift = key;
    for (int i = 0; i < SHIFT_SEGS; i++) {
        const int x0 = SHIFT_BAR.x + (i * (SHIFT_BAR.w + 2)) / SHIFT_SEGS;
        const int x1 = SHIFT_BAR.x + ((i + 1) * (SHIFT_BAR.w + 2)) / SHIFT_SEGS - 2;
        uint16_t c = T->shiftOff;
        if (flash) c = key == 100 ? T->bad : T->shiftOff;
        else if (i < lit) c = i < 5 ? T->ok : (i < 9 ? T->accent : T->bad);
        tft.fillRoundRect(x0, SHIFT_BAR.y, x1 - x0, SHIFT_BAR.h, 1, c);
    }
}

// ขีดทุก 500 รอบ (ขีดพันรอบยาวกว่า) r 57-63 และตัวเลขหน้าปัด 0-8 (พันรอบ) ที่ r 47; ตั้งแต่ redline เป็นสี bad
// วาดซ้ำหลังล้างพื้นที่ตัวเลข
void drawGaugeTicks()
{
    for (int k = 0; k <= 16; k++) {
        const bool major = k % 2 == 0;
        const bool red = k * 500.0f >= app::RPM_RED;
        const float a = rpmAngleRad(k * 500.0f);
        float x0, y0, x1, y1;
        gaugePoint(a, 63, x0, y0);
        gaugePoint(a, major ? 57 : 60, x1, y1);
        tft.drawWideLine(x0, y0, x1, y1, major ? 1.8f : 1.0f, red ? T->bad : (major ? T->muted : T->line), T->bg);
        if (major) {
            float lx, ly;
            gaugePoint(a, 47, lx, ly);
            char n[2] = {static_cast<char>('0' + k / 2), '\0'};
            textAt(FONT_10S, n, static_cast<int>(lroundf(lx)), static_cast<int>(lroundf(ly)) + 4, C_BASELINE, red ? T->bad : T->muted, T->bg);
        }
    }
}

void drawGaugeStatic()
{
    drawGaugeTicks();
    textAt(FONT_11S, "KM/H", DRV_CX, DRV_CY + 40, C_BASELINE, T->muted, T->bg);
}

// ช่วง a..b (องศาของสเกล 0..270) ของวงโค้งหลัก r 67-79
void gaugeArc(int a, int b, uint16_t color)
{
    if (b <= a) return;
    tft.drawArc(DRV_CX, DRV_CY, GAUGE_R_OUT, GAUGE_R_IN, GAUGE_START_DEG + a, GAUGE_START_DEG + b, color, T->bg, false);
}

// ส่วนที่ยังไม่ถึงรอบ: ราง track และโซนแดง (badSoft) ตั้งแต่ redline เป็นต้นไป
void gaugeTrack(int a, int b)
{
    const int redDeg = static_cast<int>(GAUGE_SWEEP_DEG * app::RPM_RED / app::RPM_MAX);
    gaugeArc(a, min(b, redDeg), T->track);
    gaugeArc(max(a, redDeg), b, T->badSoft);
}

// peak: ขีดสี text จาก r 66 ถึง r 76 ที่มุมของรอบสูงสุด (วาดทับวงโค้ง)
void drawPeakMarker()
{
    if (peakRpm <= 0) return;
    float x0, y0, x1, y1;
    const float a = rpmAngleRad(peakRpm);
    gaugePoint(a, 66, x0, y0);
    gaugePoint(a, 80, x1, y1);
    tft.drawWideLine(x0, y0, x1, y1, 2.5f, T->text, T->track);
}

// กดค้างมาตรวัด: วงแหวน accent r 45-47 เริ่มด้านบน หมุนตามเข็มตามเวลาที่กด (0 = ลบวงแหวน)
void drawGaugeHoldRing(int progressPercent)
{
    tft.drawArc(DRV_CX, DRV_CY, HOLD_RING_R + 1, HOLD_RING_R - 1, 0, 360, T->bg, T->bg, false);
    if (progressPercent > 0) {
        const int end = 180 + 360 * progressPercent / 100;  // drawArc: 0 = ด้านล่าง, 180 = ด้านบน
        if (end <= 360) {
            tft.drawArc(DRV_CX, DRV_CY, HOLD_RING_R + 1, HOLD_RING_R - 1, 180, end, T->accent, T->bg, false);
        } else {
            tft.drawArc(DRV_CX, DRV_CY, HOLD_RING_R + 1, HOLD_RING_R - 1, 180, 360, T->accent, T->bg, false);
            tft.drawArc(DRV_CX, DRV_CY, HOLD_RING_R + 1, HOLD_RING_R - 1, 0, end - 360, T->accent, T->bg, false);
        }
    }
    // วงแหวนผ่านพื้นที่ตัวเลข: ให้วาดตัวเลขใหม่
    drawnSpeed = drawnRpmText = NEVER;
}

void drawTileStatic(int i)
{
    const Rect r = tileRect(i);
    drawCardBox(r);
    textAt(FONT_10S, PID_INFO[TILE_PID[i]].name, r.x + 8, r.y + 15, L_BASELINE, T->muted, T->surf);
    textAt(FONT_10S, TILE_TAG[i], r.x + r.w - 8, r.y + 15, R_BASELINE, T->muted, T->surf);
}

// คืน true ถ้ามีการวาด
bool updateTile(int i, const Rect2View &view)
{
    const Rect r = tileRect(i);
    const int idx = TILE_PID[i];
    const float v = view.v[idx];
    const bool na = view.na[idx];
    const Level level = pidLevel(idx, v);
    uint32_t h = na ? 1u : (isnan(v) ? 2u : static_cast<uint32_t>(lroundf(v * 10.0f)) + 16u);
    h = h * 31 + static_cast<uint32_t>(level);
    const int key = static_cast<int>(h & 0x3FFFFFFF);
    if (key == drawnTile[i]) return false;
    drawnTile[i] = key;

    // ขอบการ์ด: ค่าอันตราย = สี bad
    const uint16_t border = level == Level::Bad ? T->bad : (T->cardOutline ? T->cardLine : T->surf);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, border);

    tft.fillRect(r.x + 2, r.y + 18, r.w - 4, 22, T->surf);
    if (na) {  // รถคันนี้ไม่รองรับค่านี้ (ไม่ใช่สัญญาณหลุด)
        textAt(FONT_14B, "N/A", r.x + 8, r.y + 38, L_BASELINE, T->muted, T->surf);
    } else {
        char text[16];
        formatPid(idx, v, text, sizeof(text));
        const int16_t vw = textAt(FONT_24S, text, r.x + 8, r.y + 38, L_BASELINE, valueColor(idx, v), T->surf);
        if (!isnan(v)) textAt(FONT_11S, PID_INFO[idx].unit, r.x + 8 + vw + 4, r.y + 38, L_BASELINE, T->muted, T->surf);
    }
    // แถบระดับชิดขอบล่างของการ์ด: ปกติ accent, เฝ้าดู/อันตรายใช้สีเกณฑ์
    const float frac = (na || isnan(v)) ? NAN : (v - PID_INFO[idx].lo) / (PID_INFO[idx].hi - PID_INFO[idx].lo);
    const uint16_t barColor = (level == Level::Warn || level == Level::Bad) ? levelColor(level) : T->accent;
    tft.fillRect(r.x + 3, r.y + r.h - 6, r.w - 6, 4, T->track);
    if (!isnan(frac)) {
        const int fw = max(2, static_cast<int>(lroundf(constrain(frac, 0.0f, 1.0f) * (r.w - 6))));
        tft.fillRect(r.x + 3, r.y + r.h - 6, fw, 4, barColor);
    }
    return true;
}

void updateDashboard(uint32_t now)
{
    const Rect2View view = getView(now);
    char text[24];
    bool ticksDirty = false;
    bool drew = false;

    // ความเร็ว (ตัวเลขใหญ่กลางวงโค้ง)
    const float speed = view.v[P_SPEED];
    const int speedKey = isnan(speed) ? -1 : static_cast<int>(lroundf(speed));
    if (speedKey != drawnSpeed) {
        if (speedKey < 0) snprintf(text, sizeof(text), "--");
        else snprintf(text, sizeof(text), "%d", speedKey);
        tft.fillRect(SPEED_BOX.x, SPEED_BOX.y, SPEED_BOX.w, SPEED_BOX.h, T->bg);
        ticksDirty = true;
        textAt(FONT_50S, text, DRV_CX, SPEED_BASE_Y, C_BASELINE, speedKey < 0 ? T->muted : T->text, T->bg);
        drawnSpeed = speedKey;
    }

    // รอบเครื่อง: Dark = accent, Light = text, ถึง redline = bad
    const float rpm = view.v[P_RPM];
    const bool red = !isnan(rpm) && rpm >= app::RPM_RED;
    const int rpmKey = isnan(rpm) ? -1 : static_cast<int>(lroundf(rpm / 10.0f)) * 10;
    const int rpmTextKey = rpmKey * 2 + (red ? 1 : 0);
    if (rpmTextKey != drawnRpmText) {
        if (rpmKey < 0) snprintf(text, sizeof(text), "--");
        else formatThousands(rpmKey, text, sizeof(text));
        const int16_t vw = textWidthIn(FONT_18B, text);
        const int16_t lw = textWidthIn(FONT_10S, "RPM");
        const int16_t x0 = DRV_CX - (vw + 4 + lw) / 2;
        tft.fillRect(RPM_BOX.x, RPM_BOX.y, RPM_BOX.w, RPM_BOX.h, T->bg);
        textAt(FONT_18B, text, x0, 196, L_BASELINE, rpmKey < 0 ? T->muted : (red ? T->bad : T->rpmNormal), T->bg);
        textAt(FONT_10S, "RPM", x0 + vw + 4, 196, L_BASELINE, T->muted, T->bg);
        drawnRpmText = rpmTextKey;
    }
    if (ticksDirty) drawGaugeTicks();

    // วงโค้ง: วาดเฉพาะช่วงที่เปลี่ยน; สีเปลี่ยนหรือ peak ย้าย = วาดใหม่ทั้งเส้นแล้ววาด peak ทับ
    const int sweep = isnan(rpm) ? 0 : static_cast<int>(constrain(rpm / app::RPM_MAX, 0.0f, 1.0f) * GAUGE_SWEEP_DEG);
    const uint16_t arcColor = red ? T->bad : T->accent;
    const int colorKey = red ? 1 : 0;
    const int peakMarkKey = peakRpm > 0 ? static_cast<int>(lroundf(peakRpm / 25.0f)) : -1;
    int lo = 0, hi = GAUGE_SWEEP_DEG;  // ช่วงองศาที่วาดใหม่
    if (drawnSweep == NEVER || colorKey != drawnArcColor || peakMarkKey != drawnPeak) {
        gaugeArc(0, sweep, arcColor);
        gaugeTrack(sweep, GAUGE_SWEEP_DEG);
    } else if (sweep > drawnSweep) {
        gaugeArc(drawnSweep, sweep, arcColor);
        lo = drawnSweep, hi = sweep;
    } else if (sweep < drawnSweep) {
        gaugeTrack(sweep, drawnSweep);
        lo = sweep, hi = drawnSweep;
    } else {
        hi = -1;
    }
    if (hi >= 0) {
        drawPeakMarker();
        // องศา 114-155 ของสเกล = ส่วนบนของวงโค้งที่อยู่ใต้ toast (รวมขีด peak ถ้าอยู่ช่วงนั้น)
        const int peakDeg = static_cast<int>(GAUGE_SWEEP_DEG * constrain(peakRpm / app::RPM_MAX, 0.0f, 1.0f));
        if ((lo <= 155 && hi >= 114) || (peakRpm > 0 && peakDeg >= 114 && peakDeg <= 155)) drew = true;
    }
    drawnSweep = sweep;
    drawnArcColor = colorKey;
    drawnPeak = peakMarkKey;

    for (int i = 0; i < TILE_COUNT; i++) {
        if (updateTile(i, view) && tileRect(i).y < TOAST_RECT.y + TOAST_RECT.h) drew = true;  // การ์ดชนกับ toast: วาด toast ทับอีกครั้ง
    }
    if (drew) toastDirty = true;
}

// =====================================================================
//  หน้า FOCUS: ค่าเดียวขนาดใหญ่ + MIN/AVG/MAX + กราฟ 60 วินาที
// =====================================================================
int drawnFocusValue = NEVER;

void drawFocusResetButton(int progressPercent)
{
    drawHoldButton(FOCUS_RESET_BTN, "RESET", T->surf, T->line, 1, T->text, T->line, progressPercent, "");
}

void drawFocusStatic()
{
    drawButton(FOCUS_PREV_BTN, FONT_14B, "‹", true, pressedWidget == Widget::FocusPrev, false);
    drawButton(FOCUS_NEXT_BTN, FONT_14B, "›", true, pressedWidget == Widget::FocusNext, false);
    textAt(FONT_10S, "−60 s", 6, 196, L_BASELINE, T->muted, T->bg);
    textAt(FONT_10S, "NOW", 204, 196, R_BASELINE, T->muted, T->bg);
    drawFocusResetButton(0);
}

void drawFocusGraph(int idx)
{
    const Rect &g = FOCUS_GRAPH;
    const PidInfo &info = PID_INFO[idx];
    drawCardBox(g);
    auto yOf = [&](float v) {
        const float f = constrain((v - info.lo) / (info.hi - info.lo), 0.0f, 1.0f);
        return g.y + g.h - 5 - (g.h - 10) * f;
    };
    float limit = NAN;
    if (idx == P_COOL) limit = static_cast<float>(coolLimit);
    if (idx == P_RPM) limit = app::RPM_RED;
    if (!isnan(limit)) {  // เส้นประที่เกณฑ์
        const int ly = static_cast<int>(lroundf(yOf(limit)));
        for (int x = g.x + 4; x < g.x + g.w - 4; x += 6) tft.drawFastHLine(x, ly, 3, T->bad);
    }
    char text[12];
    formatPid(idx, info.hi, text, sizeof(text));
    textAt(FONT_10S, text, g.x + 6, g.y + 13, L_BASELINE, T->muted, T->surf);
    formatPid(idx, info.lo, text, sizeof(text));
    textAt(FONT_10S, text, g.x + 6, g.y + g.h - 4, L_BASELINE, T->muted, T->surf);

    // เส้นกราฟ: จุดล่าสุดอยู่ขวาสุด, ช่วงที่ไม่มีค่า (NaN) เว้นว่าง
    const float step = (g.w - 8) / static_cast<float>(HISTORY_LEN - 1);
    const float x0 = g.x + g.w - 4 - (historyCount - 1) * step;
    const int last = (historyHead - 1 + HISTORY_LEN) % HISTORY_LEN;
    const Level lastLevel = pidLevel(idx, history[idx][last]);
    const uint16_t color = (lastLevel == Level::Warn || lastLevel == Level::Bad) ? levelColor(lastLevel) : T->accent;
    float px = NAN, py = NAN;
    for (int i = 0; i < historyCount; i++) {
        const float v = history[idx][(historyHead - historyCount + i + HISTORY_LEN) % HISTORY_LEN];
        if (isnan(v)) {
            px = NAN;
            continue;
        }
        const float x = x0 + i * step, y = yOf(v);
        if (!isnan(px)) tft.drawWideLine(px, py, x, y, 1.6f, color, T->surf);
        px = x;
        py = y;
    }
    if (!isnan(px) && !isnan(history[idx][last])) tft.fillSmoothCircle(static_cast<int>(lroundf(px)), static_cast<int>(lroundf(py)), 2, T->accent, T->surf);
}

void updateFocusPage(uint32_t now)
{
    const int idx = FOCUS_ORDER[focusPid];
    const PidInfo &info = PID_INFO[idx];
    if (drawnFocusKey != focusPid) {  // เปลี่ยนค่าที่ดู: หัว + จุดบอกตำแหน่ง
        drawnFocusKey = focusPid;
        tft.fillRect(44, 26, 232, 31, T->bg);
        textAt(FONT_12B, info.name, GAUGE_CX, 45, C_BASELINE, T->text, T->bg);
        for (int i = 0; i < P_COUNT; i++) tft.fillCircle(GAUGE_CX - 21 + i * 6, 53, 1, i == focusPid ? T->accent : T->line);
        drawnFocusValue = drawnFocusStats = NEVER;
        drawnFocusGraph = 0xFFFFFFFF;
    }

    const Rect2View view = getView(now);
    const float v = view.v[idx];
    const bool na = view.na[idx];
    const int vk = na ? -2 : (isnan(v) ? -1 : static_cast<int>(lroundf(v * 10.0f)) * 4 + static_cast<int>(pidLevel(idx, v)));
    if (vk != drawnFocusValue) {
        drawnFocusValue = vk;
        char text[16];
        tft.fillRect(FOCUS_VALUE_BOX.x, FOCUS_VALUE_BOX.y, FOCUS_VALUE_BOX.w, FOCUS_VALUE_BOX.h, T->bg);
        if (na) {
            textAt(FONT_12B, "NOT SUPPORTED", 10, 100, L_BASELINE, T->muted, T->bg);
        } else {
            formatPid(idx, v, text, sizeof(text));
            textAt(FONT_50S, text, 10, 106, L_BASELINE, valueColor(idx, v), T->bg);
        }
        textAt(FONT_11S, info.unit, 12, 120, L_BASELINE, T->muted, T->bg);
    }

    const PidStats &s = stats[idx];
    const float avg = s.n > 0 ? static_cast<float>(s.sum / s.n) : NAN;
    const float vals[3] = {s.min, avg, s.max};
    uint32_t h = 17;
    for (float x : vals) h = h * 31 + (isnan(x) ? 7u : static_cast<uint32_t>(lroundf(x * 10.0f)));
    const int sk = static_cast<int>(h & 0x3FFFFFFF);
    if (sk != drawnFocusStats) {
        drawnFocusStats = sk;
        drawCardBox(FOCUS_STATS);
        static constexpr const char *LABELS[3] = {"MIN", "AVG", "MAX"};
        for (int i = 0; i < 3; i++) {
            char text[12];
            formatPid(idx, vals[i], text, sizeof(text));
            const int y = FOCUS_STATS.y + 15 + i * 14;
            textAt(FONT_10S, LABELS[i], FOCUS_STATS.x + 8, y, L_BASELINE, T->muted, T->surf);
            textAt(FONT_11B, text, FOCUS_STATS.x + FOCUS_STATS.w - 8, y, R_BASELINE, T->text, T->surf);
        }
    }

    if (drawnFocusGraph != historyVersion) {
        drawnFocusGraph = historyVersion;
        drawFocusGraph(idx);
        toastDirty = true;
    }
}

// =====================================================================
//  หน้า TRIP: สถิติการเดินทาง 6 การ์ด + จับเวลา 0-100 km/h
// =====================================================================
constexpr const char *TRIP_LABELS[6] = {"TIME", "DIST", "AVG", "MAX", "MAX RPM", "MOVING"};
constexpr const char *TRIP_UNITS[6] = {"", "km", "km/h", "km/h", "", ""};
char tripDrawn[6][14];
int drawnZeroValue = NEVER;

void formatDuration(float seconds, char *out, size_t size)
{
    const int t = static_cast<int>(seconds);
    if (t >= 3600) snprintf(out, size, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(out, size, "%d:%02d", t / 60, t % 60);
}

void tripText(int i, char *out, size_t size)
{
    switch (i) {
        case 0: formatDuration(trip.timeS, out, size); break;
        case 1: snprintf(out, size, "%.1f", trip.distKm); break;
        case 2:
            if (trip.movingS > 5.0f) snprintf(out, size, "%d", static_cast<int>(lroundf(trip.distKm / (trip.movingS / 3600.0f))));
            else strlcpy(out, "--", size);
            break;
        case 3: snprintf(out, size, "%d", static_cast<int>(lroundf(trip.maxSpeed))); break;
        case 4: formatThousands(static_cast<int>(lroundf(trip.maxRpm / 10.0f)) * 10, out, size); break;
        default: formatDuration(trip.movingS, out, size); break;
    }
}

void drawTripResetButton(int progressPercent)
{
    drawHoldButton(TRIP_RESET_BTN, "RESET TRIP", T->bg, T->bad, 2, T->bad, T->badSoft, progressPercent);
}

void drawZeroArmButton(bool pressed)
{
    const bool arm = zeroState == ZeroState::Off || zeroState == ZeroState::Done;
    drawButton(ZERO_ARM_BTN, FONT_12B, arm ? "ARM" : "CANCEL", true, pressed, arm);
}

void drawTripStatic()
{
    for (int i = 0; i < 6; i++) {
        const Rect r = tripCardRect(i);
        drawCardBox(r);
        textAt(FONT_10S, TRIP_LABELS[i], r.x + 7, r.y + 15, L_BASELINE, T->muted, T->surf);
        if (TRIP_UNITS[i][0] != '\0') textAt(FONT_10S, TRIP_UNITS[i], r.x + r.w - 7, r.y + 38, R_BASELINE, T->muted, T->surf);
        tripDrawn[i][0] = '\x01';  // บังคับวาดค่าใหม่
    }
    drawTripResetButton(0);
    drawnZeroKey = drawnZeroValue = NEVER;
}

void updateZeroCard(uint32_t now)
{
    const Rect &c = ZERO_CARD;
    const bool blinkOn = zeroState == ZeroState::Ready && blinkPhase(now);
    const int key = static_cast<int>(zeroState) | (blinkOn ? 8 : 0) | ((isnan(zeroBest) ? 0 : static_cast<int>(lroundf(zeroBest * 10.0f)) + 1) << 4);
    if (key != drawnZeroKey) {
        drawnZeroKey = key;
        drawCardBox(c);
        textAt(FONT_10S, "0–100 km/h", c.x + 8, c.y + 15, L_BASELINE, T->muted, T->surf);
        const char *label = "OFF";
        uint16_t color = T->muted;
        switch (zeroState) {
            case ZeroState::Armed: label = "STOP FIRST"; color = T->warn; break;
            case ZeroState::Ready: label = "READY"; color = T->ok; break;
            case ZeroState::Run: label = "GO"; color = T->accent; break;
            case ZeroState::Done: label = "RESULT"; color = T->ok; break;
            case ZeroState::Off: break;
        }
        const uint16_t pill = blinkOn ? T->accentSoft : T->track;
        tft.fillRoundRect(c.x + 8, c.y + 22, c.w - 16, 16, 2, pill);
        textAt(FONT_10B, label, c.x + c.w / 2, c.y + 34, C_BASELINE, color, pill);
        textAt(FONT_10S, "BEST", c.x + 8, c.y + 120, L_BASELINE, T->muted, T->surf);
        char best[12];
        if (isnan(zeroBest)) strlcpy(best, "--", sizeof(best));
        else snprintf(best, sizeof(best), "%.1f s", zeroBest);
        textAt(FONT_11B, best, c.x + c.w - 8, c.y + 120, R_BASELINE, T->text, T->surf);
        drawZeroArmButton(pressedWidget == Widget::ZeroArm);
        drawnZeroValue = NEVER;
        toastDirty = true;
    }

    // ตัวเลขใหญ่ + แถบความคืบหน้าตามความเร็ว
    const float speed = getView(now).v[P_SPEED];
    float t = NAN;
    if (zeroState == ZeroState::Ready) t = 0;
    if (zeroState == ZeroState::Run) t = (now - zeroStartAt) / 1000.0f;
    if (zeroState == ZeroState::Done) t = zeroResult;
    const int prog = isnan(speed) ? -1 : constrain(static_cast<int>(speed), 0, 100) / 2;
    const int vk = (isnan(t) ? 0 : static_cast<int>(t * 10.0f) + 1) * 64 + prog + 1;
    if (vk == drawnZeroValue) return;
    drawnZeroValue = vk;
    char text[12];
    if (isnan(t)) strlcpy(text, "--.-", sizeof(text));
    else snprintf(text, sizeof(text), "%.1f", t);
    tft.fillRect(c.x + 4, c.y + 44, c.w - 8, 52, T->surf);
    textAt(FONT_30B, text, c.x + c.w / 2 + 4, c.y + 78, C_BASELINE, zeroState == ZeroState::Run ? T->accent : (isnan(t) ? T->muted : T->text), T->surf);
    textAt(FONT_10S, "s", c.x + c.w - 8, c.y + 92, R_BASELINE, T->muted, T->surf);
    drawLevelBar(c.x + 8, c.y + 100, c.w - 16, (isnan(speed) || speed <= 0) ? NAN : speed / 100.0f, T->accent);
}

void updateTripPage(uint32_t now)
{
    for (int i = 0; i < 6; i++) {
        char text[14];
        tripText(i, text, sizeof(text));
        if (strcmp(text, tripDrawn[i]) == 0) continue;
        strlcpy(tripDrawn[i], text, sizeof(tripDrawn[i]));
        const Rect r = tripCardRect(i);
        const int w = TRIP_UNITS[i][0] != '\0' ? 56 : r.w - 10;
        tft.fillRect(r.x + 6, r.y + 20, w, 22, T->surf);
        textAt(FONT_20B, text, r.x + 7, r.y + 38, L_BASELINE, T->text, T->surf);
        if (i < 2) toastDirty = true;
    }
    updateZeroCard(now);
}

// =====================================================================
//  หน้า FAULT
// =====================================================================
// แถบสีหน้าแถว: bad = รหัสร้ายแรง (จุดระเบิดไม่สมบูรณ์ P03xx, น้ำหล่อเย็นร้อน), warn = อื่นๆ
bool dtcCritical(const char *code)
{
    return (code[0] == 'P' && code[1] == '0' && code[2] == '3') || strcmp(code, "P0118") == 0 || strcmp(code, "P0217") == 0;
}

const char *dtcTextTh(const char *code)
{
    for (const DtcThai &t : DTC_TH_TABLE) {
        if (strcmp(t.code, code) == 0) return t.th;
    }
    return code[0] == 'P' && code[1] >= '1' && code[1] <= '3' ? TH_DTC_MANUFACTURER : TH_DTC_GENERIC;
}

void drawDtcRow(int k, const char *code)
{
    const Rect r = {6, static_cast<int16_t>(DTC_ROW_Y + k * DTC_ROW_PITCH), 308, 34};
    drawCardBox(r);
    tft.fillRoundRect(r.x, r.y, 7, r.h, 4, dtcCritical(code) ? T->bad : T->warn);  // แถบความรุนแรง 3px
    tft.fillRect(r.x + 3, r.y + 1, 4, r.h - 2, T->surf);
    textAt(FONT_14B, code, r.x + 11, r.y + 22, L_BASELINE, T->text, T->surf);
    char line[128];
    fitText(FONT_11S, dtcText(code), 223, line, sizeof(line));
    textAt(FONT_11S, line, r.x + 77, r.y + 14, L_BASELINE, T->text, T->surf);
    fitText(FONT_10TH, dtcTextTh(code), 223, line, sizeof(line));
    textAt(FONT_10TH, line, r.x + 77, r.y + 27, L_BASELINE, T->muted, T->surf);
}

void drawReadButton(bool pressed)
{
    const Rect &r = READ_BTN;
    const uint16_t bg = pressed ? T->track : T->surf;
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, T->line);
    textAt(FONT_12B, "READ", r.x + r.w / 2, r.y + 21, C_BASELINE, T->text, bg);
}

void drawClearButton(int progressPercent)
{
    drawHoldButton(CLEAR_BTN, "CLEAR", T->bg, T->bad, 2, T->bad, T->badSoft, progressPercent);
}

void drawPagerButton(const Rect &r, const char *arrow, bool enabled, bool pressed)
{
    const uint16_t bg = (pressed && enabled) ? T->track : T->surf;
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    if (T->cardOutline) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, T->cardLine);
    textAt(FONT_14B, arrow, r.x + r.w / 2, r.y + 16, C_BASELINE, enabled ? T->text : T->line, bg);
}

void updateFaultsPage()
{
    const DtcData d = snapshotDtc();
    const int pages = d.state == DtcState::Done ? max(1, (d.count + DTC_ROWS - 1) / DTC_ROWS) : 1;
    if (dtcListPage >= pages) dtcListPage = pages - 1;
    const int key = static_cast<int>(d.state) | (static_cast<int>(d.msg) << 2) | (d.mil ? 1 << 6 : 0) |
                    (d.count << 7) | (dtcListPage << 13);
    if (d.state == DtcState::Busy && key == drawnDtcKey && !toast.active) {  // แถบวิ่งระหว่างอ่าน/ลบรหัส
        const int x = static_cast<int>((millis() / 40) % 260);
        tft.fillRect(8, 49, 304, 3, T->track);
        tft.fillRect(8 + x, 49, 44, 3, T->warn);
    }
    if (key == drawnDtcKey) return;
    drawnDtcKey = key;
    toastDirty = true;

    // แถวหัว: จำนวนรหัส + Fault codes ทางซ้าย; MIL ON และตัวเปลี่ยนหน้า (เมื่อเกิน 3 รหัส) ทางขวา
    tft.fillRect(0, STATUS_H + 1, SCREEN_W, DTC_ROW_Y - STATUS_H - 1, T->bg);
    char text[48];
    if (d.state == DtcState::Done && d.count > 0) {
        snprintf(text, sizeof(text), "%d", d.count);
        const int16_t cw = textAt(FONT_20B, text, 8, 46, L_BASELINE, T->bad, T->bg);
        textAt(FONT_11S, "Fault codes", 8 + cw + 6, 46, L_BASELINE, T->text, T->bg);
    } else {
        uint16_t color = T->muted;
        switch (d.state) {
            case DtcState::Idle: snprintf(text, sizeof(text), "Press READ to scan fault codes"); break;
            case DtcState::Busy:
                snprintf(text, sizeof(text), "Working ...");
                color = T->warn;
                break;
            case DtcState::Done:
                snprintf(text, sizeof(text), d.msg == DtcMsg::Cleared ? "Codes cleared. No faults stored" : "No fault codes stored");
                color = T->ok;
                break;
            case DtcState::Error:
                snprintf(text, sizeof(text), "%s",
                         d.msg == DtcMsg::ClearFailed ? "ECU refused to clear codes"
                         : d.msg == DtcMsg::Timeout   ? "Timeout: no answer from adapter"
                         : d.msg == DtcMsg::NoLink    ? "Not connected to the OBD adapter"
                                                      : "No response from ECU");
                color = T->bad;
                break;
        }
        textAt(FONT_11S, text, 8, 44, L_BASELINE, color, T->bg);
    }
    int16_t right = SCREEN_W - 8;
    if (pages > 1) {
        drawPagerButton(PREV_BTN, "‹", dtcListPage > 0, false);
        drawPagerButton(NEXT_BTN, "›", dtcListPage < pages - 1, false);
        snprintf(text, sizeof(text), "%d/%d", dtcListPage + 1, pages);
        textAt(FONT_10S, text, (PREV_BTN.x + PREV_BTN.w + NEXT_BTN.x) / 2, 43, C_BASELINE, T->muted, T->bg);
        right = PREV_BTN.x - 6;
    }
    if (d.state == DtcState::Done && d.count > 0 && d.mil) {
        const int16_t bw = textWidthIn(FONT_10B, "MIL ON") + 12;
        tft.fillRoundRect(right - bw, 31, bw, 16, 2, T->accent);
        textAt(FONT_10B, "MIL ON", right - bw + 6, 42, L_BASELINE, T->onAccent, T->accent);
    }

    // รายการรหัส 3 แถวต่อหน้า
    tft.fillRect(6, DTC_ROW_Y, 308, DTC_ROWS * DTC_ROW_PITCH, T->bg);
    for (int k = 0; k < DTC_ROWS; k++) {
        const int idx = dtcListPage * DTC_ROWS + k;
        if (d.state == DtcState::Done && idx < d.count) drawDtcRow(k, d.codes[idx]);
    }
}

// =====================================================================
//  แก้การแสดงผลของจอแต่ละรุ่น: กลับสี, สลับแดง/น้ำเงิน, หมุน 180°
//  ค่าเริ่มต้นของแต่ละชิปมาจาก build flag ของ env (cyd / cyd_st7789) ส่วนนี้ปรับเพิ่มตอนใช้งานโดยไม่ต้อง build ใหม่
// =====================================================================
// MADCTL ของแนวนอนตามที่ TFT_eSPI 2.5.43 ตั้งให้ (rotation 1 / 3) ใช้กลับบิต BGR เมื่อสลับแดง/น้ำเงิน
uint8_t landscapeMadctl(bool flip)
{
#if defined(ST7789_DRIVER)
    const uint8_t m = flip ? (TFT_MAD_MV | TFT_MAD_MY) : (TFT_MAD_MX | TFT_MAD_MV);
#else
    const uint8_t m = flip ? (TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV) : TFT_MAD_MV;
#endif
    return m | TFT_MAD_COLOR_ORDER;
}

void applyDisplayFix()
{
    const bool flip = dispFix & DISP_FLIP;
    tft.setRotation(flip ? 3 : 1);
    if (dispFix & DISP_SWAP_RB) {
        tft.writecommand(TFT_MADCTL);
        tft.writedata(landscapeMadctl(flip) ^ TFT_MAD_BGR);
    }
    tft.invertDisplay(dispFix & DISP_INVERT);
}

void drawPageStatic();
void drawCalibrationScreen();

void setDisplayFix(uint8_t value)
{
    dispFix = value & (DISP_INVERT | DISP_SWAP_RB | DISP_FLIP);
    prefs.putUChar(DISP_NVS_KEY, dispFix);
    Serial.printf("[DISP] %s  invert=%s swapRB=%s flip=%s (saved)\n", DISPLAY_CHIP, dispFix & DISP_INVERT ? "on" : "off",
                  dispFix & DISP_SWAP_RB ? "on" : "off", dispFix & DISP_FLIP ? "on" : "off");
    applyDisplayFix();
    tft.fillScreen(T->bg);  // หมุนจอ = ภาพเดิมใช้ไม่ได้ วาดใหม่ทั้งหน้า
    if (alertActive) {
        drawnAlert = NEVER;
        return;
    }
    if (page == Page::Calibrate) drawCalibrationScreen();
    else drawPageStatic();
}

// =====================================================================
//  หน้า SET: รายการตั้งค่าที่ลากขึ้น/ลงได้ (แถวละ 31 ในพื้นที่ y 24-206, clip ด้วย viewport)
// =====================================================================
// ค่าที่ตั้งไว้เปลี่ยนจากที่อื่น (เช่นหน้าเว็บ) ระหว่างเปิดหน้านี้อยู่ ให้วาดใหม่
int configKey()
{
    char name[33], pin[17];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    uint32_t h = strlen(pin);
    for (const char *p = name; *p != '\0'; p++) h = h * 31 + static_cast<uint8_t>(*p);
    return static_cast<int>(h & 0x3FFFFFFF);
}

int setKey()
{
    uint32_t h = static_cast<uint32_t>(configKey());
    for (int v : {backlightPercent, static_cast<int>(soundVolume), static_cast<int>(themeMode), shiftRpm, coolLimit, setScroll, static_cast<int>(dispFix)}) h = h * 31 + static_cast<uint32_t>(v);
    return static_cast<int>(h & 0x3FFFFFFF);
}

// พื้นแถวแบบการ์ด (pressed = ไฮไลต์) คืนสีพื้น
uint16_t drawSetRowBox(int top, bool pressed)
{
    const uint16_t bg = pressed ? T->track : T->surf;
    tft.fillRoundRect(6, top, 308, SET_ROW_H - 1, 3, bg);
    if (T->cardOutline && !pressed) tft.drawRoundRect(6, top, 308, SET_ROW_H - 1, 3, T->cardLine);
    return bg;
}

void drawSegment(int top, int count, int w, const char *const *names, int active, Widget first)
{
    const int x0 = 308 - count * w - 2;
    tft.fillRoundRect(x0 - 2, top + 5, count * w + 4, 21, 3, T->track);
    for (int i = 0; i < count; i++) {
        const int x = x0 + i * w;
        const bool on = i == active;
        if (on) tft.fillRoundRect(x, top + 7, w, 17, 2, T->surf);
        const uint16_t fg = (first != Widget::None && pressedWidget == first + i) ? T->accent : (on ? T->text : T->muted);
        textAt(FONT_10B, names[i], x + w / 2, top + 19, C_BASELINE, fg, on ? T->surf : T->track);
    }
}

void drawStepper(int top, const char *value, Widget minus, uint16_t bg)
{
    drawButton(stepMinusRect(top), FONT_14B, "−", true, pressedWidget == minus, false);
    drawButton(stepPlusRect(top), FONT_14B, "+", true, pressedWidget == minus + 1, false);
    textAt(FONT_12V, value, 247, top + 20, C_BASELINE, T->text, bg);
}

void drawSetRow(int r, int top)
{
    tft.fillRect(0, top, 314, SET_ROW_H, T->bg);  // x 314-319 = scrollbar
    const int base = top + 20;
    auto label = [&](const char *t, uint16_t bg) { textAt(FONT_11S, t, 14, base, L_BASELINE, T->muted, bg); };
    char text[48];
    switch (r) {
        case SR_ADAPTER:
        case SR_PIN: {
            const bool pressed = pressedWidget == (r == SR_ADAPTER ? Widget::FieldName : Widget::FieldPin);
            const uint16_t bg = drawSetRowBox(top, pressed);
            label(r == SR_ADAPTER ? "Adapter" : "PIN", bg);
            const int16_t chevronW = textAt(FONT_14B, "›", 306, base, R_BASELINE, pressed ? T->accent : T->muted, bg);
            const int right = 306 - chevronW - 8;
            const uint16_t color = pressed ? T->accent : T->text;
            char name[33], pin[17];
            getConfig(name, sizeof(name), pin, sizeof(pin));
            if (r == SR_ADAPTER) {
                if (name[0] == '\0') {
                    textAt(FONT_12V, "(tap to set)", right, base, R_BASELINE, T->muted, bg);
                } else {
                    fitText(FONT_12V, name, right - 90, text, sizeof(text));
                    textAt(FONT_12V, text, right, base, R_BASELINE, color, bg);
                }
            } else if (pin[0] == '\0') {
                textAt(FONT_12V, "None", right, base, R_BASELINE, T->muted, bg);
            } else {  // PIN เป็นจุดปิดบัง ระยะห่าง 2px
                const int16_t dotW = textWidthIn(FONT_12V, "•");
                const int n = min(static_cast<int>(strlen(pin)), 8);
                int16_t x = right - n * (dotW + 2) + 2;
                for (int i = 0; i < n; i++, x += dotW + 2) textAt(FONT_12V, "•", x, base, L_BASELINE, color, bg);
            }
            break;
        }
        case SR_CONNECT:
            drawButton({6, static_cast<int16_t>(top), 308, SET_ROW_H - 2}, FONT_12B, "CONNECT", true, pressedWidget == Widget::BtnConnect, true);
            break;
        case SR_THEME: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Theme", bg);
            drawSegment(top, 3, THEME_SEG_W, THEME_MODE_NAMES, static_cast<int>(themeMode), Widget::ThemeDark);
            break;
        }
        case SR_BACKLIGHT: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Backlight", bg);
            const int fill = BL_TRACK_W * (backlightPercent - app::BACKLIGHT_MIN_PERCENT) / (100 - app::BACKLIGHT_MIN_PERCENT);
            tft.fillRoundRect(BL_TRACK_X, top + 14, BL_TRACK_W, 4, 2, T->track);
            tft.fillRoundRect(BL_TRACK_X, top + 14, max(4, fill), 4, 2, T->accent);
            tft.fillSmoothCircle(BL_TRACK_X + fill, top + 16, 8, T->line, bg);
            tft.fillSmoothCircle(BL_TRACK_X + fill, top + 16, 7, T->knob, T->line);
            snprintf(text, sizeof(text), "%d%%", backlightPercent);
            textAt(FONT_12V, text, 306, base, R_BASELINE, T->text, bg);
            break;
        }
        case SR_SOUND: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Sound", bg);
            drawSegment(top, 4, SOUND_SEG_W, VOLUME_NAMES, soundVolume, Widget::Sound0);
            break;
        }
        case SR_SHIFT: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Shift light", bg);
            formatThousands(shiftRpm, text, sizeof(text));
            drawStepper(top, text, Widget::ShiftMinus, bg);
            break;
        }
        case SR_COOL: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Overheat °C", bg);
            snprintf(text, sizeof(text), "%d", coolLimit);
            drawStepper(top, text, Widget::CoolMinus, bg);
            break;
        }
        case SR_INVERT:
        case SR_SWAPRB:
        case SR_FLIP: {
            static constexpr const char *LABELS[3] = {"Invert colors", "Swap red/blue", "Flip 180°"};
            static constexpr uint8_t BITS[3] = {DISP_INVERT, DISP_SWAP_RB, DISP_FLIP};
            static constexpr const char *OFF_ON[2] = {"OFF", "ON"};
            const int k = r - SR_INVERT;
            const Widget w = Widget::DispInvert + k;
            const uint16_t bg = drawSetRowBox(top, pressedWidget == w);
            label(LABELS[k], bg);
            drawSegment(top, 2, THEME_SEG_W, OFF_ON, (dispFix & BITS[k]) ? 1 : 0, Widget::None);
            break;
        }
        case SR_RECAL:
            drawHoldButton({6, static_cast<int16_t>(top), 308, SET_ROW_H - 2}, "RECALIBRATE TOUCH", T->surf, T->line, 1, T->text, T->line, 0);
            break;
        case SR_FW: {
            const uint16_t bg = drawSetRowBox(top, false);
            label("Firmware", bg);
            snprintf(text, sizeof(text), "%s  %s", app::FW_VERSION, DISPLAY_CHIP);
            textAt(FONT_12V, text, 306, base, R_BASELINE, T->text, bg);
            break;
        }
        default:
            break;
    }
}

bool setRowVisible(int top)
{
    return top + SET_ROW_H > SET_AREA.y && top < SET_AREA.y + SET_AREA.h;
}

void clipToSetArea()
{
    tft.setViewport(SET_AREA.x, SET_AREA.y, SET_AREA.w, SET_AREA.h, false);  // false = พิกัดเดิมของจอ แค่ตัดขอบ
}

void drawSetScrollbar()
{
    tft.fillRect(314, SET_AREA.y, 6, SET_AREA.h, T->bg);
    if (SET_MAX_SCROLL <= 0) return;
    const int th = SET_AREA.h * SET_AREA.h / SET_CONTENT_H;
    const int ty = SET_AREA.y + (SET_AREA.h - th) * setScroll / SET_MAX_SCROLL;
    tft.fillRoundRect(316, ty, 3, th, 1, T->muted);
}

void drawSetList()
{
    clipToSetArea();
    const int firstTop = setRowTop(0);
    if (firstTop > SET_AREA.y) tft.fillRect(0, SET_AREA.y, 314, firstTop - SET_AREA.y, T->bg);
    const int endY = setRowTop(SR_COUNT);
    if (endY < SET_AREA.y + SET_AREA.h) tft.fillRect(0, endY, 314, SET_AREA.y + SET_AREA.h - endY, T->bg);
    for (int r = 0; r < SR_COUNT; r++) {
        const int top = setRowTop(r);
        if (setRowVisible(top)) drawSetRow(r, top);
    }
    tft.resetViewport();
    drawSetScrollbar();
    drawnSetKey = setKey();
    toastDirty = true;
}

// วาดแถวเดียว (เช่นปุ่มถูกกด ค่าเปลี่ยน)
void drawSetRowAt(int r)
{
    const int top = setRowTop(r);
    if (!setRowVisible(top)) return;
    clipToSetArea();
    drawSetRow(r, top);
    tft.resetViewport();
    drawnSetKey = setKey();
    toastDirty = true;
}

void drawRecalButton(int progressPercent)
{
    const int top = setRowTop(SR_RECAL);
    if (!setRowVisible(top)) return;
    clipToSetArea();
    drawHoldButton({6, static_cast<int16_t>(top), 308, SET_ROW_H - 2}, "RECALIBRATE TOUCH", T->surf, T->line, 1, T->text, T->line, progressPercent);
    tft.resetViewport();
}

// แถวที่ปุ่ม/ตัวเลือกนี้อยู่ (-1 = ไม่ใช่ของหน้า SET)
int setRowOf(Widget w)
{
    switch (w) {
        case Widget::FieldName: return SR_ADAPTER;
        case Widget::FieldPin: return SR_PIN;
        case Widget::BtnConnect: return SR_CONNECT;
        case Widget::ThemeDark:
        case Widget::ThemeLight:
        case Widget::ThemeAuto: return SR_THEME;
        case Widget::BlSlider: return SR_BACKLIGHT;
        case Widget::Sound0:
        case Widget::Sound1:
        case Widget::Sound2:
        case Widget::Sound3: return SR_SOUND;
        case Widget::ShiftMinus:
        case Widget::ShiftPlus: return SR_SHIFT;
        case Widget::CoolMinus:
        case Widget::CoolPlus: return SR_COOL;
        case Widget::DispInvert: return SR_INVERT;
        case Widget::DispSwapRB: return SR_SWAPRB;
        case Widget::DispFlip: return SR_FLIP;
        case Widget::BtnRecal: return SR_RECAL;
        default: return -1;
    }
}

void updateSetPage()
{
    if (setKey() != drawnSetKey) drawSetList();
}

// =====================================================================
//  หน้า SCAN: ค้นหาและเลือกอะแดปเตอร์ OBD
//    หัว: ชื่อหน้า + สถานะ (นับถอยหลัง / จำนวนที่เจอ) และแถบความคืบหน้า
//    รายการ: ความแรงสัญญาณ | ชื่อ + MAC + ชนิด | ป้าย OBD / SAVED   (อะแดปเตอร์ OBD ขึ้นก่อน แล้วเรียงตามสัญญาณ)
//    ล่าง: BACK | ‹ › | MANUAL (พิมพ์ชื่อเอง) | RESCAN
// =====================================================================
ScanDevice uiScan[app::MAX_SCAN_DEVICES];  // รายการที่แสดงอยู่ (เรียงแล้ว) แตะแถวไหนใช้ข้อมูลชุดนี้
int uiScanCount = 0;
int scanListPage = 0;
int drawnScanKey = NEVER;
int drawnScanHeader = NEVER;
int drawnScanButtons = NEVER;

// อันดับในรายการ: OBD ก่อน > มีชื่อก่อน > สัญญาณแรงก่อน
bool scanBefore(const ScanDevice &a, const ScanDevice &b)
{
    if (a.obd != b.obd) return a.obd;
    const bool an = a.name[0] != '\0', bn = b.name[0] != '\0';
    if (an != bn) return an;
    return a.rssi > b.rssi;
}

void snapshotScan()
{
    portENTER_CRITICAL(&scanMux);
    uiScanCount = g_scanCount;
    memcpy(uiScan, g_scanDev, sizeof(ScanDevice) * g_scanCount);
    portEXIT_CRITICAL(&scanMux);
    for (int i = 1; i < uiScanCount; i++) {  // insertion sort (มีไม่เกิน 15 ตัว)
        const ScanDevice d = uiScan[i];
        int j = i - 1;
        while (j >= 0 && scanBefore(d, uiScan[j])) {
            uiScan[j + 1] = uiScan[j];
            j--;
        }
        uiScan[j + 1] = d;
    }
}

int scanPages()
{
    return max(1, (uiScanCount + SCAN_ROWS - 1) / SCAN_ROWS);
}

bool scanBusy()
{
    const ScanState st = scanState;
    return scanReq || st == ScanState::Waiting || st == ScanState::Inquiry || st == ScanState::Names;
}

// อุปกรณ์นี้คืออะแดปเตอร์ที่ตั้งไว้อยู่แล้วหรือไม่ (เทียบ MAC ก่อน ไม่มี MAC เทียบชื่อ)
bool isSavedAdapter(const ScanDevice &d)
{
    char name[33], pin[17], mac[18], text[18];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    getConfigMac(mac, sizeof(mac));
    formatMac(d.mac, text, sizeof(text));
    if (mac[0] != '\0') return strcasecmp(mac, text) == 0;
    return d.name[0] != '\0' && strcmp(d.name, name) == 0;
}

const char *codTypeName(uint32_t cod)
{
    switch ((cod >> 8) & 0x1F) {  // major device class
        case 1: return "PC";
        case 2: return "PHONE";
        case 4: return "AUDIO";
        case 5: return "INPUT";
        case 7: return "WEARABLE";
        default: return "";
    }
}

int rssiBars(int8_t rssi)
{
    if (rssi == -127) return 0;
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

// สัญญาณ 4 ขีด สูง 4/7/10/13 px ฐานที่ baseY
void drawSignalBars(int x, int baseY, int bars)
{
    const uint16_t color = bars >= 3 ? T->ok : (bars == 2 ? T->warn : T->bad);
    for (int i = 0; i < 4; i++) {
        const int h = 4 + i * 3;
        tft.fillRect(x + i * 4, baseY - h, 3, h, i < bars ? color : T->track);
    }
}

void drawScanRow(int k)
{
    const Rect r = scanRowRect(k);
    const int idx = scanListPage * SCAN_ROWS + k;
    if (idx >= uiScanCount) {
        tft.fillRect(r.x, r.y, r.w, r.h, T->bg);
        return;
    }
    const ScanDevice &d = uiScan[idx];
    const bool pressed = pressedWidget == static_cast<Widget>(static_cast<int>(Widget::ScanRow0) + k);
    const bool saved = isSavedAdapter(d);
    const uint16_t bg = pressed ? T->track : T->surf;
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    if (T->cardOutline && !pressed) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, T->cardLine);
    if (d.obd) {  // แถบ accent ซ้ายของอะแดปเตอร์ OBD
        tft.fillRoundRect(r.x, r.y, 7, r.h, 4, T->accent);
        tft.fillRect(r.x + 3, r.y + 1, 4, r.h - 2, bg);
    }

    drawSignalBars(r.x + 10, r.y + 23, rssiBars(d.rssi));

    // ป้ายขวา: SAVED (อะแดปเตอร์ที่ตั้งไว้) และ OBD
    int16_t right = r.x + r.w - 8;
    right -= textAt(FONT_14B, "›", right, r.y + 21, R_BASELINE, pressed ? T->accent : T->muted, bg) + 6;
    if (d.obd) {
        const int16_t bw = textWidthIn(FONT_10B, "OBD") + 10;
        tft.fillRoundRect(right - bw, r.y + 9, bw, 14, 2, T->accent);
        textAt(FONT_10B, "OBD", right - bw + 5, r.y + 19, L_BASELINE, T->onAccent, T->accent);
        right -= bw + 5;
    }
    if (saved) {
        right -= textAt(FONT_10B, "SAVED", right, r.y + 19, R_BASELINE, T->ok, bg) + 5;
    }

    char mac[18], line[48];
    formatMac(d.mac, mac, sizeof(mac));
    const int textX = r.x + 32;
    const int maxW = right - textX - 2;
    if (d.name[0] != '\0') {
        fitText(FONT_12V, d.name, maxW, line, sizeof(line));
        textAt(FONT_12V, line, textX, r.y + 14, L_BASELINE, T->text, bg);
    } else {
        textAt(FONT_12V, "(no name)", textX, r.y + 14, L_BASELINE, T->muted, bg);
    }
    const char *type = codTypeName(d.cod);
    if (type[0] != '\0') snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", mac, type);  // "·"
    else strlcpy(line, mac, sizeof(line));
    char shown[48];
    fitText(FONT_10S, line, maxW, shown, sizeof(shown));
    textAt(FONT_10S, shown, textX, r.y + 27, L_BASELINE, T->muted, bg);
}

// ไม่มีอุปกรณ์ในรายการ: ข้อความกลางจอบอกสถานะ/วิธีแก้
void drawScanEmpty()
{
    const ScanState st = scanState;
    const char *title = "Searching for Bluetooth devices";
    const char *hint1 = "Plug the adapter into the OBD port";
    const char *hint2 = "and turn the ignition ON";
    if (scanReq || st == ScanState::Waiting) {
        title = "Stopping the current connection";
        hint1 = "This can take a few seconds";
        hint2 = "";
    } else if (st == ScanState::Done || st == ScanState::Idle) {
        title = "No devices found";
        hint1 = "Check that the adapter's light is on,";
        hint2 = "then tap RESCAN";
    } else if (st == ScanState::Error) {
        title = "Bluetooth error";
        hint1 = "Tap RESCAN to try again";
        hint2 = "";
    }
    tft.fillRect(SCAN_LIST.x, SCAN_LIST.y + 3, SCAN_LIST.w, SCAN_LIST.h - 3, T->bg);
    textAt(FONT_12B, title, SCREEN_W / 2, 98, C_BASELINE, T->text, T->bg);
    textAt(FONT_11S, hint1, SCREEN_W / 2, 124, C_BASELINE, T->muted, T->bg);
    if (hint2[0] != '\0') textAt(FONT_11S, hint2, SCREEN_W / 2, 142, C_BASELINE, T->muted, T->bg);
}

void drawScanButton(const Rect &r, const uint8_t *font, const char *label, bool enabled, bool pressed, bool primary)
{
    uint16_t bg = T->surf, fg = enabled ? T->text : T->line, border = T->line;
    if (primary && enabled) bg = border = T->accent, fg = T->onAccent;
    if (pressed && enabled) bg = border = T->track, fg = T->text;
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    if (border != bg) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, border);
    textAt(font, label, r.x + r.w / 2, r.y + (font == FONT_14B ? 21 : 20), C_BASELINE, fg, bg);
}

void drawScanButtons()
{
    const bool busy = scanBusy();
    const int pages = scanPages();
    drawnScanButtons = (busy ? 1 : 0) | (pages << 1) | (scanListPage << 6);
    tft.fillRect(0, SCAN_BAR_Y, SCREEN_W, SCREEN_H - SCAN_BAR_Y, T->bar);
    tft.drawFastHLine(0, SCAN_BAR_Y, SCREEN_W, T->line);
    drawScanButton(SCAN_BACK_BTN, FONT_12B, "BACK", true, pressedWidget == Widget::ScanBack, false);
    drawScanButton(SCAN_PREV_BTN, FONT_14B, "‹", scanListPage > 0, pressedWidget == Widget::ScanPrev, false);
    drawScanButton(SCAN_NEXT_BTN, FONT_14B, "›", scanListPage < pages - 1, pressedWidget == Widget::ScanNext, false);
    drawScanButton(SCAN_MANUAL_BTN, FONT_12B, "MANUAL", true, pressedWidget == Widget::ScanManual, false);
    drawScanButton(SCAN_RESCAN_BTN, FONT_12B, busy ? "SCANNING" : "RESCAN", !busy, pressedWidget == Widget::ScanRescan, true);
}

// หัวหน้า: ชื่อ | สถานะชิดขวา (จุดกะพริบระหว่างค้นหา) และแถบความคืบหน้า 3px ใต้เส้น
void updateScanHeader(uint32_t now)
{
    const ScanState st = scanState;
    const bool busy = scanBusy();
    const uint32_t totalMs = app::SCAN_INQ_UNITS * 1280UL;
    const uint32_t elapsed = now - scanStartedAt;
    int remain = 0, progress = 0;
    if (!scanReq && st == ScanState::Inquiry) {
        remain = static_cast<int>((totalMs - min(elapsed, totalMs) + 999) / 1000);
        progress = static_cast<int>(min<uint32_t>(100, elapsed * 100 / totalMs));
    }
    const bool dotOn = !busy || blinkPhase(now);
    const int key = static_cast<int>(st) | (scanReq ? 8 : 0) | (dotOn ? 16 : 0) | (remain << 5) | (progress << 11) | (uiScanCount << 18);
    if (key == drawnScanHeader) return;
    drawnScanHeader = key;

    char text[24];
    uint16_t color = T->warn;
    if (scanReq || st == ScanState::Waiting) {
        strlcpy(text, "WAITING", sizeof(text));
    } else if (st == ScanState::Inquiry) {
        snprintf(text, sizeof(text), "SEARCHING  %ds", remain);
    } else if (st == ScanState::Names) {
        strlcpy(text, "READING NAMES", sizeof(text));
    } else if (st == ScanState::Error) {
        strlcpy(text, "SCAN FAILED", sizeof(text));
        color = T->bad;
    } else {
        snprintf(text, sizeof(text), "%d FOUND", uiScanCount);
        color = uiScanCount > 0 ? T->ok : T->muted;
    }
    tft.fillRect(0, 0, SCREEN_W, STATUS_H, T->bg);
    tft.drawFastHLine(0, STATUS_H, SCREEN_W, T->line);
    textAt(FONT_11B, "SELECT OBD ADAPTER", 10, 16, L_BASELINE, T->text, T->bg);
    const int16_t w = textAt(FONT_10S, text, SCREEN_W - 10, 15, R_BASELINE, color, T->bg);
    tft.fillSmoothCircle(SCREEN_W - 10 - w - 8, 12, 3, dotOn ? color : T->line, T->bg);

    // แถบความคืบหน้า: ระหว่างค้นหาเติมตามเวลา, ขั้นถามชื่อเต็มแถบ, เสร็จแล้วซ่อน
    tft.fillRect(0, STATUS_H + 1, SCREEN_W, 3, T->bg);
    if (st == ScanState::Inquiry && !scanReq) {
        tft.fillRect(0, STATUS_H + 1, SCREEN_W * progress / 100, 3, T->accent);
    } else if (st == ScanState::Names) {
        tft.fillRect(0, STATUS_H + 1, SCREEN_W, 3, dotOn ? T->accent : T->track);
    }
}

void updateScanPage(uint32_t now)
{
    snapshotScan();
    if (scanListPage >= scanPages()) scanListPage = scanPages() - 1;
    updateScanHeader(now);

    const int key = (static_cast<int>(scanVersion) & 0xFFFF) | (scanListPage << 16) | (static_cast<int>(scanState) << 20) | (scanReq ? 1 << 24 : 0);
    if (key != drawnScanKey) {
        drawnScanKey = key;
        if (uiScanCount == 0) {
            drawScanEmpty();
        } else {
            for (int k = 0; k < SCAN_ROWS; k++) drawScanRow(k);
        }
    }
    const int buttons = (scanBusy() ? 1 : 0) | (scanPages() << 1) | (scanListPage << 6);
    if (buttons != drawnScanButtons) drawScanButtons();
}

// =====================================================================
//  คีย์บอร์ดบนจอ
// =====================================================================
char keyLabelChar(int i)
{
    char c = KEY_CHARS[i];
    if (kbShift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return c;
}

void drawKey(int i, bool pressed)
{
    const Rect r = keyRect(i);
    char label[8];
    uint16_t fill = T->surf, fg = T->text, border = T->cardOutline ? T->cardLine : T->surf;
    if (i < 39) {
        label[0] = keyLabelChar(i);
        label[1] = '\0';
    } else if (i == KEY_SHIFT) {
        strcpy(label, "SHIFT");
        if (kbShift) {
            fill = border = T->accent;
            fg = T->onAccent;
        }
    } else if (i == KEY_SPACE) {
        strcpy(label, "SPACE");
    } else if (i == KEY_DEL) {
        strcpy(label, "DEL");
    } else if (i == KEY_ESC) {
        strcpy(label, "ESC");
        fill = T->bg;
        border = fg = T->bad;
    } else {
        strcpy(label, "OK");
        fill = border = T->accent;
        fg = T->onAccent;
    }
    if (pressed) {
        fill = border = T->track;
        fg = T->text;
    }
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 4, fill);
    if (border != fill) tft.drawRoundRect(r.x, r.y, r.w, r.h, 4, border);
    textAt(FONT_12V, label, r.x + r.w / 2, r.y + r.h / 2 + 4, C_BASELINE, fg, fill);
}

void drawKeyboardTextBox()
{
    tft.fillRoundRect(KB_TEXT_BOX.x, KB_TEXT_BOX.y, KB_TEXT_BOX.w, KB_TEXT_BOX.h, 4, T->surf);
    tft.drawRoundRect(KB_TEXT_BOX.x, KB_TEXT_BOX.y, KB_TEXT_BOX.w, KB_TEXT_BOX.h, 4, T->accent);
    char shown[40];
    snprintf(shown, sizeof(shown), "%s_", kbBuf);
    textAt(FONT_12V, shown, KB_TEXT_BOX.x + 8, KB_TEXT_BOX.y + 18, L_BASELINE, T->text, T->surf);
}

// =====================================================================
//  วาดปุ่มใหม่ตามสถานะ "ถูกกด"
// =====================================================================
void redrawWidget(Widget w)
{
    const bool pressed = pressedWidget == w;
    if (isKey(w)) {
        drawKey(keyIndex(w), pressed);
        return;
    }
    if (w >= Widget::TabDrive && w <= Widget::TabSet) {
        drawTab(w - Widget::TabDrive);
        return;
    }
    const int setRow = setRowOf(w);
    if (setRow >= 0) {
        if (page != Page::Config || w == Widget::BlSlider) return;
        if (w == Widget::BtnRecal) drawRecalButton(0);
        else drawSetRowAt(setRow);
        return;
    }
    switch (w) {
        case Widget::FocusPrev: drawButton(FOCUS_PREV_BTN, FONT_14B, "‹", true, pressed, false); break;
        case Widget::FocusNext: drawButton(FOCUS_NEXT_BTN, FONT_14B, "›", true, pressed, false); break;
        case Widget::FocusReset: drawFocusResetButton(0); break;
        case Widget::TripReset: drawTripResetButton(0); break;
        case Widget::ZeroArm: drawZeroArmButton(pressed); break;
        case Widget::BtnRead: drawReadButton(pressed); break;
        case Widget::BtnClear: drawClearButton(0); break;
        case Widget::BtnPrev: drawPagerButton(PREV_BTN, "‹", dtcListPage > 0, pressed); break;
        case Widget::BtnNext: drawPagerButton(NEXT_BTN, "›", dtcListPage < dtcPages() - 1, pressed); break;
        case Widget::ScanRow0:
        case Widget::ScanRow1:
        case Widget::ScanRow2:
        case Widget::ScanRow3:
        case Widget::ScanRow4:
            if (uiScanCount > 0) drawScanRow(w - Widget::ScanRow0);
            break;
        case Widget::ScanPrev:
        case Widget::ScanNext:
        case Widget::ScanBack:
        case Widget::ScanManual:
        case Widget::ScanRescan: drawScanButtons(); break;
        default:
            break;
    }
}

// =====================================================================
//  หน้า calibrate (ใช้ฟอนต์ในตัว สีจากธีม)
// =====================================================================
void resetDrawnKeys()
{
    drawnStatus = NEVER;
    drawnFaultBadge = NEVER;
    drawnSpeed = NEVER;
    drawnRpmText = NEVER;
    drawnSweep = NEVER;
    drawnArcColor = NEVER;
    drawnPeak = NEVER;
    drawnPeakText = NEVER;
    drawnShift = NEVER;
    for (int &t : drawnTile) t = NEVER;
    drawnDtcKey = NEVER;
    drawnFocusKey = NEVER;
    drawnFocusValue = NEVER;
    drawnFocusStats = NEVER;
    drawnFocusGraph = 0xFFFFFFFF;
    drawnTripKey = NEVER;
    drawnZeroKey = NEVER;
    drawnZeroValue = NEVER;
    drawnSetKey = NEVER;
    drawnToast = NEVER;
    drawnScanKey = NEVER;
    drawnScanHeader = NEVER;
    drawnScanButtons = NEVER;
}

void drawCalibrationTarget()
{
    int tx, ty;
    calTarget(calStep, tx, ty);
    tft.drawFastHLine(tx - 12, ty, 25, T->ok);
    tft.drawFastVLine(tx, ty - 12, 25, T->ok);
    tft.drawCircle(tx, ty, 7, T->ok);
}

void eraseCalibrationTarget(int step)
{
    int tx, ty;
    calTarget(step, tx, ty);
    tft.fillRect(tx - 14, ty - 14, 29, 29, T->bg);
}

void drawCalibrationScreen()
{
    useBuiltinFont();
    tft.fillScreen(T->bg);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(T->text, T->bg);
    tft.drawString("Touch calibration", SCREEN_W / 2, 70, 4);
    tft.setTextColor(T->muted, T->bg);
    tft.drawString("Touch the CENTER of each green cross,", SCREEN_W / 2, 108, 2);
    tft.drawString("hold until the next cross appears.", SCREEN_W / 2, 128, 2);
    tft.drawString("(a fingernail or a stylus is more accurate)", SCREEN_W / 2, 148, 2);
    tft.setTextDatum(TL_DATUM);
    char text[24];
    snprintf(text, sizeof(text), "Point %d / 4", calStep + 1);
    drawText(text, 112, 176, 2, T->accent, T->bg, 96);
    drawCalibrationTarget();
}

void beginCalibration()
{
    page = Page::Calibrate;
    alertActive = false;  // หน้า calibrate ต้องแตะได้เสมอ
    calStep = 0;
    calAccepted = 0;
    calSamples = 0;
    calSumX = calSumY = 0;
    calWaitRelease = false;
    calMessageUntil = 0;
    pressTarget = pressedWidget = Widget::None;
    resetDrawnKeys();
    Serial.println("[CAL] touch the center of each cross (4 points)");
    drawCalibrationScreen();
}

// =====================================================================
//  เปลี่ยนหน้า: พื้นหลังสีเดียว (fillScreen) แล้ววาดส่วนที่ไม่เปลี่ยนครั้งเดียว ส่วนค่าวาดใน loop เมื่อเปลี่ยน
// =====================================================================
// วาดส่วนคงที่ของหน้าปัจจุบัน (ไม่ล้างจอ) แล้วให้ค่าตัวเลขวาดใหม่ในรอบถัดไป: ใช้ตอนเปลี่ยนหน้าและตอน toast หายไป
void drawPageStatic()
{
    resetDrawnKeys();
    switch (page) {
        case Page::Drive:
            drawGaugeStatic();
            for (int i = 0; i < TILE_COUNT; i++) drawTileStatic(i);
            break;
        case Page::Focus:
            drawFocusStatic();
            break;
        case Page::Trip:
            drawTripStatic();
            break;
        case Page::Faults:
            drawReadButton(false);
            drawClearButton(0);
            break;
        case Page::Config:
            drawSetList();
            break;
        case Page::Keyboard:
            tft.drawFastHLine(0, STATUS_H, SCREEN_W, T->line);
            textAt(FONT_11B, kbField == 0 ? "EDIT  ADAPTER NAME / MAC" : "EDIT  PIN", 10, 16, L_BASELINE, T->text, T->bg);
            drawKeyboardTextBox();
            for (int i = 0; i < KEY_COUNT; i++) drawKey(i, false);
            break;
        case Page::Scan:
            snapshotScan();
            updateScanPage(millis());
            break;
        case Page::Calibrate:
            break;
    }
    if (hasTabs(page)) drawTabBar();
}

void enterPage(Page next)
{
    if (next == Page::Calibrate) {
        beginCalibration();
        return;
    }
    Serial.printf("[UI] page -> %s\n", pageName(next));
    page = next;
    pressTarget = pressedWidget = Widget::None;
    dragging = false;
    tft.fillScreen(T->bg);
    drawPageStatic();
}

// =====================================================================
//  หน้า ALERT: ทับทุกหน้าเมื่อน้ำหล่อเย็น >= เกณฑ์ พื้นสลับ alertA / alertB 1 Hz
//  แตะที่ใดก็ได้ = รับทราบ แล้วกลับหน้าเดิม; เตือนอีกครั้งเมื่อลดต่ำกว่า (เกณฑ์ - 3) แล้วกลับมาเกินใหม่
// =====================================================================
void openAlert(const char *why)
{
    if (alertActive || page == Page::Calibrate) return;
    alertActive = true;
    alertArmed = false;
    alertReturnPage = page;
    pressTarget = pressedWidget = Widget::None;
    drawnAlert = NEVER;
    Serial.printf("[ALERT] overheat: %s\n", why);
}

void checkAlert(uint32_t now)
{
    const float cool = getView(now).v[P_COOL];
    if (isnan(cool)) return;
    if (cool < coolLimit - app::COOLANT_ALERT_HYST_C) alertArmed = true;
    if (alertArmed && cool >= coolLimit) openAlert("coolant at or above the limit");
}

void acknowledgeAlert()
{
    alertActive = false;
    playSound(Sound::Stop);
    Serial.println("[ALERT] acknowledged");
    enterPage(alertReturnPage);
}

void drawAlert(uint32_t now)
{
    const float cool = getView(now).v[P_COOL];
    const int phase = blinkPhase(now) ? 1 : 0;
    const int value = isnan(cool) ? -999 : static_cast<int>(lroundf(cool));
    const int key = value * 2 + phase;
    if (key == drawnAlert) return;
    drawnAlert = key;

    const uint16_t bg = phase ? T->alertA : T->alertB;
    const uint16_t fg = T->alertFg;
    tft.fillScreen(bg);
    // สามเหลี่ยมเตือน 34x30 เส้นหนา 3px และเครื่องหมาย "!"
    tft.drawWideLine(160, 36, 175, 62, 3, fg, bg);
    tft.drawWideLine(175, 62, 145, 62, 3, fg, bg);
    tft.drawWideLine(145, 62, 160, 36, 3, fg, bg);
    tft.fillRect(159, 45, 3, 9, fg);
    tft.fillRect(159, 56, 3, 3, fg);

    textAt(FONT_30B, "OVERHEAT", 160, 95, C_BASELINE, fg, bg);

    char text[12];
    if (value == -999) {
        snprintf(text, sizeof(text), "--°C");
    } else {
        snprintf(text, sizeof(text), "%d°C", value);
    }
    const int16_t lw = textWidthIn(FONT_11B, "COOLANT");
    const int16_t vw = textWidthIn(FONT_24B, text);
    const int16_t x0 = 160 - (lw + 6 + vw) / 2;
    textAt(FONT_11B, "COOLANT", x0, 130, L_BASELINE, fg, bg);
    textAt(FONT_24B, text, x0 + lw + 6, 130, L_BASELINE, fg, bg);

    textAt(FONT_12TH, TH_ALERT_MESSAGE, 160, 155, C_BASELINE, fg, bg);
    const int16_t bw = textWidthIn(FONT_12TH, TH_ALERT_ACK) + 44;
    tft.fillRoundRect(160 - bw / 2, 171, bw, 36, 4, T->alertBtnBg);
    textAt(FONT_12TH, TH_ALERT_ACK, 160, 192, C_BASELINE, T->alertBtnFg, T->alertBtnBg);
}

// =====================================================================
//  ธีม DARK / LIGHT / AUTO
//  AUTO: ใช้เซนเซอร์แสง (LDR GPIO34) ถ้าเปิดใช้ใน app_config ไม่งั้นใช้เวลา 06:00-18:00 = LIGHT (ต้องมีเวลาจาก NTP)
//  ตรวจตอนเปิดเครื่องและทุก 1 นาที; เปลี่ยนธีม = วาดส่วนคงที่ใหม่ทั้งหน้า แล้ววาดค่าปัจจุบัน
// =====================================================================
int readLdr()
{
    long sum = 0;
    for (int i = 0; i < 8; i++) sum += analogRead(cyd::LDR_PIN);
    return static_cast<int>(sum / 8);
}

bool evaluateAutoLight(bool current)
{
    if (app::AUTO_THEME_USE_LDR) {
        const int raw = readLdr();  // CYD: ยิ่งสว่าง ค่ายิ่งต่ำ
        if (raw <= app::LDR_LIGHT_AT_OR_BELOW) return true;
        if (raw >= app::LDR_DARK_AT_OR_ABOVE) return false;
        return current;  // อยู่ในช่วง hysteresis: คงธีมเดิม
    }
    const time_t t = time(nullptr);
    if (t < 1700000000) return current;  // ยังไม่มีเวลาจริง
    struct tm local;
    localtime_r(&t, &local);
    return local.tm_hour >= 6 && local.tm_hour < 18;
}

const Theme *themeForMode()
{
    switch (themeMode) {
        case ThemeMode::Light: return &THEME_LIGHT;
        case ThemeMode::Auto: return autoIsLight ? &THEME_LIGHT : &THEME_DARK;
        case ThemeMode::Dark: break;
    }
    return &THEME_DARK;
}

// ใช้ธีมตามโหมด: ถ้าเปลี่ยน ตั้งความสว่างของธีมนั้นแล้ววาดทั้งหน้าใหม่
void applyTheme(bool force)
{
    const Theme *next = themeForMode();
    if (next == T && !force) return;
    T = next;
    applyBacklight(T == &THEME_LIGHT ? backlightLight : backlightDark);
    Serial.printf("[THEME] %s (mode %s)\n", T->name, THEME_MODE_NAMES[static_cast<int>(themeMode)]);
    if (alertActive) {
        drawnAlert = NEVER;
    } else if (page == Page::Calibrate) {
        drawCalibrationScreen();
    } else {
        enterPage(page);
    }
}

void setThemeMode(ThemeMode mode)
{
    themeMode = mode;
    prefs.putUChar("theme", static_cast<uint8_t>(mode));
    if (mode == ThemeMode::Auto) {
        autoIsLight = evaluateAutoLight(T == &THEME_LIGHT);
        lastAutoThemeAt = millis();
    }
    Serial.printf("[THEME] mode -> %s (saved)\n", THEME_MODE_NAMES[static_cast<int>(mode)]);
    if (themeForMode() == T && page == Page::Config) {
        drawSetRowAt(SR_THEME);  // ธีมที่ใช้ไม่เปลี่ยน (เช่น DARK -> AUTO ตอนกลางคืน) วาดแค่ตัวเลือก
    } else {
        applyTheme(false);
    }
}

void updateAutoTheme(uint32_t now)
{
    if (themeMode != ThemeMode::Auto || now - lastAutoThemeAt < app::AUTO_THEME_CHECK_MS) return;
    lastAutoThemeAt = now;
    autoIsLight = evaluateAutoLight(autoIsLight);
    applyTheme(false);
}

// อ่านโหมดธีมและความสว่างของแต่ละธีมจาก NVS (เรียกหลัง loadSettings ก่อนเริ่มจอ)
void loadThemeSettings()
{
    const uint8_t saved = prefs.getUChar("theme", static_cast<uint8_t>(ThemeMode::Dark));
    themeMode = saved <= static_cast<uint8_t>(ThemeMode::Auto) ? static_cast<ThemeMode>(saved) : ThemeMode::Dark;
    backlightDark = prefs.getUChar("bl_d", app::BACKLIGHT_DARK_DEFAULT);
    backlightLight = prefs.getUChar("bl_l", app::BACKLIGHT_LIGHT_DEFAULT);
    if (themeMode == ThemeMode::Auto) autoIsLight = evaluateAutoLight(false);
    lastAutoThemeAt = millis();
    T = themeForMode();
    backlightPercent = T == &THEME_LIGHT ? backlightLight : backlightDark;
    Serial.printf("[THEME] mode %s -> %s, backlight %d%%\n", THEME_MODE_NAMES[static_cast<int>(themeMode)], T->name, backlightPercent);
}

// =====================================================================
//  การทำงานของปุ่ม
// =====================================================================
void saveObdConfig()
{
    char name[33], pin[17];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    prefs.putString("obdname", name);
    prefs.putString("obdpin", pin);
    char mac[18];
    getConfigMac(mac, sizeof(mac));
    prefs.putString("obdmac", mac);
    Serial.printf("[CFG] saved: name=\"%s\" mac=%s pin=%s\n", name, mac[0] != '\0' ? mac : "(none)", pin[0] != '\0' ? "set" : "(none)");
}

void requestReconnect()
{
    Serial.println("[OBD] reconnect requested");
    reconnectReq = true;
}

// เปิดหน้า SCAN: หยุดเชื่อมต่อ แล้วเริ่มค้นหาทันที
void openScan()
{
    Serial.println("[SCAN] scan page opened");
    scanListPage = 0;
    scanState = ScanState::Waiting;
    scanHold = true;
    scanReq = true;
    enterPage(Page::Scan);
}

// ออกจากหน้า SCAN (OBD task กลับไปเชื่อมต่อตามค่าที่ตั้งไว้เอง)
void closeScan(Page next)
{
    scanHold = false;
    requestReconnect();
    enterPage(next);
}

void selectScanDevice(int idx)
{
    if (idx < 0 || idx >= uiScanCount) return;
    const ScanDevice &d = uiScan[idx];
    char mac[18];
    formatMac(d.mac, mac, sizeof(mac));
    setAdapter(d.name[0] != '\0' ? d.name : mac, mac);
    saveObdConfig();
    Serial.printf("[SCAN] selected \"%s\" (%s)\n", d.name, mac);
    closeScan(Page::Config);
}

void openKeyboard(int field)
{
    char name[33], pin[17];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    kbField = field;
    strlcpy(kbBuf, field == 0 ? name : pin, sizeof(kbBuf));
    kbShift = true;
    enterPage(Page::Keyboard);
}

void handleKey(int idx)
{
    const size_t maxLen = (kbField == 0) ? 32 : 16;
    if (idx < 39) {
        const size_t n = strlen(kbBuf);
        if (n < maxLen) {
            kbBuf[n] = keyLabelChar(idx);
            kbBuf[n + 1] = '\0';
        }
        drawKeyboardTextBox();
    } else if (idx == KEY_SPACE) {
        const size_t n = strlen(kbBuf);
        if (n < maxLen) {
            kbBuf[n] = ' ';
            kbBuf[n + 1] = '\0';
        }
        drawKeyboardTextBox();
    } else if (idx == KEY_DEL) {
        const size_t n = strlen(kbBuf);
        if (n > 0) kbBuf[n - 1] = '\0';
        drawKeyboardTextBox();
    } else if (idx == KEY_SHIFT) {
        kbShift = !kbShift;
        for (int i = 0; i < KEY_COUNT; i++) {
            if (i < 39 || i == KEY_SHIFT) drawKey(i, false);
        }
    } else if (idx == KEY_ESC) {
        enterPage(Page::Config);
    } else if (idx == KEY_OK) {
        char name[33], pin[17];
        getConfig(name, sizeof(name), pin, sizeof(pin));
        if (kbField == 0) {
            // พิมพ์ชื่อเอง (MANUAL) = ลืม MAC เดิมเสมอ แม้ชื่อซ้ำของเดิม (อะแดปเตอร์คนละตัวที่ชื่อเหมือนกัน)
            setAdapter(kbBuf, "");
        } else {
            setConfig(name, kbBuf);
        }
        saveObdConfig();
        requestReconnect();
        enterPage(Page::Config);
    }
}

// ความคืบหน้าของการกดค้าง 0-100% (ปัดทีละ 5% ลดการวาดซ้ำ)
int holdProgress(uint32_t now)
{
    const uint32_t held = now - touch.downSince;
    return static_cast<int>(min<uint32_t>(100, held * 100 / app::LONG_PRESS_MS)) / 5 * 5;
}

// ปุ่มกดค้าง: วาดความคืบหน้า (0 = กลับเป็นปกติ)
void drawHoldProgress(Widget w, int progressPercent)
{
    switch (w) {
        case Widget::Gauge: drawGaugeHoldRing(progressPercent); break;
        case Widget::FocusReset: drawFocusResetButton(progressPercent); break;
        case Widget::TripReset: drawTripResetButton(progressPercent); break;
        case Widget::BtnClear: drawClearButton(progressPercent); break;
        case Widget::BtnRecal: drawRecalButton(progressPercent); break;
        default: break;
    }
}

bool isHoldWidget(Widget w)
{
    return w == Widget::Gauge || w == Widget::FocusReset || w == Widget::TripReset || w == Widget::BtnClear || w == Widget::BtnRecal;
}

// กดค้างครบ 2 วินาที
void onLongPress(Widget w)
{
    switch (w) {
        case Widget::Gauge:
            Serial.println("[UI] long press: peak RPM cleared");
            peakRpm = 0;
            drawGaugeHoldRing(0);
            break;
        case Widget::FocusReset:
            Serial.printf("[UI] long press: %s MIN/AVG/MAX reset\n", PID_INFO[FOCUS_ORDER[focusPid]].name);
            resetFocusStats(FOCUS_ORDER[focusPid]);
            drawFocusResetButton(0);
            break;
        case Widget::TripReset:
            Serial.println("[UI] long press: trip reset");
            resetTrip();
            drawTripResetButton(0);
            break;
        case Widget::BtnClear:
            Serial.println("[UI] long press: clear fault codes");
            requestDtc(DtcRequest::Clear);
            drawClearButton(0);
            break;
        case Widget::BtnRecal:
            Serial.println("[UI] long press: starting touch calibration");
            beginCalibration();
            break;
        default:
            break;
    }
}

// แถบเลื่อนความสว่าง: 10-100% ตามตำแหน่งนิ้วบนราง
void setBacklightFromTouch()
{
    const int span = 100 - app::BACKLIGHT_MIN_PERCENT;
    const int percent = constrain(app::BACKLIGHT_MIN_PERCENT + (touch.x - BL_TRACK_X) * span / BL_TRACK_W, app::BACKLIGHT_MIN_PERCENT, 100);
    if (percent == backlightPercent) return;
    applyBacklight(percent);
    drawSetRowAt(SR_BACKLIGHT);
}

void onHold(Widget w, uint32_t now)
{
    if (w == Widget::BlSlider) {
        setBacklightFromTouch();
        return;
    }
    if (!isHoldWidget(w) || pressedWidget != w || longPressDone) return;
    if (w == Widget::BtnClear && dtcBusy()) return;
    const int progress = holdProgress(now);
    if (progress != drawnLongPress) {
        drawHoldProgress(w, progress);
        drawnLongPress = progress;
    }
    if (now - touch.downSince >= app::LONG_PRESS_MS) {
        longPressDone = true;
        pressedWidget = Widget::None;
        playSound(Sound::Confirm);
        onLongPress(w);
    }
}

void setShiftRpm(int value)
{
    shiftRpm = constrain(value, app::SHIFT_RPM_MIN, app::SHIFT_RPM_MAX);
    prefs.putUShort("shift", static_cast<uint16_t>(shiftRpm));
    Serial.printf("[CFG] shift light at %d rpm (saved)\n", shiftRpm);
}

void setCoolLimit(int value)
{
    coolLimit = constrain(value, app::COOLANT_LIMIT_MIN_C, app::COOLANT_LIMIT_MAX_C);
    prefs.putUChar("coollim", static_cast<uint8_t>(coolLimit));
    alertArmed = true;
    Serial.printf("[CFG] overheat limit %d C (saved)\n", coolLimit);
}

void openFocus(int pid)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (FOCUS_ORDER[i] == pid) focusPid = i;
    }
    enterPage(Page::Focus);
}

void onActivate(Widget w, uint32_t now)
{
    (void)now;
    if (w >= Widget::TabDrive && w <= Widget::TabSet) {
        const Page next = static_cast<Page>(w - Widget::TabDrive);
        if (page != next) enterPage(next);
        return;
    }
    if (w >= Widget::Tile0 && w <= Widget::Tile2) {
        openFocus(TILE_PID[w - Widget::Tile0]);
        return;
    }
    switch (w) {
        case Widget::Gauge: openFocus(P_RPM); break;
        case Widget::FocusPrev: focusPid = (focusPid + P_COUNT - 1) % P_COUNT; break;
        case Widget::FocusNext: focusPid = (focusPid + 1) % P_COUNT; break;
        case Widget::ZeroArm:
            if (zeroState == ZeroState::Off || zeroState == ZeroState::Done) {
                zeroState = ZeroState::Armed;  // updateZeroTimer เปลี่ยนเป็น READY เมื่อความเร็ว 0
                Serial.println("[0-100] armed");
            } else {
                zeroState = ZeroState::Off;
                Serial.println("[0-100] cancelled");
            }
            break;
        case Widget::BtnRead: requestDtc(DtcRequest::Read); break;
        case Widget::BtnPrev:
            if (dtcListPage > 0) {
                dtcListPage--;
                drawnDtcKey = NEVER;
            }
            break;
        case Widget::BtnNext:
            if (dtcListPage < dtcPages() - 1) {
                dtcListPage++;
                drawnDtcKey = NEVER;
            }
            break;
        case Widget::FieldName: openScan(); break;
        case Widget::FieldPin: openKeyboard(1); break;
        case Widget::ThemeDark:
        case Widget::ThemeLight:
        case Widget::ThemeAuto: setThemeMode(static_cast<ThemeMode>(w - Widget::ThemeDark)); break;
        case Widget::BtnConnect: requestReconnect(); break;
        case Widget::Sound0:
        case Widget::Sound1:
        case Widget::Sound2:
        case Widget::Sound3:
            setVolume(static_cast<uint8_t>(w - Widget::Sound0), true);
            drawSetRowAt(SR_SOUND);
            playSound(Sound::Confirm);  // ให้ได้ยินระดับใหม่ทันที
            break;
        case Widget::ShiftMinus:
        case Widget::ShiftPlus:
            setShiftRpm(shiftRpm + (w == Widget::ShiftPlus ? app::SHIFT_RPM_STEP : -app::SHIFT_RPM_STEP));
            drawSetRowAt(SR_SHIFT);
            break;
        case Widget::CoolMinus:
        case Widget::CoolPlus:
            setCoolLimit(coolLimit + (w == Widget::CoolPlus ? 1 : -1));
            drawSetRowAt(SR_COOL);
            break;
        case Widget::DispInvert: setDisplayFix(dispFix ^ DISP_INVERT); break;
        case Widget::DispSwapRB: setDisplayFix(dispFix ^ DISP_SWAP_RB); break;
        case Widget::DispFlip: setDisplayFix(dispFix ^ DISP_FLIP); break;
        case Widget::Toast: hideToast(); break;
        case Widget::ScanRow0:
        case Widget::ScanRow1:
        case Widget::ScanRow2:
        case Widget::ScanRow3:
        case Widget::ScanRow4:
            selectScanDevice(scanListPage * SCAN_ROWS + (w - Widget::ScanRow0));
            break;
        case Widget::ScanBack: closeScan(Page::Config); break;
        case Widget::ScanManual:
            scanHold = false;
            requestReconnect();
            openKeyboard(0);
            break;
        case Widget::ScanRescan:
            if (!scanBusy()) {
                scanState = ScanState::Waiting;
                scanReq = true;
                scanListPage = 0;
            }
            break;
        case Widget::ScanPrev:
            if (scanListPage > 0) scanListPage--;
            break;
        case Widget::ScanNext:
            if (scanListPage < scanPages() - 1) scanListPage++;
            break;
        default:
            if (isKey(w)) handleKey(keyIndex(w));
            break;
    }
}

void onRelease(Widget w)
{
    if (w == Widget::BlSlider) saveBacklight();  // จำแยกตามธีมที่ใช้อยู่
    if (isHoldWidget(w) && !longPressDone) drawHoldProgress(w, 0);  // ปล่อยก่อนครบเวลา = แถบเติมหายไป
}

// ปัดซ้าย/ขวาในพื้นที่เนื้อหาของหน้า DRIVE / FOCUS / TRIP / FAULT (วนรอบ)
bool handleSwipe(uint32_t now)
{
    const int dx = touch.lastX - downX, dy = touch.lastY - downY;
    if (static_cast<int>(page) >= SWIPE_PAGE_COUNT || downY < CONTENT_TOP || downY >= CONTENT_BOTTOM) return false;
    if (abs(dx) <= app::SWIPE_MIN_PX || abs(dx) * 2 <= abs(dy) * 3 || now - touch.downSince >= app::SWIPE_MAX_MS) return false;
    const int next = (static_cast<int>(page) + (dx < 0 ? 1 : SWIPE_PAGE_COUNT - 1)) % SWIPE_PAGE_COUNT;
    Serial.printf("[TOUCH] swipe %s\n", dx < 0 ? "left" : "right");
    playSound(Sound::Click);
    enterPage(static_cast<Page>(next));
    return true;
}

// หน้า SET: ลากแนวตั้งเกิน DRAG_START_PX = เลื่อนรายการ (ยกเลิกการแตะ)
void handleSetDrag()
{
    const int dx = touch.x - downX, dy = touch.y - downY;
    if (!dragging) {
        if (page != Page::Config || !contains(SET_AREA, downX, downY) || abs(dy) <= app::DRAG_START_PX) return;
        if (pressTarget == Widget::BlSlider) {
            if (abs(dy) <= 12 || abs(dy) <= abs(dx)) return;  // ยังเป็นการเลื่อนแถบความสว่าง
            if (backlightPercent != blBeforeDrag) applyBacklight(blBeforeDrag);
        }
        dragging = true;
        const Widget t = pressTarget;
        pressTarget = pressedWidget = Widget::None;
        redrawWidget(t);
    }
    const int next = constrain(dragStartScroll - dy, 0, SET_MAX_SCROLL) & ~1;  // ทีละ 2px ลดการวาดซ้ำ
    if (next != setScroll) {
        setScroll = next;
        drawSetList();
    }
}

void handleTouchEvents(uint32_t now)
{
    if (touch.pressEdge) {
        downX = touch.x;
        downY = touch.y;
        dragging = false;
        dragStartScroll = setScroll;
        blBeforeDrag = backlightPercent;
        pressTarget = hitTest(page, touch.x, touch.y);
        pressedWidget = pressTarget;
        Serial.printf("[TOUCH] down  x=%3d y=%3d  z=%4d  page=%s target=%s\n", touch.x, touch.y, touch.z, pageName(page), widgetName(pressTarget));
        longPressDone = false;
        drawnLongPress = -1;
        redrawWidget(pressTarget);
        if (pressTarget != Widget::None) playSound(Sound::Click);
    }

    if (touch.down) {
        handleSetDrag();
        if (!dragging && pressTarget != Widget::None) {
            const bool stillInside = hitTest(page, touch.x, touch.y) == pressTarget;
            const Widget wanted = stillInside ? pressTarget : Widget::None;
            if (wanted != pressedWidget && pressTarget != Widget::BlSlider) {
                pressedWidget = wanted;
                redrawWidget(pressTarget);
                if (isHoldWidget(pressTarget) && wanted == Widget::None) drawHoldProgress(pressTarget, 0);
            }
            onHold(pressTarget, now);
        }
    }

    if (touch.releaseEdge) {
        const Widget target = pressTarget;
        pressedWidget = Widget::None;
        pressTarget = Widget::None;
        if (dragging) {
            dragging = false;
            return;
        }
        if (target != Widget::BlSlider && !longPressDone && handleSwipe(now)) return;
        const Widget releasedOn = hitTest(page, touch.lastX, touch.lastY);
        const bool clicked = target != Widget::None && releasedOn == target && !longPressDone && target != Widget::BlSlider;
        Serial.printf("[TOUCH] up    x=%3d y=%3d  target=%s  %s\n", touch.lastX, touch.lastY, widgetName(target), clicked ? "click" : "(not a click)");
        const Page before = page;
        redrawWidget(target);  // กลับเป็นสีปกติก่อน (ถ้า onActivate เปลี่ยนหน้า หน้าใหม่จะวาดใหม่ทั้งหมด)
        if (clicked) onActivate(target, now);
        if (page == before) onRelease(target);
    }
}

void handleCalibration(uint32_t now)
{
    if (calMessageUntil != 0) {
        if (now >= calMessageUntil) {
            calMessageUntil = 0;
            if (calResultOk) {
                enterPage(Page::Drive);
            } else {
                beginCalibration();
            }
        }
        return;
    }

    if (calWaitRelease) {
        if (!touch.down && !touch.touchingNow) {
            calWaitRelease = false;
            eraseCalibrationTarget(calStep);
            calStep++;
            if (calStep >= 4) {
                const bool ok = computeCalibration();
                calResultOk = ok;
                useBuiltinFont();
                tft.fillRect(0, 60, SCREEN_W, 130, T->bg);
                tft.setTextDatum(TC_DATUM);
                tft.setTextColor(ok ? T->ok : T->bad, T->bg);
                tft.drawString(ok ? "Calibration saved" : "Calibration FAILED", SCREEN_W / 2, 100, 4);
                tft.setTextColor(T->muted, T->bg);
                tft.drawString(ok ? "Starting the dashboard..." : "Please try again", SCREEN_W / 2, 140, 2);
                tft.setTextDatum(TL_DATUM);
                if (ok) saveCalibration();
                calMessageUntil = now + 1500;
            } else {
                char text[24];
                snprintf(text, sizeof(text), "Point %d / 4", calStep + 1);
                drawText(text, 112, 176, 2, T->accent, T->bg, 96);
                drawCalibrationTarget();
                calSamples = 0;
                calAccepted = 0;
                calSumX = calSumY = 0;
            }
        }
        return;
    }

    if (touch.touchingNow) {
        calSamples++;
        if (calSamples > 3) {
            calSumX += touch.rawX;
            calSumY += touch.rawY;
            calAccepted++;
        }
        if (calAccepted >= app::CAL_SAMPLES) {
            calRawX[calStep] = static_cast<int>(calSumX / calAccepted);
            calRawY[calStep] = static_cast<int>(calSumY / calAccepted);
            Serial.printf("[CAL] point %d raw = (%d, %d)\n", calStep + 1, calRawX[calStep], calRawY[calStep]);
            int tx, ty;
            calTarget(calStep, tx, ty);
            tft.fillCircle(tx, ty, 5, T->ok);
            calWaitRelease = true;
        }
    } else {
        calSamples = 0;
        calAccepted = 0;
        calSumX = calSumY = 0;
    }
}

// =====================================================================
//  LED ของบอร์ดบอกสถานะ และ Serial
// =====================================================================
void updateLed(uint32_t now)
{
    static int lastState = -1;
    int state;  // 0 = แดง (ออฟไลน์), 1 = น้ำเงิน (กำลังต่อ), 2 = เหลือง (ต่อแล้ว ECU เงียบ), 3 = เขียว (REAL)
    const LinkState ls = link;
    if (isRealData(now)) state = 3;
    else if (ls == LinkState::Online) state = 2;
    else if (ls == LinkState::Connecting || ls == LinkState::BtStart || ls == LinkState::Init || ls == LinkState::Scanning) state = 1;
    else state = 0;
    if (state == lastState) return;
    lastState = state;
    cyd::ledSet(state == 0 || state == 2, state == 2 || state == 3, state == 1);
}

void printHelp()
{
    Serial.println("[HELP] r = reconnect Bluetooth,  d = read fault codes,  c = calibrate touch,  k = show calibration,");
    Serial.println("[HELP] a = show the OVERHEAT alert (test),  v = next theme (DARK/LIGHT/AUTO),  l = light sensor reading,");
    Serial.println("[HELP] b = next sound volume (OFF/LOW/MID/HIGH),  p = play the next sound (test),");
    Serial.printf("[HELP] f = reset display fixes (invert / swap red-blue / flip) for this %s panel,  h = help\n", DISPLAY_CHIP);
}

void printStatus(uint32_t now)
{
    const Rect2View v = getView(now);
    Serial.printf("[STATUS] page=%s bt=%s src=%s rpm=%.0f speed=%.0f cool=%.0f thr=%.0f load=%.0f iat=%.0f fuel=%.0f volt=%.1f bl=%d%%\n",
                  pageName(page), linkName(link), v.real ? "LIVE" : "NO DATA", v.v[P_RPM], v.v[P_SPEED], v.v[P_COOL], v.v[P_THR], v.v[P_LOAD], v.v[P_IAT],
                  v.v[P_FUEL], v.v[P_VOLT], backlightPercent);
}

void handleSerialInput(uint32_t now)
{
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'b' || c == 'B') {
            setVolume(soundVolume + 1, true);
            if (page == Page::Config && !alertActive) drawSetRowAt(SR_SOUND);
            playSound(Sound::Confirm);
        } else if (c == 'p' || c == 'P') {
            playNextDemoSound();
        } else if (c == 'a' || c == 'A') {
            openAlert("test from Serial ('a')");
        } else if (c == 'v' || c == 'V') {
            setThemeMode(static_cast<ThemeMode>((static_cast<int>(themeMode) + 1) % 3));
        } else if (c == 'l' || c == 'L') {
            Serial.printf("[THEME] LDR raw=%d (LIGHT at <= %d, DARK at >= %d)\n", readLdr(), app::LDR_LIGHT_AT_OR_BELOW, app::LDR_DARK_AT_OR_ABOVE);
        } else if (c == 'r' || c == 'R') {
            requestReconnect();
        } else if (c == 'd' || c == 'D') {
            requestDtc(DtcRequest::Read);
        } else if (c == 'c' || c == 'C') {
            beginCalibration();
        } else if (c == 'k' || c == 'K') {
            Serial.printf("[TOUCH] calibration: %s", cal.valid ? "valid" : "NOT calibrated");
            if (cal.valid) Serial.printf("  swapXY=%s x: %.4f*u%+.1f  y: %.4f*v%+.1f", cal.swapXY ? "yes" : "no", cal.xb, cal.xa, cal.yb, cal.ya);
            Serial.printf("\n[TOUCH] last raw=(%d,%d) z=%d\n", touch.rawX, touch.rawY, touch.z);
        } else if (c == 'f' || c == 'F') {
            setDisplayFix(0);  // กู้คืนเมื่อปรับจอแล้วอ่านไม่ออก
        } else if (c == 'h' || c == 'H' || c == '?') {
            printHelp();
        }
    }
}

// =====================================================================
//  เหตุการณ์ที่มีเสียง: ดูการเปลี่ยนสถานะทุก 200 ms (เรียกใน loop พร้อมการวาดจอ)
// =====================================================================
void soundEvents(uint32_t now)
{
    static bool wasConnected = false;
    static DtcState lastDtcState = DtcState::Idle;
    static int lastDtcCount = 0;
    static bool redline = false;
    static bool wasAlert = false;
    static uint32_t alertSoundAt = 0;
    static ScanState lastScan = ScanState::Idle;

    // ต่ออะแดปเตอร์ได้ / หลุด (หลุด = แถบเตือนด้วย)
    const bool connected = btConnected();
    if (connected != wasConnected) {
        playSound(connected ? Sound::BtOn : Sound::BtOff);
        if (!connected && link == LinkState::Backoff) {
            char sub[16];
            snprintf(sub, sizeof(sub), "RETRY %lus", static_cast<unsigned long>(app::BT_RETRY_MS / 1000));
            showToast(ToastKind::Bad, "OBD LINK LOST", sub, false);
        }
    }
    wasConnected = connected;

    // ผลอ่าน/ลบรหัสข้อผิดพลาด (ทั้งจากจอ เว็บ และ Serial)
    const DtcData d = snapshotDtc();
    if (d.state != lastDtcState && lastDtcState == DtcState::Busy) {
        if (d.state == DtcState::Error) {
            playSound(Sound::Error);
        } else if (d.msg == DtcMsg::Cleared) {
            playSound(Sound::DtcCleared);
            showToast(ToastKind::Ok, "CODES CLEARED", d.mil ? "MIL ON" : "MIL OFF", false);
        } else {
            playSound(d.count > 0 || d.mil ? Sound::DtcFound : Sound::DtcNone);
        }
    } else if (d.state == DtcState::Done && d.count > lastDtcCount) {
        playSound(Sound::DtcFound);  // พบรหัสใหม่จากการตรวจอัตโนมัติ
    }
    lastDtcState = d.state;
    if (d.state == DtcState::Done) lastDtcCount = d.count;

    // รอบถึง shift light (ดังอีกครั้งเมื่อรอบลดต่ำกว่า shift - RPM_BEEP_HYST ก่อน)
    const float rpm = getView(now).v[P_RPM];
    if (!isnan(rpm)) {
        if (!redline && rpm >= shiftRpm) {
            redline = true;
            playSound(Sound::Redline);
        } else if (redline && rpm < shiftRpm - app::RPM_BEEP_HYST) {
            redline = false;
        }
    }

    // หน้า ALERT: เตือนซ้ำจนกว่าจะแตะรับทราบ
    if (alertActive && (!wasAlert || now - alertSoundAt >= app::ALERT_SOUND_EVERY_MS)) {
        alertSoundAt = now;
        playSound(Sound::Alert);
    }
    wasAlert = alertActive;

    // ค้นหา Bluetooth เสร็จ
    const ScanState scan = scanState;
    if (scan != lastScan && (scan == ScanState::Done || scan == ScanState::Error)) playSound(scan == ScanState::Done ? Sound::ScanDone : Sound::Error);
    lastScan = scan;
}

// =====================================================================
//  เครือข่าย: ลองต่อ WiFi (STA) ก่อน ถ้าไม่ได้ภายในเวลาที่กำหนดจะเปิด AP ของตัวบอร์ดเอง
// =====================================================================
enum class NetMode : uint8_t { Off, Connecting, Sta, Ap };
NetMode netMode = NetMode::Off;
uint32_t netSince = 0;
bool mdnsOn = false;
WebServer server(app::HTTP_PORT);

const char *netModeName(NetMode m)
{
    switch (m) {
        case NetMode::Off: return "OFF";
        case NetMode::Connecting: return "CONNECTING";
        case NetMode::Sta: return "WIFI";
        case NetMode::Ap: return "AP";
    }
    return "?";
}

IPAddress netIp()
{
    return netMode == NetMode::Ap ? WiFi.softAPIP() : WiFi.localIP();
}

// IP ที่แถบสถานะบนจอแสดง: WiFi = "192.168.1.13", AP ของบอร์ด = "AP 192.168.4.1", ยังไม่มี IP = ""
IPAddress labelIp;
void updateNetLabel()
{
    labelIp = netMode == NetMode::Sta || netMode == NetMode::Ap ? netIp() : IPAddress();
    if (netMode == NetMode::Sta) {
        snprintf(netLabel, sizeof(netLabel), "%s", labelIp.toString().c_str());
    } else if (netMode == NetMode::Ap) {
        snprintf(netLabel, sizeof(netLabel), "AP %s", labelIp.toString().c_str());
    } else {
        netLabel[0] = '\0';
    }
}

void startMdns()
{
    if (mdnsOn) MDNS.end();
    mdnsOn = MDNS.begin(app::MDNS_NAME);
    Serial.printf("[NET] mDNS %s.local %s\n", app::MDNS_NAME, mdnsOn ? "ready" : "FAILED");
}

void startAp(uint32_t now)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    const bool ok = WiFi.softAP(app::AP_SSID, AP_PASS, app::AP_CHANNEL);
    netMode = NetMode::Ap;
    netSince = now;
    Serial.printf("[NET] AP \"%s\" %s -> http://%s\n", app::AP_SSID, ok ? "started" : "FAILED", WiFi.softAPIP().toString().c_str());
    updateNetLabel();
    if (AP_PASS_IS_DEFAULT) Serial.println("[NET] WARNING: AP uses the default password: set CYD_AP_PASSWORD in secrets.h");
    startMdns();
}

void beginSta(uint32_t now)
{
    WiFi.mode(WIFI_STA);
    Serial.println("[NET] connecting to WiFi ...");  // ไม่พิมพ์ชื่อ WiFi เพื่อไม่ให้หลุดเวลาแชร์ log
    WiFi.begin(WIFI_SSID, strlen(WIFI_PASSWORD) > 0 ? WIFI_PASSWORD : nullptr);
    netMode = NetMode::Connecting;
    netSince = now;
    updateNetLabel();
}

void startNet(uint32_t now)
{
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("[NET] no WiFi name in secrets.h: using the board's own AP");
        startAp(now);
        return;
    }
    beginSta(now);
}

// Connecting: ต่อได้ = Sta / หมดเวลา = เปิด AP
// Sta: หลุดนานเกิน WIFI_LOST_TO_AP_MS = เปิด AP สำรอง (netSince = เวลาล่าสุดที่ยังต่ออยู่)
// Ap: ถ้ามีชื่อ WiFi และไม่มีใครต่อ AP อยู่ ลองต่อ WiFi ใหม่ทุก WIFI_RETRY_MS (เช่น เปิด hotspot มือถือทีหลัง)
void updateNet(uint32_t now)
{
    switch (netMode) {
        case NetMode::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                netMode = NetMode::Sta;
                netSince = now;
                Serial.printf("[NET] WiFi connected, RSSI %d dBm -> http://%s\n", WiFi.RSSI(), WiFi.localIP().toString().c_str());
                configTime(app::GMT_OFFSET_SEC, 0, app::NTP_SERVER);  // เวลาจริงสำหรับธีม AUTO แบบตามเวลา
                startMdns();
                updateNetLabel();
            } else if (now - netSince >= app::WIFI_CONNECT_TIMEOUT_MS) {
                Serial.println("[NET] WiFi not reachable in time: switching to the board's own AP");
                startAp(now);
            }
            break;
        case NetMode::Sta:
            if (WiFi.status() == WL_CONNECTED) {
                netSince = now;
                if (WiFi.localIP() != labelIp) updateNetLabel();  // ต่อใหม่แล้วได้ IP ใหม่
            } else if (now - netSince >= app::WIFI_LOST_TO_AP_MS) {
                Serial.println("[NET] WiFi lost: switching to the board's own AP");
                startAp(now);
            }
            break;
        case NetMode::Ap:
            if (strlen(WIFI_SSID) > 0 && WiFi.softAPgetStationNum() == 0 && now - netSince >= app::WIFI_RETRY_MS) {
                Serial.println("[NET] no one on the AP: trying WiFi again");
                beginSta(now);
            }
            break;
        case NetMode::Off:
            break;
    }
}

// =====================================================================
//  เว็บ API (ทำงานใน loop() เดียวกับ UI จึงไม่แย่งข้อมูลกับหน้าจอ)
// =====================================================================
const char *tf(bool b)
{
    return b ? "true" : "false";
}

// ใส่ค่าลง JSON: ค่า NaN เป็น null
void jsonNum(char *out, size_t size, float v, const char *fmt)
{
    if (isnan(v)) {
        snprintf(out, size, "null");
    } else {
        snprintf(out, size, fmt, v);
    }
}

// escape เครื่องหมาย " และ \ ของข้อความที่ผู้ใช้พิมพ์ (เช่นชื่อ OBD) ก่อนใส่ใน JSON
void jsonEscape(const char *in, char *out, size_t size)
{
    size_t j = 0;
    for (; *in != '\0' && j + 2 < size; in++) {
        const unsigned char c = static_cast<unsigned char>(*in);
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = static_cast<char>(c);
        } else if (c < 0x20) {
            out[j++] = ' ';
        } else {
            out[j++] = static_cast<char>(c);
        }
    }
    out[j] = '\0';
}

const char *dtcStateName(DtcState s)
{
    switch (s) {
        case DtcState::Idle: return "IDLE";
        case DtcState::Busy: return "BUSY";
        case DtcState::Done: return "DONE";
        case DtcState::Error: return "ERROR";
    }
    return "?";
}

const char *dtcMsgName(DtcMsg m)
{
    switch (m) {
        case DtcMsg::None: return "";
        case DtcMsg::Cleared: return "CLEARED";
        case DtcMsg::NoEcu: return "NO_ECU";
        case DtcMsg::ClearFailed: return "CLEAR_FAILED";
        case DtcMsg::Timeout: return "TIMEOUT";
        case DtcMsg::NoLink: return "NO_LINK";
    }
    return "?";
}

// หน้าเว็บ: ส่งเป็นชิ้น (chunked) HEAD -> ค่าจากเฟิร์มแวร์ -> TAIL
// ค่าที่แทรก: เกณฑ์น้ำหล่อเย็น, เวอร์ชัน และคำอธิบายไทยของรหัส (ใช้ตาราง ui_text_th.h ชุดเดียวกับหน้าจอ)
void handleIndex()
{
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent_P(WEB_PAGE_HEAD);

    String js;
    js.reserve(4800);
    char esc[160];
    char line[224];
    jsonEscape(app::FW_VERSION, esc, sizeof(esc));
    snprintf(line, sizeof(line), "<script>const CFG={limit:%d,fw:\"%s\"};const DTC_TH={", coolLimit, esc);
    js += line;
    for (const DtcThai &t : DTC_TH_TABLE) {
        jsonEscape(t.th, esc, sizeof(esc));
        snprintf(line, sizeof(line), "\"%s\":\"%s\",", t.code, esc);
        js += line;
    }
    jsonEscape(TH_DTC_MANUFACTURER, esc, sizeof(esc));
    snprintf(line, sizeof(line), "};const DTC_TH_MFR=\"%s\";", esc);
    js += line;
    jsonEscape(TH_DTC_GENERIC, esc, sizeof(esc));
    snprintf(line, sizeof(line), "const DTC_TH_GEN=\"%s\";</script>\n", esc);
    js += line;
    server.sendContent(js);

    server.sendContent_P(WEB_PAGE_TAIL);
    server.sendContent("");  // จบการส่งแบบ chunked
}

void handleStatus()
{
    const uint32_t now = millis();
    const Rect2View v = getView(now);
    char rpm[16], spd[16], cool[16], thr[16], load[16], iat[16], fuel[16], volt[16];
    jsonNum(rpm, sizeof(rpm), v.v[P_RPM], "%.0f");
    jsonNum(spd, sizeof(spd), v.v[P_SPEED], "%.0f");
    jsonNum(cool, sizeof(cool), v.v[P_COOL], "%.0f");
    jsonNum(thr, sizeof(thr), v.v[P_THR], "%.1f");
    jsonNum(load, sizeof(load), v.v[P_LOAD], "%.1f");
    jsonNum(iat, sizeof(iat), v.v[P_IAT], "%.0f");
    jsonNum(fuel, sizeof(fuel), v.v[P_FUEL], "%.1f");
    jsonNum(volt, sizeof(volt), v.v[P_VOLT], "%.1f");

    // ค่าที่รถไม่รองรับ (ชื่อตรงกับ key ใน JSON) หน้าเว็บแสดง N/A
    static constexpr const char *KEYS[P_COUNT] = {"rpm", "speed", "cool", "throttle", "load", "intake", "fuel", "volt"};
    char na[96] = "";
    for (int i = 0; i < P_COUNT; i++) {
        if (!v.na[i]) continue;
        if (na[0] != '\0') strlcat(na, ",", sizeof(na));
        strlcat(na, "\"", sizeof(na));
        strlcat(na, KEYS[i], sizeof(na));
        strlcat(na, "\"", sizeof(na));
    }

    static char body[768];
    snprintf(body, sizeof(body),
             "{\"link\":\"%s\",\"connected\":%s,\"real\":%s,\"net\":\"%s\",\"ip\":\"%s\",\"backlight\":%d,\"heap\":%lu,"
             "\"rpm\":%s,\"speed\":%s,\"cool\":%s,\"throttle\":%s,\"load\":%s,\"intake\":%s,\"fuel\":%s,\"volt\":%s,\"na\":[%s]}",
             linkName(link), tf(btConnected()), tf(v.real), netModeName(netMode), netIp().toString().c_str(), backlightPercent,
             static_cast<unsigned long>(ESP.getFreeHeap()), rpm, spd, cool, thr, load, iat, fuel, volt, na);
    server.send(200, "application/json", body);
}

void handleDtc()
{
    const DtcData d = snapshotDtc();
    // รหัสละไม่เกิน ~50 ไบต์ (ข้อความยาวสุดใน DTC_TABLE 22 ตัวอักษร) จองไว้ 64 ไบต์ต่อรหัส พอสำหรับ MAX_DTC รหัส
    // ถ้าที่ไม่พอจริง หยุดใส่รหัส และ "count" บอกจำนวนที่ส่งจริง (JSON ยังถูกต้องเสมอ)
    static char codes[app::MAX_DTC * 64];
    int n = 0;
    int shown = 0;
    codes[0] = '\0';
    for (int i = 0; i < d.count; i++) {
        const int len = snprintf(codes + n, sizeof(codes) - n, "%s{\"code\":\"%s\",\"text\":\"%s\"}", i > 0 ? "," : "", d.codes[i], dtcText(d.codes[i]));
        if (len < 0 || n + len >= static_cast<int>(sizeof(codes))) {
            codes[n] = '\0';
            break;
        }
        n += len;
        shown++;
    }
    static char body[sizeof(codes) + 128];
    snprintf(body, sizeof(body), "{\"state\":\"%s\",\"msg\":\"%s\",\"mil\":%s,\"count\":%d,\"codes\":[%s]}", dtcStateName(d.state), dtcMsgName(d.msg),
             tf(d.mil), shown, codes);
    server.send(200, "application/json", body);
}

// กัน CSRF และ DNS rebinding สำหรับคำสั่ง POST ทุกตัว
//  - ต้องมี header "X-CYD: 1": เว็บอื่นใส่ header เองข้ามโดเมนได้ต้องผ่าน preflight (OPTIONS) ซึ่งบอร์ดไม่ตอบ จึงถูกเบราว์เซอร์บล็อก
//  - ต้องเรียกผ่าน IP ของบอร์ด หรือชื่อ mDNS เท่านั้น (กันโดเมนอื่นที่ชี้ DNS มาที่ IP บอร์ด)
bool hostAllowed()
{
    String host = server.hostHeader();
    const int colon = host.indexOf(':');
    if (colon >= 0) host.remove(colon);
    host.toLowerCase();
    return host == netIp().toString() || host == app::MDNS_NAME || host == String(app::MDNS_NAME) + ".local";
}

bool postAllowed()
{
    if (server.header("X-CYD") != "1") {
        server.send(403, "application/json", "{\"error\":\"missing X-CYD header\"}");
        return false;
    }
    if (!hostAllowed()) {
        server.send(403, "application/json", "{\"error\":\"use the board IP or cyd-obd.local\"}");
        return false;
    }
    return true;
}

void handleDtcRead()
{
    if (!postAllowed()) return;
    requestDtc(DtcRequest::Read);
    server.send(202, "application/json", "{\"ok\":true}");
}

void handleDtcClear()
{
    if (!postAllowed()) return;
    if (server.arg("confirm") != "yes") {
        server.send(400, "application/json", "{\"error\":\"confirm=yes is required\"}");
        return;
    }
    // ไม่ลบรหัสระหว่างรถวิ่ง (ดูจากค่าความเร็วจริงล่าสุด)
    const uint32_t now = millis();
    const Rect2View v = getView(now);
    if (v.real && !isnan(v.v[P_SPEED]) && v.v[P_SPEED] > 0) {
        server.send(409, "application/json", "{\"error\":\"vehicle is moving\"}");
        return;
    }
    requestDtc(DtcRequest::Clear);
    server.send(202, "application/json", "{\"ok\":true}");
}

void handleConfigGet()
{
    char name[33], pin[17], mac[18], esc[80];
    getConfig(name, sizeof(name), pin, sizeof(pin));
    getConfigMac(mac, sizeof(mac));
    jsonEscape(name, esc, sizeof(esc));
    static char body[192];
    snprintf(body, sizeof(body), "{\"name\":\"%s\",\"mac\":\"%s\",\"pinSet\":%s}", esc, mac, tf(pin[0] != '\0'));
    server.send(200, "application/json", body);
}

void handleConfigPost()
{
    if (!postAllowed()) return;
    if (!server.hasArg("name") || !server.hasArg("pin")) {
        server.send(400, "application/json", "{\"error\":\"name and pin are required\"}");
        return;
    }
    const String name = server.arg("name");
    const String pin = server.arg("pin");
    if (name.length() > 32 || pin.length() > 16) {
        server.send(400, "application/json", "{\"error\":\"name max 32, pin max 16 characters\"}");
        return;
    }
    setConfig(name.c_str(), pin.c_str());
    saveObdConfig();
    requestReconnect();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleBacklight()
{
    if (!postAllowed()) return;
    if (!server.hasArg("value")) {
        server.send(400, "application/json", "{\"error\":\"value is required\"}");
        return;
    }
    applyBacklight(server.arg("value").toInt());
    saveBacklight();  // จำแยกตามธีมที่ใช้อยู่
    static char body[32];
    snprintf(body, sizeof(body), "{\"backlight\":%d}", backlightPercent);
    server.send(200, "application/json", body);
}

void handleReconnect()
{
    if (!postAllowed()) return;
    requestReconnect();
    server.send(202, "application/json", "{\"ok\":true}");
}

void handleNotFound()
{
    server.send(404, "text/plain", "not found");
}

void setupWeb()
{
    static const char *HEADERS[] = {"X-CYD"};
    server.collectHeaders(HEADERS, 1);  // WebServer เก็บเฉพาะ header ที่ขอไว้
    server.on("/", HTTP_GET, handleIndex);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/dtc", HTTP_GET, handleDtc);
    server.on("/api/dtc/read", HTTP_POST, handleDtcRead);
    server.on("/api/dtc/clear", HTTP_POST, handleDtcClear);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/backlight", HTTP_POST, handleBacklight);
    server.on("/api/reconnect", HTTP_POST, handleReconnect);
    server.onNotFound(handleNotFound);
    server.begin();
}

}  // namespace

void setup()
{
    Serial.begin(app::SERIAL_BAUD);
    delay(200);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("CYD OBD-II Bluetooth Dashboard (Classic SPP)");
    Serial.println("==========================================");

    cyd::ledInit();
    loadSettings();
    loadThemeSettings();  // ธีม DARK / LIGHT / AUTO และความสว่างของแต่ละธีม
    soundBegin();         // ลำโพง GPIO26 + task เล่นเสียง (ความดังจาก NVS)

    tft.init();
    applyDisplayFix();  // แนวนอน (rotation 1 หรือ 3) + กลับสี/สลับแดงน้ำเงินตามที่ตั้งไว้
    tft.setTextDatum(TL_DATUM);
    tft.fillScreen(T->bg);
    backlightBegin();

    pinMode(cyd::TOUCH_PIN_CS, OUTPUT);
    digitalWrite(cyd::TOUCH_PIN_CS, HIGH);
    touchSpi.begin(cyd::TOUCH_PIN_CLK, cyd::TOUCH_PIN_MISO, cyd::TOUCH_PIN_MOSI, cyd::TOUCH_PIN_CS);

    randomSeed(esp_random());
    printHelp();

    // เครือข่าย: ลอง WiFi ก่อน แล้วค่อยเปิดเว็บ (ถ้าไม่ได้จะเปิด AP ของตัวบอร์ดภายหลังใน loop)
    startNet(millis());
    setupWeb();

    // OBD task: Bluetooth + ELM327 state machine ทำงานแยกจาก UI (ปักที่ core 0 ร่วมกับ Bluetooth stack)
    xTaskCreatePinnedToCore(obdTask, "obd", 8192, nullptr, 1, nullptr, 0);

    if (cal.valid) {
        enterPage(Page::Drive);
    } else {
        beginCalibration();
    }
    playSound(Sound::Boot);
}

void loop()
{
    const uint32_t now = millis();

    // 1) จอสัมผัส
    pollTouch(now);
    if (page == Page::Calibrate) {
        if (touch.pressEdge || touch.releaseEdge || touch.touchingNow || calWaitRelease || calMessageUntil != 0) handleCalibration(now);
    } else if (alertActive) {
        if (touch.pressEdge) acknowledgeAlert();  // แตะที่ใดก็ได้ = รับทราบ
    } else {
        handleTouchEvents(now);
    }

    // ออกจากหน้า SCAN ทางอื่น (เช่น calibrate จาก Serial) = ปล่อยให้ OBD task กลับไปเชื่อมต่อ
    if (scanHold && page != Page::Scan) scanHold = false;

    // 2) LED, Serial
    updateLed(now);
    handleSerialInput(now);

    // 2.5) เครือข่ายและเว็บ
    updateNet(now);
    server.handleClient();

    // 3) วาดจอ: ค่าตัวเลข 5 ครั้ง/วินาที วาดเฉพาะส่วนที่เปลี่ยน; หน้า ALERT ทับทุกหน้าเมื่อน้ำหล่อเย็นร้อนเกิน
    if (page != Page::Calibrate) {
        if (now - lastDisplayAt >= app::DISPLAY_REFRESH_MS) {
            lastDisplayAt = now;
            updateAutoTheme(now);
            updateStats(now);  // สถิติ, trip, กราฟ, 0-100, แถบเตือน
            checkAlert(now);
            soundEvents(now);
            if (!alertActive && page != Page::Keyboard) {
                if (hasTabs(page)) updateFaultBadge();
                switch (page) {
                    case Page::Drive: updateDashboard(now); break;
                    case Page::Focus: updateFocusPage(now); break;
                    case Page::Trip: updateTripPage(now); break;
                    case Page::Faults: updateFaultsPage(); break;
                    case Page::Config: if (!dragging) updateSetPage(); break;
                    case Page::Scan: updateScanPage(now); break;
                    default: break;
                }
            }
        }
        if (alertActive) {
            drawAlert(now);  // พื้นสลับสี 1 Hz
        } else if (page == Page::Scan) {
            updateScanHeader(now);  // นับถอยหลัง/จุดกะพริบ
        } else if (hasTabs(page)) {
            updateStatusBar(now);
            if (page == Page::Drive) updateShiftLight(now);  // กะพริบ 4 Hz เมื่อถึงรอบ shift
            updateToast(now);
        }
    }

    if (now - lastStatusAt >= app::STATUS_PRINT_MS) {
        lastStatusAt = now;
        printStatus(now);
    }
}
