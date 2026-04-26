"""
FINAL FIX: Touch + Overlapping UI
===================================
Problems:
1. getTouch() called with threshold=300 (too low, reads noise as touches)
   Working code uses NO threshold (default 600 = correct for XPT2046)
2. connectWiFi() draws complex UI that bleeds into home screen

Fixes:
1. Remove threshold from ALL getTouch() calls
2. Replace connectWiFi() with minimal version: black screen + 1 status line only
3. setup() shows simple splash, then home screen — like working reference code
"""

import re

path = r'firmware/voting_system/voting_system.ino'
content = open(path, encoding='utf-8', errors='replace').read()
c = content.replace('\r\n', '\n').replace('\r', '\n')

print(f"Loaded: {len(c.splitlines())} lines")

# ─────────────────────────────────────────────────────────────────
# Fix 1: Remove threshold=300 from ALL getTouch() calls
# Working code: tft.getTouch(&x, &y)  [default threshold = 600]
# ─────────────────────────────────────────────────────────────────
before = c.count('tft.getTouch(')
c = re.sub(r'tft\.getTouch\(&(\w+),\s*&(\w+),\s*300\)', r'tft.getTouch(&\1, &\2)', c)
after_re = c.count('tft.getTouch(')
remaining_300 = c.count(', 300)')
print(f"Fix 1: getTouch threshold=300 removed. Remaining ', 300)': {remaining_300}")

# ─────────────────────────────────────────────────────────────────
# Fix 2: Replace entire connectWiFi() with minimal version
# ─────────────────────────────────────────────────────────────────

# Find and replace connectWiFi() body
fw_start = c.find('void connectWiFi() {')
if fw_start == -1:
    fw_start = c.find('void connectWiFi(){')

if fw_start >= 0:
    depth = 0
    fw_end = fw_start
    for i, ch in enumerate(c[fw_start:], fw_start):
        if ch == '{': depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                fw_end = i
                break

    NEW_WIFI = r'''void connectWiFi() {
  // Minimal boot: black screen, single status line.
  // No titles or decorative elements that could bleed into home screen.
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(0x7BEF); tft.setTextSize(1);
  tft.setCursor(10, 10);
  tft.print("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(300);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: %s\n", WIFI_SSID);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Try once more
    WiFi.disconnect(true); delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
    tft.fillRect(0, 0, SCREEN_W, 20, TFT_BLACK);
    tft.setTextColor(0x07E0); tft.setTextSize(1);
    tft.setCursor(10, 10);
    tft.print("WiFi OK: ");
    tft.print(WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAILED. Continuing offline.");
    tft.fillRect(0, 0, SCREEN_W, 20, TFT_BLACK);
    tft.setTextColor(0xF800); tft.setTextSize(1);
    tft.setCursor(10, 10);
    tft.print("WiFi FAILED - offline mode");
  }

  // Init fingerprint sensor (Serial only, no display)
  tft.fillRect(0, 20, SCREEN_W, 10, TFT_BLACK);
  tft.setTextColor(0x7BEF); tft.setTextSize(1);
  tft.setCursor(10, 22);
  tft.print("Init fingerprint...");

  if (!SIMULATE_FINGERPRINT) {
    if (initFingerprint()) {
      fpSensorConnected = true;
      finger.getTemplateCount();
      Serial.printf("FP OK (%d templates)\n", finger.templateCount);
      tft.fillRect(0, 20, SCREEN_W, 10, TFT_BLACK);
      tft.setTextColor(0x07E0); tft.setCursor(10, 22);
      tft.print("FP sensor OK");
    } else {
      fpSensorConnected = false;
      Serial.println("FP sensor NOT found");
      tft.fillRect(0, 20, SCREEN_W, 10, TFT_BLACK);
      tft.setTextColor(0xFD20); tft.setCursor(10, 22);
      tft.print("FP: not found");
    }
  } else {
    fpSensorConnected = true;
    tft.fillRect(0, 20, SCREEN_W, 10, TFT_BLACK);
    tft.setTextColor(0xFD20); tft.setCursor(10, 22);
    tft.print("FP: SIM mode");
  }

  delay(800);

  // CLEAR SCREEN COMPLETELY before returning — mandatory
  tft.fillScreen(TFT_BLACK);
}'''

    c = c[:fw_start] + NEW_WIFI + '\n' + c[fw_end + 1:]
    print("Fix 2: connectWiFi() replaced with minimal version")
else:
    print("Fix 2 SKIP: connectWiFi() not found")

# ─────────────────────────────────────────────────────────────────
# Write
# ─────────────────────────────────────────────────────────────────
open(path, 'w', encoding='utf-8', newline='\n').write(c)
print("File written.")

o = c.count('{'); cl = c.count('}')
print(f"Brace balance: {o} open, {cl} close, diff={o-cl}")
assert o == cl, "BRACE MISMATCH!"
print("PASSED")
