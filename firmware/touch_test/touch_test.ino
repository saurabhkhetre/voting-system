#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();

int currentRotation = 0;
unsigned long lastSwitch = 0;
bool autoRotate = true;
int touchCount = 0;

void showRotationScreen(int rot) {
  tft.setRotation(rot);
  tft.fillScreen(TFT_BLACK);
  
  int w = tft.width();
  int h = tft.height();
  
  // Draw border to show screen bounds
  tft.drawRect(0, 0, w, h, TFT_GREEN);
  tft.drawRect(1, 1, w-2, h-2, TFT_GREEN);
  
  // Show rotation info
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.printf("ROT %d", rot);
  
  tft.setTextSize(2);
  tft.setCursor(10, 45);
  tft.printf("%dx%d", w, h);
  
  // Show orientation type
  tft.setTextSize(2);
  tft.setCursor(10, 75);
  if (w > h) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("LANDSCAPE");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("PORTRAIT");
  }
  
  // Instructions
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 105);
  tft.print("TOUCH SCREEN TO TEST!");
  tft.setCursor(10, 120);
  tft.print("Dots will appear where");
  tft.setCursor(10, 135);
  tft.print("you touch the screen.");
  
  tft.setCursor(10, 160);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.print("Auto-rotate in 8 sec...");
  tft.setCursor(10, 175);
  tft.print("Touch stops auto-rotate");
  
  // Touch counter
  tft.setCursor(10, 200);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.printf("Touches detected: %d", touchCount);
  
  // Calibration
  uint16_t calData[5] = {300, 3600, 300, 3600, 7};
  tft.setTouch(calData);
  
  Serial.printf("\n--- Rotation %d: %dx%d (%s) ---\n", 
    rot, w, h, w > h ? "LANDSCAPE" : "PORTRAIT");
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== TFT Touch & Rotation Test ===\n");
  Serial.println("This sketch tests all 4 rotations and touch input.");
  Serial.println("Touch the screen - you should see dots and Serial output.\n");
  
  tft.init();
  
  // Start with rotation 0
  showRotationScreen(0);
  lastSwitch = millis();
}

void loop() {
  // Auto-rotate every 8 seconds (unless touch was detected)
  if (autoRotate && millis() - lastSwitch > 8000) {
    currentRotation = (currentRotation + 1) % 4;
    showRotationScreen(currentRotation);
    lastSwitch = millis();
  }
  
  // Check for touch
  uint16_t tx = 0, ty = 0;
  
  if (tft.getTouch(&tx, &ty)) {
    touchCount++;
    
    // Stop auto-rotation on first touch
    if (autoRotate) {
      autoRotate = false;
      Serial.println("*** TOUCH DETECTED! Auto-rotate stopped ***");
    }
    
    // Draw a dot where touched
    tft.fillCircle(tx, ty, 4, TFT_RED);
    tft.drawCircle(tx, ty, 6, TFT_YELLOW);
    
    // Print to serial
    Serial.printf("Touch #%d: x=%d, y=%d (rotation=%d)\n", 
      touchCount, tx, ty, currentRotation);
    
    // Update touch count on screen
    tft.fillRect(0, 195, tft.width(), 20, TFT_BLACK);
    tft.setCursor(10, 200);
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setTextSize(1);
    tft.printf("Touches: %d | Last: %d,%d", touchCount, tx, ty);
    
    delay(50);  // Small debounce
  }
  
  // Also try raw SPI read of touch (bypassing library)
  static unsigned long lastRawCheck = 0;
  if (millis() - lastRawCheck > 1000) {
    lastRawCheck = millis();
    
    // Quick check: manually read XPT2046 via SPI
    digitalWrite(21, LOW);  // TOUCH_CS LOW (select)
    uint8_t cmd = 0xD0;     // Read X position command
    SPI.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
    SPI.transfer(cmd);
    uint8_t hi = SPI.transfer(0);
    uint8_t lo = SPI.transfer(0);
    SPI.endTransaction();
    digitalWrite(21, HIGH); // TOUCH_CS HIGH (deselect)
    
    uint16_t rawX = ((hi << 8) | lo) >> 3;
    
    // If rawX is not 0 and not max, something is being pressed
    if (rawX > 100 && rawX < 4000) {
      Serial.printf("RAW SPI TOUCH detected! rawX=%d\n", rawX);
    }
  }
}
