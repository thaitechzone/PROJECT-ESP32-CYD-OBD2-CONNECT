#pragma once

// =====================================================================
//  board_config.h -- ข้อเท็จจริงของฮาร์ดแวร์บอร์ด ESP32-2432S028 (CYD)
//
//  ไฟล์นี้รวม "ทุก pin" ไว้ที่เดียว ห้ามแก้ เว้นแต่ฮาร์ดแวร์จริงต่างไป
//  (ค่าที่ปรับได้ของโปรแกรม ให้ไปแก้ใน app_config.h แทน)
// =====================================================================

#include <Arduino.h>

namespace cyd {

// ---- จอ TFT (SPI ชุดที่ 1) ----
constexpr uint8_t TFT_PIN_MISO = 12;
constexpr uint8_t TFT_PIN_MOSI = 13;
constexpr uint8_t TFT_PIN_SCLK = 14;
constexpr uint8_t TFT_PIN_CS = 15;
constexpr uint8_t TFT_PIN_DC = 2;
constexpr uint8_t TFT_PIN_BACKLIGHT = 21;  // ห้ามใช้ต่อเซนเซอร์ (ดูหมายเหตุด้านล่าง)

// ---- จอสัมผัส XPT2046 (SPI คนละชุดกับจอ) ----
constexpr uint8_t TOUCH_PIN_CLK = 25;
constexpr uint8_t TOUCH_PIN_CS = 33;
constexpr uint8_t TOUCH_PIN_MOSI = 32;
constexpr uint8_t TOUCH_PIN_MISO = 39;
constexpr uint8_t TOUCH_PIN_IRQ = 36;

// ---- RGB LED บนบอร์ด (สามสีในดวงเดียว) ----
//
//   สี          ขา GPIO     ประเภทสัญญาณ
//   แดง   (R)   GPIO4       เปิด/ปิดด้วย digitalWrite หรือ PWM (LEDC) ก็ได้
//   เขียว (G)   GPIO16      เปิด/ปิดด้วย digitalWrite หรือ PWM (LEDC) ก็ได้
//   น้ำเงิน (B) GPIO17      เปิด/ปิดด้วย digitalWrite หรือ PWM (LEDC) ก็ได้
//
// หมายเหตุ: ตรวจจากบอร์ดจริงแล้วว่า GPIO4 ขับ LED สีแดง (ไม่ใช่สีเขียวตามเอกสาร pinout เดิม)
//           ตรงกับ pinout ของ CYD ที่ใช้กันทั่วไป: แดง=4, เขียว=16, น้ำเงิน=17
//
// ทั้งสามสีเป็นแบบ active-LOW: ส่ง LOW = LED ติด, ส่ง HIGH = LED ดับ
//   -> ถ้าใช้ PWM ต้อง "กลับค่า duty" ก่อนเขียน คือเขียน (ค่าสูงสุด - ค่าที่อยากให้สว่าง)
constexpr uint8_t LED_PIN_R = 4;   // LED แดง
constexpr uint8_t LED_PIN_G = 16;  // LED เขียว
constexpr uint8_t LED_PIN_B = 17;  // LED น้ำเงิน
// ถ้าบอร์ดของคุณเป็นกลับกัน (สีที่เห็นตรงข้ามกับที่ตั้งใจ) ให้เปลี่ยนเป็น false
constexpr bool LED_ACTIVE_LOW = true;

// ---- อื่นๆ บนบอร์ด ----
constexpr uint8_t AUDIO_PIN = 26;  // ลำ โพง (DAC)
constexpr uint8_t BOOT_BUTTON_PIN = 0;  // ปุ่ม BOOT (กดแล้วเป็น LOW) ใช้เป็นปุ่มทั่วไปได้หลังบูตเสร็จ
constexpr uint8_t LDR_PIN = 34;    // ตัววัดแสง input-only, ADC1_CH6 ใช้ analogRead() ได้เลย

// ---- microSD (SPI ชุดที่ 3) ----
constexpr uint8_t SD_PIN_CS = 5;
constexpr uint8_t SD_PIN_MOSI = 23;
constexpr uint8_t SD_PIN_CLK = 18;
constexpr uint8_t SD_PIN_MISO = 19;

// ---- ขา expansion ที่ปลอดภัยสำหรับต่อเซนเซอร์เพิ่ม ----
// ทดสอบกับฮาร์ดแวร์จริงแล้ว: GPIO27 (SDA) + GPIO22 (SCL) ใช้ I2C ได้ปกติ
// และ GPIO27 ใช้ต่อเซนเซอร์ตัวเดียว (เช่น DHT11) ได้โดยไม่รบกวนจอ
constexpr uint8_t I2C_PIN_SDA = 27;
constexpr uint8_t I2C_PIN_SCL = 22;
constexpr uint8_t SAFE_SENSOR_PIN = 27;

// !! ห้ามใช้ GPIO21 ต่อเซนเซอร์เด็ดขาด เพราะเป็นขา backlight ของจอ
// ทดสอบแล้วว่าทำให้จอกระพริบตามการสื่อสาร ทั้ง I2C และเซนเซอร์ 1-wire
// !! GPIO34/35/36/39 เป็น input-only: อ่านค่าได้อย่างเดียว สั่ง output ไม่ได้

// ---- จอ ----
constexpr uint16_t DISPLAY_WIDTH = 320;
constexpr uint16_t DISPLAY_HEIGHT = 240;
constexpr uint8_t DISPLAY_ROTATION = 1;  // 1 = แนวนอน (หมุน 180° = 3 ตั้งได้ในหน้า SET → Flip 180°)

// ---- Touch calibration ----
// ค่าเหล่านี้ได้จากโปรแกรม calibration ของ "บอร์ดแต่ละตัว" (ใช้ร่วมกันข้ามบอร์ดไม่ได้)
// ค่าเริ่มต้นด้านล่างเป็นค่าประมาณ และ TOUCH_CALIBRATED = false จนกว่าจะวัดจริง
constexpr bool TOUCH_CALIBRATED = false;
constexpr uint16_t TOUCH_RAW_X_MIN = 480;
constexpr uint16_t TOUCH_RAW_X_MAX = 3600;
constexpr uint16_t TOUCH_RAW_Y_MIN = 600;
constexpr uint16_t TOUCH_RAW_Y_MAX = 3530;
constexpr bool TOUCH_SWAP_XY = false;
constexpr bool TOUCH_INVERT_X = false;
constexpr bool TOUCH_INVERT_Y = false;
constexpr uint8_t TOUCH_SPI_BUS = VSPI;
constexpr uint32_t TOUCH_SPI_FREQUENCY = 2500000;

// ---------------------------------------------------------------------
// ฟังก์ชันช่วย (inline = วางในไฟล์ header ได้เลย)
// ---------------------------------------------------------------------

// ตั้งค่า pin ของ RGB LED เป็น output และดับไฟทั้งหมดตั้งแต่เริ่ม
inline void ledInit()
{
    pinMode(LED_PIN_R, OUTPUT);
    pinMode(LED_PIN_G, OUTPUT);
    pinMode(LED_PIN_B, OUTPUT);
    digitalWrite(LED_PIN_R, LED_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(LED_PIN_G, LED_ACTIVE_LOW ? HIGH : LOW);
    digitalWrite(LED_PIN_B, LED_ACTIVE_LOW ? HIGH : LOW);
}

// สั่งสี RGB LED: true = ติด, false = ดับ (ไม่ต้องสนใจว่าบอร์ดเป็น active-LOW หรือ HIGH)
inline void ledSet(bool red, bool green, bool blue)
{
    digitalWrite(LED_PIN_R, (red == LED_ACTIVE_LOW) ? LOW : HIGH);
    digitalWrite(LED_PIN_G, (green == LED_ACTIVE_LOW) ? LOW : HIGH);
    digitalWrite(LED_PIN_B, (blue == LED_ACTIVE_LOW) ? LOW : HIGH);
}

}  // namespace cyd
