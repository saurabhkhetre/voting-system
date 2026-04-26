/*
 * ═══════════════════════════════════════════════════════════════
 *   RAW XPT2046 TOUCH TEST — Direct SPI, No TFT_eSPI touch code
 * ═══════════════════════════════════════════════════════════════
 *
 *  This sketch uses Arduino's hardware SPI directly to talk to the
 *  XPT2046 touchscreen controller, completely bypassing TFT_eSPI.
 *
 *  INSTRUCTIONS:
 *  1. Upload this sketch
 *  2. Open Serial Monitor at 115200 baud
 *  3. Touch the screen — you should see "TOUCH DETECTED" lines
 *  4. If you see "RAW: x=0 y=0" always → TOUCH_CS wiring issue
 *     If you see "RAW: x=4095 y=4095" → MISO wiring issue
 *     If you see valid numbers (200-3900) → hardware is GOOD
 *
 *  XPT2046 SPI MODE: 0 (CPOL=0, CPHA=0)
 *  Your pins (from User_Setup):
 *    MOSI = 23, MISO = 19, CLK = 18, TOUCH_CS = 21
 * ═══════════════════════════════════════════════════════════════
 */

#include <TFT_eSPI.h>
#include <SPI.h>

// ── Pin definitions ─────────────────────────────────────────
#define TOUCH_CS_PIN 21
#define SPI_MOSI     23
#define SPI_MISO     19
#define SPI_CLK      18

// XPT2046 command bytes (12-bit differential mode)
#define CMD_X_POS  0xD0   // 1 101 0 000 = START, CH=X(101), 12bit, DFR, PD=00
#define CMD_Y_POS  0x90   // 1 001 0 000 = START, CH=Y(001), 12bit, DFR, PD=00
#define CMD_Z1_POS 0xB0   // Pressure Z1
#define CMD_Z2_POS 0xC0   // Pressure Z2

TFT_eSPI tft = TFT_eSPI();

// ── Read XPT2046 using direct SPI transaction ───────────────
// Returns 12-bit ADC value (0-4095)
uint16_t xpt_read(uint8_t command) {
  uint8_t buf[3] = {command, 0x00, 0x00};
  
  digitalWrite(TOUCH_CS_PIN, LOW);
  delayMicroseconds(5);
  
  // Use hardware SPI — must match the frequency
  SPI.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
  SPI.transfer(command);          // Send command
  uint16_t high = SPI.transfer(0x00);  // Read high byte
  uint16_t low  = SPI.transfer(0x00);  // Read low byte
  SPI.endTransaction();
  
  digitalWrite(TOUCH_CS_PIN, HIGH);
  delayMicroseconds(5);
  
  // Result is in bits 14:3 of the 16-bit response (shift right 3)
  return ((high << 8) | low) >> 3;
}

// Read multiple samples and return median (noise reduction)
uint16_t xpt_read_avg(uint8_t command, int samples = 5) {
  uint16_t vals[10];
  int n = min(samples, 10);
  for (int i = 0; i < n; i++) {
    vals[i] = xpt_read(command);
    delayMicroseconds(200);
  }
  // Simple sort
  for (int i = 0; i < n-1; i++)
    for (int j = 0; j < n-1-i; j++)
      if (vals[j] > vals[j+1]) { uint16_t t = vals[j]; vals[j] = vals[j+1]; vals[j+1] = t; }
  return vals[n/2]; // median
}

// Check if screen is being touched (Z pressure reading)
bool isTouched() {
  SPI.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS_PIN, LOW);
  delayMicroseconds(5);
  SPI.transfer(CMD_Z1_POS);
  uint16_t z1h = SPI.transfer(0); uint16_t z1l = SPI.transfer(0);
  uint16_t z1 = ((z1h << 8) | z1l) >> 3;
  SPI.transfer(CMD_Z2_POS);
  uint16_t z2h = SPI.transfer(0); uint16_t z2l = SPI.transfer(0);
  uint16_t z2 = ((z2h << 8) | z2l) >> 3;
  digitalWrite(TOUCH_CS_PIN, HIGH);
  SPI.endTransaction();
  
  // Z1 should be > 0 and Z2 should be < 4095 when touched
  // Pressure value = (X/4096) * (Z2/Z1 - 1)  (simplified check)
  return (z1 > 100 && z2 < 3900);
}

int touchCount = 0;

void drawInfo(uint16_t rawX, uint16_t rawY, uint16_t calX, uint16_t calY) {
  tft.fillRect(0, 80, 240, 80, TFT_BLACK);
  tft.setTextSize(1);
  
  tft.setTextColor(TFT_CYAN);
  tft.setCursor(5, 84); tft.print("RAW X: "); tft.print(rawX);
  tft.setCursor(5, 98); tft.print("RAW Y: "); tft.print(rawY);
  
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(5, 114); tft.print("CAL X: "); tft.print(calX);
  tft.setCursor(5, 128); tft.print("CAL Y: "); tft.print(calY);
  
  tft.setTextColor(TFT_YELLOW);
  tft.setCursor(5, 144); tft.print("Touches: "); tft.print(touchCount);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("\n\n=== RAW XPT2046 TOUCH TEST ===");
  Serial.println("Touch_CS=21, MOSI=23, MISO=19, CLK=18");
  Serial.println("Touching screen should show values 200-3900");
  Serial.println("x=0 or y=0 always  -> CS pin issue");
  Serial.println("x=4095 always       -> MISO pin issue");
  Serial.println("==========================================\n");
  
  // Configure TOUCH_CS as output, HIGH = deselected
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);
  
  // Init display (draws the UI, does NOT initialise touch)
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  delay(100);
  
  // Apply some calibration for the CAL readout
  uint16_t cal[5] = {339, 3498, 275, 3593, 2};
  tft.setTouch(cal);
  
  // Draw header
  tft.fillRect(0, 0, 240, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN); tft.setTextSize(2);
  tft.setCursor(8, 10); tft.print("TOUCH TEST");
  
  tft.setTextColor(TFT_WHITE); tft.setTextSize(1);
  tft.setCursor(8, 36); tft.print("TOUCH_CS = GPIO 21");
  tft.setCursor(8, 50); tft.print("Touch screen to test...");
  tft.setCursor(8, 64); tft.print("Watch Serial Monitor too.");
  
  tft.drawFastHLine(0, 76, 240, TFT_DARKGREY);
  
  // Corner markers so you can see the full screen area
  tft.drawRect(2,   2,   20, 20, TFT_YELLOW);
  tft.drawRect(218, 2,   20, 20, TFT_YELLOW);
  tft.drawRect(2,   298, 20, 20, TFT_YELLOW);
  tft.drawRect(218, 298, 20, 20, TFT_YELLOW);
  
  tft.setCursor(5,   168); tft.setTextColor(TFT_DARKGREY);
  tft.print("Touch here");
  tft.setCursor(5,   270); tft.print("Touch here");
  tft.setCursor(140, 168); tft.print("Touch here");
  tft.setCursor(140, 270); tft.print("Touch here");
}

uint32_t lastPrint = 0;
uint16_t lastDotX = 0, lastDotY = 0;

void loop() {
  // ── Method 1: Direct SPI read from XPT2046 ───────────────
  bool touched1 = isTouched();
  uint16_t rawX = 0, rawY = 0;
  if (touched1) {
    rawX = xpt_read_avg(CMD_X_POS, 5);
    rawY = xpt_read_avg(CMD_Y_POS, 5);
  }
  
  // ── Method 2: TFT_eSPI getTouch (low threshold=200) ──────
  uint16_t calX = 0, calY = 0;
  bool touched2 = tft.getTouch(&calX, &calY, 200);
  
  // ── Output ───────────────────────────────────────────────
  bool anyTouch = touched1 || touched2;
  
  if (anyTouch) {
    touchCount++;
    
    // Print to Serial
    Serial.printf("[%d] RAW(%4d,%4d) CAL(%3d,%3d) | spi=%s tft=%s\n",
      touchCount, rawX, rawY, calX, calY,
      touched1 ? "YES" : "no",
      touched2 ? "YES" : "no");
    
    // Update display
    drawInfo(rawX, rawY, calX, calY);
    
    // Draw dot at calibrated position
    if (calX > 0 && calX < 240 && calY > 200 && calY < 320) {
      if (lastDotX > 0) tft.fillCircle(lastDotX, lastDotY, 6, TFT_BLACK);
      lastDotX = calX; lastDotY = calY;
      tft.fillCircle(calX, calY, 6, TFT_RED);
      tft.drawCircle(calX, calY, 7, TFT_ORANGE);
    } else if (rawX > 200 && rawX < 3900) {
      // Map raw to screen (rough)
      int sx = map(rawX, 339, 3498, 0, 240);
      int sy = map(rawY, 275, 3593, 0, 320);
      if (sx > 0 && sx < 240 && sy > 160 && sy < 320) {
        if (lastDotX > 0) tft.fillCircle(lastDotX, lastDotY, 6, TFT_BLACK);
        lastDotX = sx; lastDotY = sy;
        tft.fillCircle(sx, sy, 6, TFT_PURPLE);
        tft.drawCircle(sx, sy, 7, TFT_PINK);
      }
    }
    
    lastPrint = millis();
  } else {
    if (millis() - lastPrint > 2000) {
      uint16_t idleX = xpt_read(CMD_X_POS);
      uint16_t idleY = xpt_read(CMD_Y_POS);
      Serial.printf("No touch | idle raw(%4d,%4d)\n", idleX, idleY);
      lastPrint = millis();
    }
  }
  
  delay(16);
}
