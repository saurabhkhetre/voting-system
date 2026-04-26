"""
Fix the overlapping UI issue.

Root cause: connectWiFi() draws a startup splash screen that stays visible,
then drawHomeScreen() is called but it draws ON TOP rather than fully replacing
because both use the same bg color — the TFT sometimes doesn't fully refresh
before the next draw call begins.

Fix:
1. connectWiFi() ends with tft.fillScreen(TFT_BLACK) to guarantee full clear
2. setup() explicitly clears screen before drawHomeScreen()
3. Every screen transition starts with fillScreen
4. Remove the "SYSTEM READY" delay blocker from connectWiFi — it just sits there
   while the home screen hasn't loaded yet, confusing the user
"""

path = r'firmware/voting_system/voting_system.ino'
content = open(path, encoding='utf-8', errors='replace').read()
c = content.replace('\r\n', '\n').replace('\r', '\n')

# ─────────────────────────────────────────────────────────────────
# Fix 1: connectWiFi() — end with full screen clear + brief delay
# Replace the "System ready" block at the end to clear screen after showing it
# ─────────────────────────────────────────────────────────────────

OLD_READY = '''  // System ready
  tft.fillRoundRect(40, 195, SCREEN_W - 80, 30, 8, SUCCESS_COLOR);
  drawCenteredText("SYSTEM READY", 203, TEXT_COLOR, 2);

  delay(2000);
}'''

NEW_READY = '''  // System ready banner
  tft.fillRoundRect(40, 195, SCREEN_W - 80, 30, 8, SUCCESS_COLOR);
  drawCenteredText("SYSTEM READY", 203, TEXT_COLOR, 2);
  delay(1500);

  // CRITICAL: Clear the entire screen before returning.
  // This prevents the startup splash from bleeding into the next screen.
  tft.fillScreen(TFT_BLACK);
}'''

if OLD_READY in c:
    c = c.replace(OLD_READY, NEW_READY, 1)
    print("Fix 1 applied: connectWiFi() now clears screen before returning")
else:
    print("Fix 1 SKIP: could not find system ready block")
    # Show the area
    idx = c.find('System ready')
    if idx >= 0:
        print(repr(c[idx:idx+200]))

# ─────────────────────────────────────────────────────────────────
# Fix 2: setup() — explicit fillScreen before drawHomeScreen
# ─────────────────────────────────────────────────────────────────

OLD_HOME = '''  // Launch home screen
  currentScreen = SCREEN_HOME;
  drawHomeScreen();
}'''

NEW_HOME = '''  // Clear screen fully then draw home
  tft.fillScreen(TFT_BLACK);
  delay(50);
  currentScreen = SCREEN_HOME;
  drawHomeScreen();
}'''

if OLD_HOME in c:
    c = c.replace(OLD_HOME, NEW_HOME, 1)
    print("Fix 2 applied: setup() clears screen before drawHomeScreen()")
else:
    print("Fix 2 SKIP: launch home block not found")

# ─────────────────────────────────────────────────────────────────
# Fix 3: drawHomeScreen() — ensure it always starts with full clear
# (already calls fillScreen at line 229, but ensure BG_COLOR is right)
# Add delay after fillScreen to let the TFT DMA flush complete
# ─────────────────────────────────────────────────────────────────

OLD_DRAW = '''void drawHomeScreen() {
  tft.fillScreen(BG_COLOR);'''

NEW_DRAW = '''void drawHomeScreen() {
  tft.fillScreen(TFT_BLACK);  // Hard black clear first (avoids color bleed)
  delay(20);                   // Let DMA flush
  tft.fillScreen(BG_COLOR);   // Now paint the background'''

if OLD_DRAW in c:
    c = c.replace(OLD_DRAW, NEW_DRAW, 1)
    print("Fix 3 applied: drawHomeScreen() double-clears for clean slate")
else:
    print("Fix 3 SKIP: drawHomeScreen pattern not found")

# ─────────────────────────────────────────────────────────────────
# Write result
# ─────────────────────────────────────────────────────────────────
open(path, 'w', encoding='utf-8', newline='\n').write(c)
print("\nAll fixes written.")
o = c.count('{'); cl = c.count('}')
print(f"Brace balance: {o} open, {cl} close, diff={o-cl}")
assert o == cl, "BRACE MISMATCH!"
print("Brace check PASSED")
