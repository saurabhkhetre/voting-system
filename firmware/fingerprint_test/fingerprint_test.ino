/*
 * Fingerprint Sensor Diagnostic Test
 * ====================================
 * Upload this to ESP32 and open Serial Monitor at 115200 baud.
 * It will test communication with the R307/AS608 sensor.
 *
 * Wiring:
 *   Sensor TX  → ESP32 GPIO 16 (RX2)
 *   Sensor RX  → ESP32 GPIO 17 (TX2)
 *   VCC → 3.3V (or 5V if your sensor needs it)
 *   GND → GND
 */

#include <Adafruit_Fingerprint.h>

// Fingerprint sensor on Serial2
#define FP_RX 17
#define FP_TX 16

HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("  Fingerprint Sensor Diagnostic Test");
  Serial.println("========================================\n");
  
  // Test 1: Initialize Serial2
  Serial.println("[TEST 1] Initializing Serial2 (GPIO16=RX, GPIO17=TX)...");
  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(57600);
  Serial.println("  Serial2 started at 57600 baud ✓\n");
  
  // Test 2: Verify sensor password (checks if sensor responds)
  Serial.println("[TEST 2] Sending verify password command...");
  if (finger.verifyPassword()) {
    Serial.println("  ✅ Fingerprint sensor FOUND and responding!");
    Serial.println("  Communication is working.\n");
  } else {
    Serial.println("  ❌ Fingerprint sensor NOT FOUND!");
    Serial.println("  Possible issues:");
    Serial.println("    1. Check wiring: TX→GPIO16, RX→GPIO17");
    Serial.println("    2. Check power: VCC→3.3V (or 5V), GND→GND");
    Serial.println("    3. Try swapping TX and RX wires");
    Serial.println("    4. Check if sensor LED blinks on power-up");
    Serial.println("    5. Try different baud rate (9600, 19200, 38400, 57600)");
    Serial.println("");
    
    // Try other baud rates
    Serial.println("[TEST 2b] Trying other baud rates...");
    int baudRates[] = {9600, 19200, 38400, 57600, 115200};
    for (int i = 0; i < 5; i++) {
      fpSerial.begin(baudRates[i], SERIAL_8N1, FP_RX, FP_TX);
      finger.begin(baudRates[i]);
      delay(500);
      if (finger.verifyPassword()) {
        Serial.printf("  ✅ Sensor found at %d baud!\n", baudRates[i]);
        Serial.println("  Update your main sketch to use this baud rate.\n");
        break;
      } else {
        Serial.printf("  ❌ No response at %d baud\n", baudRates[i]);
      }
    }
    Serial.println("\n  If all baud rates failed, the sensor may be:");
    Serial.println("    - Not powered (check VCC/GND)");
    Serial.println("    - Wired incorrectly (swap TX/RX)");
    Serial.println("    - Damaged or defective\n");
    return;
  }
  
  // Test 3: Read sensor parameters
  Serial.println("[TEST 3] Reading sensor parameters...");
  finger.getParameters();
  Serial.printf("  Sensor capacity: %d fingerprints\n", finger.capacity);
  Serial.printf("  Security level: %d\n", finger.security_level);
  Serial.printf("  Packet length: %d\n", finger.packet_len);
  Serial.printf("  Baud rate: %d\n", finger.baud_rate);
  Serial.println("");
  
  // Test 4: Check how many fingerprints are stored
  Serial.println("[TEST 4] Checking stored fingerprints...");
  finger.getTemplateCount();
  Serial.printf("  Templates stored: %d / %d\n\n", finger.templateCount, finger.capacity);
  
  // Test 5: Check specific slots
  Serial.println("[TEST 5] Checking key slots...");
  for (int id = 1; id <= 10; id++) {
    uint8_t p = finger.loadModel(id);
    if (p == FINGERPRINT_OK) {
      Serial.printf("  Slot %d: OCCUPIED ✓\n", id);
    } else {
      Serial.printf("  Slot %d: empty\n", id);
    }
  }
  // Check admin slot
  uint8_t p = finger.loadModel(127);
  if (p == FINGERPRINT_OK) {
    Serial.println("  Slot 127 (ADMIN): OCCUPIED ✓");
  } else {
    Serial.println("  Slot 127 (ADMIN): empty (no admin enrolled)");
  }
  
  Serial.println("\n========================================");
  Serial.println("  All tests complete!");
  Serial.println("  Now place finger on sensor to test scanning...");
  Serial.println("========================================\n");
}

void loop() {
  Serial.println("Waiting for finger...");
  
  uint8_t p = finger.getImage();
  
  if (p == FINGERPRINT_OK) {
    Serial.println("  ✅ Finger detected! Image captured.");
    
    p = finger.image2Tz();
    if (p == FINGERPRINT_OK) {
      Serial.println("  ✅ Image converted to template.");
      
      p = finger.fingerSearch();
      if (p == FINGERPRINT_OK) {
        Serial.printf("  ✅ MATCH FOUND! ID: %d, Confidence: %d\n\n", 
                       finger.fingerID, finger.confidence);
      } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("  ⚠️ Finger not recognized (not enrolled)\n");
      } else {
        Serial.printf("  ❌ Search error: 0x%02X\n\n", p);
      }
    } else {
      Serial.printf("  ❌ Image conversion failed: 0x%02X\n\n", p);
    }
    
    delay(2000);  // Wait before next scan
  } else if (p == FINGERPRINT_NOFINGER) {
    // No finger — normal, just wait
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("  ❌ Communication error! Check wiring.");
  } else {
    Serial.printf("  ❌ Unknown error: 0x%02X\n", p);
  }
  
  delay(200);
}
