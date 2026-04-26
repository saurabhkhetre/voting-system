"""
Production-level fix for voting_system.ino based on the working reference code.

Changes:
1. Remove Preferences include and prefs global
2. Fix FP pins: RX_PIN=17, TX_PIN=16 (matching working code: begin(57600, SERIAL_8N1, 17, 16))
3. Fix initFingerprint: use exact working pattern, single baud, correct pins
4. Fix setup(): remove all Preferences complexity, use proven cal values {379,3363,226,3449,4}
5. Remove forced recal line
"""

import re

path = r'firmware/voting_system/voting_system.ino'
content = open(path, encoding='utf-8', errors='replace').read()
c = content.replace('\r\n', '\n').replace('\r', '\n')

print(f"File loaded: {len(c)} chars, {c.count(chr(10))} lines")

changes = 0

# ─────────────────────────────────────────────────────────────────
# Fix 1: Remove Preferences include
# ─────────────────────────────────────────────────────────────────
OLD = '#include <Preferences.h>   // For persistent touch calibration\n\nPreferences prefs;         // Used to store calibration\n'
NEW = ''
if OLD in c:
    c = c.replace(OLD, NEW, 1)
    print("Fix 1 applied: Removed Preferences include and global")
    changes += 1
else:
    print("Fix 1 SKIP: Preferences block not found (may already be removed)")

# ─────────────────────────────────────────────────────────────────
# Fix 2: FP pin defines — match working code (RX=17, TX=16)
# ─────────────────────────────────────────────────────────────────
OLD = ('// Fingerprint sensor on Serial2\n'
       '// ESP32 Serial2 hardware pins: RX2=GPIO16, TX2=GPIO17\n'
       '// Sensor TX \u2192 GPIO16 (ESP32 RX),  Sensor RX \u2192 GPIO17 (ESP32 TX)\n'
       '#define FP_RX_PIN 17  // ESP32 RX2 \u2190 Sensor TX\n'
       '#define FP_TX_PIN 16   // ESP32 TX2 \u2192 Sensor RX\n')
NEW = ('// Fingerprint sensor on Serial2\n'
       '// Wiring (verified from working reference code):\n'
       '//   Sensor TX  \u2192  GPIO17  (ESP32 RX2)\n'
       '//   Sensor RX  \u2192  GPIO16  (ESP32 TX2)\n'
       '#define FP_RX_PIN 17  // ESP32 RX2 receives from sensor TX\n'
       '#define FP_TX_PIN 16  // ESP32 TX2 sends to sensor RX\n')
if OLD in c:
    c = c.replace(OLD, NEW, 1)
    print("Fix 2 applied: FP pins corrected to RX=17, TX=16")
    changes += 1
else:
    # Try to find and show what's there
    idx = c.find('FP_RX_PIN')
    if idx >= 0:
        print("Fix 2 partial: Found FP_RX_PIN at char", idx)
        print(repr(c[max(0,idx-100):idx+200]))
    else:
        print("Fix 2 SKIP: FP_RX_PIN not found")

# ─────────────────────────────────────────────────────────────────
# Fix 3: initFingerprint — use proven single-baud pattern from working code
# ─────────────────────────────────────────────────────────────────
# Find the whole initFingerprint function and replace it
fp_start = c.find('bool initFingerprint()')
if fp_start == -1:
    fp_start = c.find('void initFingerprint()')
    
if fp_start >= 0:
    # Find end by brace counting
    depth = 0
    fp_end = fp_start
    for i, ch in enumerate(c[fp_start:], fp_start):
        if ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                fp_end = i
                break
    
    NEW_FP_FUNC = '''bool initFingerprint() {
  // Exact pin order from verified working reference: (baud, config, RX_pin=17, TX_pin=16)
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(200);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor OK (57600 baud)");
    Serial.print("Capacity: "); Serial.println(finger.capacity);
    return true;
  }

  // Fallback: try 9600 baud (some clone sensors)
  fpSerial.end();
  delay(100);
  fpSerial.begin(9600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(9600);
  delay(200);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor OK (9600 baud)");
    return true;
  }

  Serial.println("Fingerprint sensor NOT found!");
  Serial.println("  Check wiring: Sensor TX -> GPIO17, Sensor RX -> GPIO16");
  return false;
}'''
    
    c = c[:fp_start] + NEW_FP_FUNC + c[fp_end+1:]
    print("Fix 3 applied: initFingerprint() replaced with clean working version")
    changes += 1
else:
    print("Fix 3 SKIP: initFingerprint not found")

# ─────────────────────────────────────────────────────────────────
# Fix 4: setup() — remove ALL Preferences / calibration wizard
#         Use proven cal values from working code: {379,3363,226,3449,4}
# ─────────────────────────────────────────────────────────────────
setup_start = c.find('void setup() {')
if setup_start >= 0:
    depth = 0
    setup_end = setup_start
    for i, ch in enumerate(c[setup_start:], setup_start):
        if ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                setup_end = i
                break

    NEW_SETUP = r'''void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Blockchain Voting System (Fingerprint Auth) ===\n");

  if (SIMULATE_FINGERPRINT) {
    Serial.println("*** SIMULATION MODE — Fingerprint sensor bypassed ***");
  }

  // ── Display init ───────────────────────────────────────────────
  tft.init();
  tft.setRotation(2);   // Portrait 240x320

  // ── Touch calibration values (verified from working reference code) ──
  // { xMin, xMax, yMin, yMax, rotation_flag }
  // These are hardware-specific values for this display panel.
  // If touch feels off after replacing display hardware, run:
  //   File -> Examples -> TFT_eSPI -> Generic -> Touch_calibrate
  uint16_t calData[5] = { 379, 3363, 226, 3449, 4 };
  tft.setTouch(calData);
  Serial.println("Touch calibrated with proven values {379,3363,226,3449,4}");

  tft.fillScreen(TFT_BLACK);
  delay(100);

  // ── Connect WiFi + init fingerprint sensor ─────────────────────
  connectWiFi();

  // ── Admin fingerprint check ────────────────────────────────────
  if (!SIMULATE_FINGERPRINT) {
    adminExists = checkAdminExists();
    if (!adminExists) {
      Serial.println("No admin fingerprint found - starting admin enrollment...");
      enrollAdmin();
      return;
    }
    Serial.println("Admin fingerprint: OK");
  } else {
    adminExists = true;
    Serial.println("SIM MODE: Admin bypassed");
  }

  // ── Launch home screen ─────────────────────────────────────────
  currentScreen = SCREEN_HOME;
  drawHomeScreen();
}'''

    c = c[:setup_start] + NEW_SETUP + '\n' + c[setup_end+1:]
    print("Fix 4 applied: setup() replaced with clean production version")
    changes += 1
else:
    print("Fix 4 SKIP: void setup() not found")

# ─────────────────────────────────────────────────────────────────
# Fix 5: Fix Serial debug message in initFingerprint
#         (update wiring hint to correct pins)
# ─────────────────────────────────────────────────────────────────
OLD5 = 'Check wiring: Sensor TX -> GPIO16, Sensor RX -> GPIO17'
NEW5 = 'Check wiring: Sensor TX -> GPIO17, Sensor RX -> GPIO16'
if OLD5 in c:
    c = c.replace(OLD5, NEW5)
    print("Fix 5 applied: Updated wiring hint in error message")
    changes += 1

# ─────────────────────────────────────────────────────────────────
# Write result
# ─────────────────────────────────────────────────────────────────
open(path, 'w', encoding='utf-8', newline='\n').write(c)
print(f"\nAll done! {changes} fixes applied.")

o = c.count('{')
cl = c.count('}')
print(f"Brace balance: {o} open, {cl} close, diff={o-cl}")
assert o == cl, "BRACE MISMATCH!"
print("Brace check PASSED")
