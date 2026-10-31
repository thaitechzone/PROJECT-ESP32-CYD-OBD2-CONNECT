#pragma once

// =====================================================================
//  app_config.h -- ค่าที่ "ปรับได้" ของโปรเจกต์ OBD-II Bluetooth (Classic SPP + ELM327)
//
//  board_config.h = ข้อเท็จจริงของฮาร์ดแวร์ (ไม่แก้)
//  app_config.h   = ค่าที่เราอยากลองปรับ (แก้ได้เลย แล้ว upload ใหม่)
// =====================================================================

#include <Arduino.h>

namespace app {

constexpr uint32_t SERIAL_BAUD = 115200;  // ต้องตรงกับ monitor_speed ใน platformio.ini

// ---- Bluetooth / ELM327 ----
inline constexpr char DEFAULT_OBD_NAME[] = "OBDII";  // ชื่อ Bluetooth ของอะแดปเตอร์ (ตั้งใหม่ได้ในหน้า CONFIG)
inline constexpr char DEFAULT_OBD_PIN[] = "1234";    // รหัสจับคู่ (อะแดปเตอร์ ELM327 ส่วนใหญ่ใช้ 1234 หรือ 0000)
inline constexpr char BT_LOCAL_NAME[] = "CYD-OBD";   // ชื่อของบอร์ดเราเองตอนเป็น master
constexpr uint32_t BT_RETRY_MS = 4000;               // เชื่อมไม่สำเร็จ/หลุด รอกี่ ms ก่อนลองใหม่
constexpr uint32_t CMD_TIMEOUT_MS = 1500;            // รอคำตอบของคำสั่ง AT / PID ปกติกี่ ms
constexpr uint32_t CMD_TIMEOUT_INIT_MS = 3500;       // ATZ
constexpr uint32_t CMD_TIMEOUT_PROTO_MS = 12000;     // 0100 ครั้งแรก (ELM327 ต้องค้นหา protocol "SEARCHING...")
constexpr uint32_t CMD_TIMEOUT_DTC_MS = 6000;        // อ่าน/ลบรหัสข้อผิดพลาด (ECU บางรุ่นตอบช้า)
constexpr uint32_t ERROR_PAUSE_MS = 250;             // ECU ตอบ NO DATA แล้วหยุดพักก่อนถามต่อกี่ ms
constexpr uint32_t DATA_FRESH_MS = 4000;             // ไม่มีค่าจริงใหม่เกินนี้ = ถือว่ายังไม่ได้ค่าจากรถ (แถบหัวขึ้น WAITING FOR ECU)
constexpr uint32_t VALUE_STALE_MS = 7000;            // ค่าของ PID ใดเก่ากว่านี้จะแสดง "--"
constexpr uint8_t MAX_DTC = 16;                      // จำนวนรหัสข้อผิดพลาดที่เก็บได้สูงสุด

// ---- ค้นหาอุปกรณ์ Bluetooth (หน้า SCAN: แตะแถว Adapter ในหน้า CONFIG) ----
constexpr uint8_t SCAN_INQ_UNITS = 8;           // ระยะเวลาค้นหา หน่วยละ 1.28 วินาที (8 = ~10 วินาที, ช่วง 1-48)
constexpr uint32_t SCAN_NAME_TIMEOUT_MS = 5000; // อุปกรณ์ที่ไม่บอกชื่อตอนค้นหา: รอถามชื่อทีละตัวกี่ ms
constexpr uint32_t SCAN_NAMES_TOTAL_MS = 12000; // เวลารวมสูงสุดของขั้นถามชื่อ
constexpr uint8_t MAX_SCAN_DEVICES = 15;        // จำนวนอุปกรณ์ที่แสดงได้สูงสุด

// ---- มาตรวัดรอบ ----
constexpr float RPM_MAX = 8000.0f;          // ปลายสเกลมาตรวัดรอบ
constexpr float RPM_RED = 6500.0f;          // เริ่มโซนแดง (redline): วงโค้งและตัวเลข RPM เปลี่ยนเป็นสีแดง

// ---- เกณฑ์เตือน (หน้า DASH และหน้า ALERT) ----
constexpr int COOLANT_LIMIT_C = 105;        // ค่าเริ่มต้นของเกณฑ์น้ำร้อน (ปรับได้ในหน้า SET): >= เกณฑ์ = แดง + หน้า ALERT, ต่ำกว่า 8 องศา = เหลือง
constexpr int COOLANT_LIMIT_MIN_C = 95;     // ช่วงที่ปรับได้ในหน้า SET
constexpr int COOLANT_LIMIT_MAX_C = 115;
constexpr int COOLANT_ALERT_HYST_C = 3;     // ต้องลดต่ำกว่า (เกณฑ์ - ค่านี้) ก่อน ALERT จะเตือนได้อีกครั้ง
inline constexpr char FW_VERSION[] = "v2.1.0";  // แสดงในหน้า SET และหน้าเว็บ

// ---- Shift light (แถบไฟบนหน้า DRIVE) ----
constexpr int SHIFT_RPM_DEFAULT = 6000;     // รอบที่ไฟเต็มแถบและกะพริบ (ปรับได้ในหน้า SET)
constexpr int SHIFT_RPM_MIN = 3000;
constexpr int SHIFT_RPM_MAX = 7500;
constexpr int SHIFT_RPM_STEP = 250;
constexpr int SHIFT_RANGE_RPM = 1500;       // ไฟเริ่มติดที่ (shift - ค่านี้)

// ---- แถบเตือน (toast) ----
constexpr float TOAST_BATTERY_LOW_V = 12.2f;  // แบตเตอรี่ลดต่ำกว่านี้ = เตือน BATTERY LOW
constexpr float TOAST_FUEL_LOW_PCT = 10.0f;   // น้ำมันลดต่ำกว่านี้ = เตือน FUEL LOW
constexpr uint32_t TOAST_SHOW_MS = 4000;      // แสดงกี่ ms

// ---- การปัดเปลี่ยนหน้า / ลากเลื่อน ----
constexpr int SWIPE_MIN_PX = 50;              // ระยะแนวนอนขั้นต่ำ
constexpr uint32_t SWIPE_MAX_MS = 700;        // ต้องปัดให้จบภายในเวลานี้
constexpr int DRAG_START_PX = 8;              // หน้า SET: ลากแนวตั้งเกินนี้ = เลื่อนรายการ (ไม่ใช่แตะ)

// ---- จอสัมผัส XPT2046 (อ่านค่าผ่าน SPI) ----
constexpr uint32_t TOUCH_POLL_MS = 10;       // อ่านจอสัมผัสทุกกี่ ms
constexpr int TOUCH_Z_MIN = 400;             // แรงกดขั้นต่ำที่ถือว่า "แตะ"
constexpr uint8_t TOUCH_PRESS_SAMPLES = 2;   // ต้องเห็นการแตะติดกันกี่ครั้งถึงนับว่า "กดลง"
constexpr uint8_t TOUCH_RELEASE_SAMPLES = 3; // ต้องไม่เห็นการแตะติดกันกี่ครั้งถึงนับว่า "ปล่อย"
constexpr float TOUCH_SMOOTH_ALPHA = 0.5f;   // ตัวกรองพิกัด (0.0-1.0 ยิ่งน้อยยิ่งนิ่ง)

// ---- การ calibrate (แตะเป้า 4 มุม) ----
constexpr int CAL_MARGIN = 20;
constexpr uint8_t CAL_SAMPLES = 10;
constexpr int CAL_MIN_SPAN = 300;
constexpr int CAL_MAX_ERROR_PX = 25;

// ---- ปุ่ม ----
constexpr uint32_t LONG_PRESS_MS = 2000;     // ปุ่มสำคัญ (CLEAR, RECAL) ต้องกดค้างกี่ ms ("HOLD 2s" บนปุ่ม)

// ---- ความสว่างจอ (PWM ที่ GPIO21) ----
constexpr uint32_t PWM_FREQUENCY_HZ = 5000;
constexpr uint8_t PWM_RESOLUTION_BITS = 8;
constexpr uint32_t PWM_MAX_VALUE = (1UL << PWM_RESOLUTION_BITS) - 1;
constexpr uint8_t BACKLIGHT_CHANNEL = 2;
constexpr int BACKLIGHT_MIN_PERCENT = 10;
constexpr int BACKLIGHT_DEFAULT_PERCENT = 100;
constexpr int BACKLIGHT_DARK_DEFAULT = 70;    // ความสว่างแนะนำของธีม DARK (ผู้ใช้ปรับแล้วจำแยกต่อธีม)
constexpr int BACKLIGHT_LIGHT_DEFAULT = 95;   // ความสว่างแนะนำของธีม LIGHT

// ---- เสียง (ลำโพงที่ขั้ว SPEAK, GPIO26 ผ่านวงจรขยายบนบอร์ด) ----
// ใช้ PWM คลื่นสี่เหลี่ยม: ความดังปรับด้วย duty (ยิ่ง duty น้อยยิ่งเบา) ความละเอียด 10 บิต (0-1023)
constexpr uint8_t AUDIO_CHANNEL = 4;          // LEDC ช่อง 4 (timer 2) ห้ามใช้ช่อง 2-3 เพราะ timer เดียวกับไฟหลังจอ
constexpr uint8_t AUDIO_RESOLUTION_BITS = 10;
constexpr uint16_t VOLUME_DUTY[4] = {0, 24, 96, 512};  // OFF / LOW / MID / HIGH (512 = 50% = ดังสุด)
constexpr uint8_t VOLUME_DEFAULT = 2;         // MID
constexpr uint32_t ALERT_SOUND_EVERY_MS = 1400;  // หน้า ALERT: เล่นเสียงเตือนซ้ำทุกกี่ ms จนกว่าจะแตะรับทราบ
constexpr float RPM_BEEP_HYST = 300.0f;       // เสียงเตือน shift light ดังอีกครั้งเมื่อรอบลดต่ำกว่า (shift - ค่านี้) ก่อน

// ---- ธีม AUTO (เลือก DARK / LIGHT เองตอนเปิดเครื่องและทุก 1 นาที) ----
constexpr uint32_t AUTO_THEME_CHECK_MS = 60000;
// true = ใช้เซนเซอร์แสง (LDR ที่ GPIO34) / false = ใช้เวลา 06:00-18:00 = LIGHT (ต้องต่อ WiFi ที่มีอินเทอร์เน็ตเพื่อได้เวลา NTP)
constexpr bool AUTO_THEME_USE_LDR = true;
// ค่าดิบ analogRead ของ LDR บน CYD: ยิ่งสว่างค่ายิ่งต่ำ ยังไม่ได้วัดจากบอร์ดจริง ใช้คำสั่ง Serial "l" ดูค่าแล้วปรับ
// ระหว่างสองค่านี้คือช่วง hysteresis (คงธีมเดิม)
constexpr int LDR_LIGHT_AT_OR_BELOW = 100;
constexpr int LDR_DARK_AT_OR_ABOVE = 400;
constexpr long GMT_OFFSET_SEC = 7 * 3600;           // เวลาประเทศไทย UTC+7
inline constexpr char NTP_SERVER[] = "pool.ntp.org";

// ---- จอและจับเวลา ----
constexpr uint32_t DISPLAY_REFRESH_MS = 200; // อัปเดตตัวเลขบนจอ 5 ครั้ง/วินาที (วาดเมื่อค่าเปลี่ยนเท่านั้น)
constexpr uint32_t STATUS_PRINT_MS = 5000;   // สรุปสถานะออก Serial ทุกกี่ ms

// ---- เครือข่าย / เว็บ: ลองต่อ WiFi (secrets.h) ก่อน ถ้าไม่ได้จะเปิด AP ของตัวบอร์ดเอง ----
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;  // รอต่อ WiFi กี่ ms ก่อนเปลี่ยนไปเปิด AP
constexpr uint32_t WIFI_LOST_TO_AP_MS = 30000;       // ต่อ WiFi ได้แล้วแต่หลุดนานเกินนี้ = เปิด AP สำรอง
constexpr uint32_t WIFI_RETRY_MS = 60000;            // อยู่โหมด AP และไม่มีใครต่ออยู่: ลองต่อ WiFi ใหม่ทุกกี่ ms
inline constexpr char AP_SSID[] = "CYD-OBD";        // ชื่อ WiFi ที่ตัวบอร์ดปล่อยเอง
inline constexpr char AP_PASSWORD[] = "obd12345";   // ค่าเริ่มต้นเท่านั้น: ตั้งรหัสจริงใน secrets.h (#define CYD_AP_PASSWORD)
constexpr uint8_t AP_CHANNEL = 6;
inline constexpr char MDNS_NAME[] = "cyd-obd";      // เข้าได้ที่ http://cyd-obd.local
constexpr uint16_t HTTP_PORT = 80;

}  // namespace app
