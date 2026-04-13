/*
 * ═══════════════════════════════════════════════════════════════
 *   Blockchain Voting System — ESP32 + TFT + Fingerprint Sensor
 * ═══════════════════════════════════════════════════════════════
 *
 *  Hardware:
 *    - ESP32 Dev Board
 *    - ILI9341 320×240 TFT Display (SPI)
 *    - XPT2046 Touchscreen Controller
 *    - R307 / AS608 Fingerprint Sensor (UART on Serial2)
 *
 *  Libraries Required (install via Arduino Library Manager):
 *    - TFT_eSPI           (configure pins in User_Setup.h)
 *    - ArduinoJson        (v6+)
 *    - Adafruit_Fingerprint
 *
 *  Fingerprint Wiring:
 *    Sensor TX  → ESP32 GPIO 16 (RX2)
 *    Sensor RX  → ESP32 GPIO 17 (TX2)
 *    VCC → 3.3V,  GND → GND
 *
 *  Flow:
 *    Fingerprint Wait → Welcome → Candidates → Confirm → Result
 *
 *  The ESP32 talks to a Node.js backend over WiFi (HTTP).
 * ═══════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Adafruit_Fingerprint.h>

// ─── USER CONFIG ─────────────────────────────────────────────
const char* WIFI_SSID     = "Saurabh";
const char* WIFI_PASSWORD = "12345678";  // Set this to match your hotspot password
const char* SERVER_IP     = "10.182.151.220";
const int   SERVER_PORT   = 3000;

// Fingerprint sensor on Serial2 (GPIO 16=RX, GPIO 17=TX)
#define FP_RX 16
#define FP_TX 17

// Admin fingerprint is stored at this slot in the sensor
#define ADMIN_FP_ID 127  // Reserved slot for admin fingerprint
// ──────────────────────────────────────────────────────────────

// ─── Display & Touch ─────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

#define SCREEN_W 320
#define SCREEN_H 240

// ─── Fingerprint Sensor ──────────────────────────────────────
HardwareSerial fpSerial(2);  // Serial2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fpSerial);

// ─── App State ───────────────────────────────────────────────
enum Screen {
  SCREEN_FINGERPRINT,     // "Place your finger on sensor"
  SCREEN_WELCOME,         // "Welcome, [Name]!"
  SCREEN_CANDIDATES,      // Candidate selection
  SCREEN_CONFIRM,         // Confirm vote
  SCREEN_RESULT,          // Vote recorded + live results
  SCREEN_ALREADY_VOTED,   // Already voted warning
  SCREEN_ERROR,           // Error screen
  SCREEN_ENROLL_WAIT,     // Admin: waiting for finger to enroll
  SCREEN_ENROLL_REMOVE,   // Admin: remove and place finger again
  SCREEN_ENROLL_AGAIN,    // Admin: place same finger again
  SCREEN_ENROLL_DONE,     // Admin: enrollment success
  SCREEN_ADMIN_MENU,      // Admin: main menu
  SCREEN_ADMIN_NAME_ENTRY,// Admin: enter voter name
  SCREEN_ADMIN_SETUP,     // First boot: setup admin fingerprint
  SCREEN_SERVER_ENROLL    // Server-triggered enrollment (from browser)
};

bool adminExists = false;

Screen currentScreen = SCREEN_FINGERPRINT;

// Voter info
String voterId = "";
String voterName = "";
int    voterFingerprintId = -1;

// Candidates
struct Candidate {
  int    id;
  String name;
  int    voteCount;
};

Candidate candidates[10];
int candidateCount = 0;
int selectedCandidate = -1;

// Result data
String resultMessage = "";
String txHash = "";
int    resultVoteCount = 0;

// Enrollment state
int enrollId = -1;
String enrollName = "";

// Admin name entry state
char nameBuffer[20] = "";
int nameLen = 0;
int kbPage = 0;  // 0=ABC, 1=abc, 2=123

// Server enrollment state
int serverEnrollId = -1;
String serverEnrollName = "";
String serverEnrollUserId = "";

// Timing
unsigned long welcomeShownTime = 0;
unsigned long resultShownTime = 0;
unsigned long lastScanTime = 0;
unsigned long lastResultRefresh = 0;
unsigned long lastServerPoll = 0;
const unsigned long SERVER_POLL_INTERVAL = 3000; // Poll every 3 seconds
unsigned long lastFpAnimTime = 0;
int fpAnimFrame = 0;

// ─── Enhanced Colors (Premium Dark Theme) ───────────────────
#define BG_COLOR       0x0841   // Deep dark blue-black
#define BG_DARK        0x0000   // Pure black for contrast
#define CARD_COLOR     0x18C3   // Elevated card surface
#define CARD_HOVER     0x2965   // Lighter card hover
#define PRIMARY_COLOR  0x5ADF   // Vivid indigo-blue
#define PRIMARY_LIGHT  0x741F   // Lighter indigo
#define SUCCESS_COLOR  0x2E8B   // Emerald green
#define SUCCESS_LIGHT  0x47ED   // Light green
#define ERROR_COLOR    0xF186   // Soft red-pink
#define WARNING_COLOR  0xFCC0   // Warm amber
#define TEXT_COLOR     0xFFFF   // Pure white
#define TEXT_DIM       0x9CF3   // Soft gray text
#define TEXT_MUTED     0x6B6D   // Very dim text
#define ACCENT_PURPLE  0xA11F   // Rich purple
#define ACCENT_TEAL    0x2F1C   // Deep teal
#define ACCENT_CYAN    0x067F   // Bright cyan
#define GOLD_COLOR     0xFE60   // Gold for winner/highlights
#define DIVIDER_COLOR  0x1082   // Subtle divider line

// ─── Helpers ─────────────────────────────────────────────────
String serverUrl(String path) {
  return "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + path;
}

// Draw centered text with optional transparent background
void drawCenteredText(const char* text, int y, uint16_t color, int fontSize) {
  tft.setTextColor(color, BG_COLOR);
  tft.setTextSize(fontSize);
  int16_t tw = tft.textWidth(text);
  int16_t x = (SCREEN_W - tw) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(text);
}

// Draw a premium rounded button with shadow effect
void drawButton(int x, int y, int w, int h, const char* label, uint16_t bgColor, uint16_t textColor) {
  // Shadow
  tft.fillRoundRect(x + 2, y + 2, w, h, 8, BG_DARK);
  // Button body
  tft.fillRoundRect(x, y, w, h, 8, bgColor);
  // Highlight line on top
  tft.drawFastHLine(x + 4, y + 1, w - 8, textColor);
  // Label
  tft.setTextColor(textColor);
  tft.setTextSize(1);
  int16_t tw = tft.textWidth(label);
  int16_t tx = x + (w - tw) / 2;
  int16_t ty = y + (h - 8) / 2;
  tft.setCursor(tx, ty);
  tft.print(label);
}

// Draw a gradient-like horizontal line (simulated with color steps)
void drawGradientLine(int x, int y, int w, uint16_t color1, uint16_t color2) {
  int half = w / 2;
  for (int i = 0; i < w; i++) {
    uint16_t color = (i < half) ? color1 : color2;
    tft.drawPixel(x + i, y, color);
  }
}

// Draw decorative header bar
void drawHeaderBar(const char* title, uint16_t accentColor) {
  // Accent top strip
  tft.fillRect(0, 0, SCREEN_W, 3, accentColor);
  // Title
  drawCenteredText(title, 10, accentColor, 2);
  // Gradient line under title
  drawGradientLine(40, 30, SCREEN_W - 80, accentColor, ACCENT_PURPLE);
  tft.drawFastHLine(40, 31, SCREEN_W - 80, DIVIDER_COLOR);
}

bool isTouched(int tx, int ty, int bx, int by, int bw, int bh) {
  return (tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh);
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Fingerprint Wait (Premium Design)
// ═══════════════════════════════════════════════════════════════

void drawFingerprintScreen() {
  tft.fillScreen(BG_COLOR);

  // Top accent strip
  tft.fillRect(0, 0, SCREEN_W, 3, PRIMARY_COLOR);

  // Title with branding
  drawCenteredText("SECURE E-VOTE", 12, PRIMARY_COLOR, 2);
  tft.drawFastHLine(30, 32, SCREEN_W - 60, DIVIDER_COLOR);

  // Subtitle
  drawCenteredText("Blockchain Powered Voting", 38, TEXT_MUTED, 1);

  // Fingerprint icon — concentric circles with pulse effect
  int cx = SCREEN_W / 2;
  int cy = 115;

  // Outer glow ring
  tft.drawCircle(cx, cy, 45, DIVIDER_COLOR);
  tft.drawCircle(cx, cy, 44, DIVIDER_COLOR);

  // Main circle border (double)
  tft.drawCircle(cx, cy, 38, PRIMARY_COLOR);
  tft.drawCircle(cx, cy, 37, PRIMARY_COLOR);

  // Inner decorative circle
  tft.drawCircle(cx, cy, 28, ACCENT_TEAL);

  // Fingerprint pattern (curved lines inside)
  for (int i = -18; i <= 18; i += 5) {
    int halfW = sqrt(max(0, 22 * 22 - i * i));
    // Draw arc-like lines
    for (int px = -halfW; px <= halfW; px++) {
      int wave = sin((float)(px + cx) * 0.15) * 2;
      tft.drawPixel(cx + px, cy + i + wave, TEXT_DIM);
    }
  }

  // Central dot
  tft.fillCircle(cx, cy, 3, ACCENT_CYAN);

  // Instructions with icon hints
  drawCenteredText("Place your finger", 170, TEXT_COLOR, 2);
  drawCenteredText("on the sensor", 192, TEXT_COLOR, 2);

  // Animated status text
  tft.drawFastHLine(60, 215, SCREEN_W - 120, DIVIDER_COLOR);
  drawCenteredText("Waiting for fingerprint...", 222, TEXT_DIM, 1);

  // Bottom branding
  tft.setTextSize(1);
  tft.setTextColor(TEXT_MUTED, BG_COLOR);
  tft.setCursor(SCREEN_W - 85, SCREEN_H - 10);
  tft.print("Ethereum");
}

void animateFingerprintPulse() {
  unsigned long now = millis();
  if (now - lastFpAnimTime < 600) return;
  lastFpAnimTime = now;

  int cx = SCREEN_W / 2;
  int cy = 115;
  fpAnimFrame = (fpAnimFrame + 1) % 4;

  // Pulse ring animation — expanding rings
  uint16_t colors[] = {PRIMARY_COLOR, ACCENT_TEAL, ACCENT_PURPLE, ACCENT_CYAN};
  int radii[] = {48, 52, 56, 60};

  // Clear previous rings
  for (int r = 47; r <= 63; r++) {
    tft.drawCircle(cx, cy, r, BG_COLOR);
  }

  // Draw current expanding ring
  tft.drawCircle(cx, cy, radii[fpAnimFrame], colors[fpAnimFrame]);

  // Also pulse the center dot
  uint16_t dotColor = (fpAnimFrame % 2 == 0) ? ACCENT_CYAN : PRIMARY_COLOR;
  tft.fillCircle(cx, cy, 3, dotColor);
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Welcome (Premium Design)
// ═══════════════════════════════════════════════════════════════

void drawWelcomeScreen() {
  tft.fillScreen(BG_COLOR);

  // Top accent strip — green for success
  tft.fillRect(0, 0, SCREEN_W, 4, SUCCESS_COLOR);

  // Success icon — checkmark circle
  int cx = SCREEN_W / 2;
  tft.fillCircle(cx, 60, 32, SUCCESS_COLOR);
  tft.fillCircle(cx, 60, 28, BG_COLOR);
  tft.fillCircle(cx, 60, 26, SUCCESS_COLOR);
  tft.setTextColor(BG_COLOR);
  tft.setTextSize(3);
  tft.setCursor(cx - 12, 48);
  tft.print("OK");

  // Decorative line
  tft.drawFastHLine(50, 102, SCREEN_W - 100, SUCCESS_COLOR);
  tft.drawFastHLine(50, 103, SCREEN_W - 100, DIVIDER_COLOR);

  // Welcome text
  drawCenteredText("AUTHENTICATED", 112, SUCCESS_COLOR, 2);

  // Voter name card
  tft.fillRoundRect(30, 138, SCREEN_W - 60, 36, 8, CARD_COLOR);
  tft.drawRoundRect(30, 138, SCREEN_W - 60, 36, 8, SUCCESS_COLOR);
  drawCenteredText(voterName.c_str(), 148, TEXT_COLOR, 2);

  // ID display
  String idText = "ID: " + voterId;
  drawCenteredText(idText.c_str(), 185, TEXT_MUTED, 1);

  // Loading indicator
  drawCenteredText("Loading candidates...", 210, TEXT_DIM, 1);

  // Progress dots animation area
  tft.fillCircle(cx - 15, 228, 3, PRIMARY_COLOR);
  tft.fillCircle(cx, 228, 3, TEXT_DIM);
  tft.fillCircle(cx + 15, 228, 3, TEXT_DIM);

  welcomeShownTime = millis();
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Candidates (Premium Card Selection)
// ═══════════════════════════════════════════════════════════════

void fetchCandidates() {
  HTTPClient http;
  http.begin(serverUrl("/api/candidates"));
  int httpCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, response);
    JsonArray arr = doc["candidates"].as<JsonArray>();
    candidateCount = 0;
    for (JsonObject c : arr) {
      if (candidateCount >= 10) break;
      candidates[candidateCount].id = c["id"].as<int>();
      candidates[candidateCount].name = c["name"].as<String>();
      candidates[candidateCount].voteCount = c["voteCount"].as<int>();
      candidateCount++;
    }
    currentScreen = SCREEN_CANDIDATES;
    drawCandidateScreen();
  } else {
    resultMessage = "Failed to load candidates";
    currentScreen = SCREEN_ERROR;
    drawErrorScreen(resultMessage);
  }
}

void drawCandidateScreen() {
  tft.fillScreen(BG_COLOR);

  // Top accent strip
  tft.fillRect(0, 0, SCREEN_W, 3, PRIMARY_COLOR);

  // Voter name in top-left
  tft.setTextSize(1);
  tft.setTextColor(TEXT_MUTED);
  tft.setCursor(8, 8);
  tft.print("Voter: ");
  tft.setTextColor(ACCENT_CYAN);
  tft.print(voterName.c_str());

  // Title
  drawCenteredText("SELECT CANDIDATE", 24, PRIMARY_COLOR, 2);
  tft.drawFastHLine(15, 43, SCREEN_W - 30, DIVIDER_COLOR);

  // Candidate cards — premium elevated style
  int btnHeight = 40;
  int startY = 50;
  int gap = 5;

  // Color palette for each candidate card accent
  uint16_t accentColors[] = {PRIMARY_COLOR, ACCENT_PURPLE, ACCENT_TEAL, SUCCESS_COLOR, WARNING_COLOR};

  for (int i = 0; i < candidateCount; i++) {
    int by = startY + i * (btnHeight + gap);
    if (by + btnHeight > SCREEN_H - 5) break;

    // Card shadow
    tft.fillRoundRect(12, by + 2, SCREEN_W - 22, btnHeight, 10, BG_DARK);
    // Card body
    tft.fillRoundRect(10, by, SCREEN_W - 20, btnHeight, 10, CARD_COLOR);
    // Left accent stripe
    tft.fillRoundRect(10, by, 5, btnHeight, 3, accentColors[i % 5]);

    // Candidate number badge
    tft.fillRoundRect(22, by + 8, 26, 24, 6, accentColors[i % 5]);
    tft.setTextColor(TEXT_COLOR);
    tft.setTextSize(2);
    tft.setCursor(29, by + 12);
    tft.print(i + 1);

    // Candidate name
    tft.setTextSize(2);
    tft.setTextColor(TEXT_COLOR);
    tft.setCursor(56, by + 12);
    tft.print(candidates[i].name.c_str());

    // Right arrow indicator
    tft.setTextSize(1);
    tft.setTextColor(TEXT_DIM);
    tft.setCursor(SCREEN_W - 30, by + 16);
    tft.print(">");
  }
}

void handleCandidateTouch(int tx, int ty) {
  int btnHeight = 40;
  int startY = 50;
  int gap = 5;

  for (int i = 0; i < candidateCount; i++) {
    int by = startY + i * (btnHeight + gap);
    if (isTouched(tx, ty, 10, by, SCREEN_W - 20, btnHeight)) {
      // Visual feedback — highlight the selected card
      tft.fillRoundRect(10, by, SCREEN_W - 20, btnHeight, 10, CARD_HOVER);
      delay(100);

      selectedCandidate = i;
      currentScreen = SCREEN_CONFIRM;
      drawConfirmScreen();
      return;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Confirm (Premium Confirmation Dialog)
// ═══════════════════════════════════════════════════════════════

void drawConfirmScreen() {
  tft.fillScreen(BG_COLOR);

  // Top accent — warning amber
  tft.fillRect(0, 0, SCREEN_W, 4, WARNING_COLOR);

  // Title
  drawCenteredText("CONFIRM VOTE", 16, WARNING_COLOR, 2);
  tft.drawFastHLine(50, 36, SCREEN_W - 100, DIVIDER_COLOR);

  // Candidate info card
  tft.fillRoundRect(25, 50, SCREEN_W - 50, 60, 10, CARD_COLOR);
  tft.drawRoundRect(25, 50, SCREEN_W - 50, 60, 10, PRIMARY_COLOR);
  drawCenteredText("Your choice:", 58, TEXT_DIM, 1);
  drawCenteredText(candidates[selectedCandidate].name.c_str(), 78, TEXT_COLOR, 2);

  // Voter info
  tft.fillRoundRect(50, 120, SCREEN_W - 100, 20, 6, CARD_COLOR);
  String voterStr = "Voter: " + voterName;
  drawCenteredText(voterStr.c_str(), 124, ACCENT_CYAN, 1);

  // Warning message with icon
  drawCenteredText("! This cannot be undone !", 150, WARNING_COLOR, 1);

  // Buttons — Cancel and Confirm with premium styling
  // Cancel button (left)
  drawButton(25, 178, 125, 45, "CANCEL", ERROR_COLOR, TEXT_COLOR);

  // Confirm button (right) — green with emphasis
  drawButton(170, 178, 125, 45, "CONFIRM", SUCCESS_COLOR, TEXT_COLOR);

  // Blockchain info at bottom
  drawCenteredText("Stored on Ethereum Blockchain", 230, TEXT_MUTED, 1);
}

void handleConfirmTouch(int tx, int ty) {
  // CANCEL
  if (isTouched(tx, ty, 25, 178, 125, 45)) {
    currentScreen = SCREEN_CANDIDATES;
    drawCandidateScreen();
    return;
  }

  // CONFIRM
  if (isTouched(tx, ty, 170, 178, 125, 45)) {
    submitVote();
    return;
  }
}

void submitVote() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 3, ACCENT_CYAN);

  // Animated loading screen
  drawCenteredText("SUBMITTING VOTE", 70, PRIMARY_COLOR, 2);
  tft.drawFastHLine(60, 92, SCREEN_W - 120, DIVIDER_COLOR);

  // Loading animation
  drawCenteredText("Recording on blockchain...", 105, TEXT_DIM, 1);

  // Animated blocks
  int bx = SCREEN_W / 2 - 40;
  for (int i = 0; i < 4; i++) {
    tft.fillRoundRect(bx + i * 22, 130, 18, 18, 4, (i % 2 == 0) ? PRIMARY_COLOR : ACCENT_PURPLE);
    delay(200);
  }

  drawCenteredText("Please wait...", 165, TEXT_MUTED, 1);

  HTTPClient http;
  http.begin(serverUrl("/api/vote"));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["voterId"] = voterId;
  doc["candidateId"] = selectedCandidate;
  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    StaticJsonDocument<512> resDoc;
    deserializeJson(resDoc, response);
    txHash = resDoc["transactionHash"].as<String>();
    resultVoteCount = resDoc["candidate"]["voteCount"].as<int>();
    resultMessage = resDoc["message"].as<String>();
    currentScreen = SCREEN_RESULT;
    fetchAndDrawResults();
  } else if (httpCode == 403) {
    currentScreen = SCREEN_ALREADY_VOTED;
    drawAlreadyVotedScreen();
  } else {
    resultMessage = "Vote failed!";
    currentScreen = SCREEN_ERROR;
    drawErrorScreen(resultMessage);
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Result (Success) — Premium Results Dashboard
// ═══════════════════════════════════════════════════════════════

void fetchAndDrawResults() {
  tft.fillScreen(BG_COLOR);

  // Top accent — success green
  tft.fillRect(0, 0, SCREEN_W, 4, SUCCESS_COLOR);

  // Success icon
  int cx = SCREEN_W / 2;
  tft.fillCircle(cx, 24, 16, SUCCESS_COLOR);
  tft.setTextColor(BG_COLOR);
  tft.setTextSize(2);
  tft.setCursor(cx - 6, 17);
  tft.print("Y");

  drawCenteredText("VOTE RECORDED!", 48, SUCCESS_COLOR, 2);
  drawCenteredText("Ethereum Blockchain", 68, TEXT_MUTED, 1);

  // TX hash display — premium mono style
  tft.fillRoundRect(8, 80, SCREEN_W - 16, 16, 4, CARD_COLOR);
  tft.setTextSize(1);
  tft.setTextColor(TEXT_MUTED);
  tft.setCursor(12, 84);
  tft.print("TX:");
  tft.setTextColor(ACCENT_CYAN);
  String shortHash = txHash.substring(0, 12) + "..." + txHash.substring(txHash.length() - 8);
  tft.print(shortHash.c_str());

  // Divider
  tft.drawFastHLine(8, 100, SCREEN_W - 16, PRIMARY_COLOR);
  drawCenteredText("LIVE RESULTS", 104, PRIMARY_COLOR, 1);

  // Fetch live results from server
  HTTPClient http;
  http.begin(serverUrl("/api/results"));
  int httpCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, response);
    JsonArray arr = doc["candidates"].as<JsonArray>();
    int totalVotes = doc["totalVotes"].as<int>();

    int barY = 116;
    int barH = 18;
    int barGap = 5;
    int maxBarW = SCREEN_W - 130;

    // Accent colors for bars
    uint16_t barColors[] = {PRIMARY_COLOR, ACCENT_PURPLE, ACCENT_TEAL, SUCCESS_COLOR, WARNING_COLOR, ERROR_COLOR};

    // Find max votes for scaling
    int maxVotes = 1;
    for (JsonObject c : arr) {
      int v = c["voteCount"].as<int>();
      if (v > maxVotes) maxVotes = v;
    }

    int idx = 0;
    for (JsonObject c : arr) {
      if (idx >= 5) break;  // Max 5 on screen
      int y = barY + idx * (barH + barGap);
      String name = c["name"].as<String>();
      int votes = c["voteCount"].as<int>();
      int barW = max(4, (int)((float)votes / maxVotes * maxBarW));

      // Name
      tft.setTextSize(1);
      tft.setTextColor(TEXT_COLOR);
      tft.setCursor(10, y + 5);
      if (name.length() > 10) name = name.substring(0, 10);
      tft.print(name.c_str());

      // Bar background
      tft.fillRoundRect(80, y, maxBarW + 5, barH, 4, CARD_COLOR);
      // Bar fill
      tft.fillRoundRect(80, y, barW, barH, 4, barColors[idx % 6]);

      // Highlight selected candidate's bar
      if (idx == selectedCandidate) {
        tft.drawRoundRect(79, y - 1, barW + 2, barH + 2, 5, GOLD_COLOR);
      }

      // Vote count badge
      tft.setTextColor(TEXT_COLOR, BG_COLOR);
      tft.setCursor(80 + maxBarW + 10, y + 5);
      tft.print(votes);

      idx++;
    }

    // Total votes display
    tft.drawFastHLine(8, SCREEN_H - 32, SCREEN_W - 16, DIVIDER_COLOR);
    tft.setTextSize(1);
    tft.setTextColor(TEXT_MUTED);
    tft.setCursor(10, SCREEN_H - 26);
    tft.print("Total: ");
    tft.setTextColor(GOLD_COLOR);
    tft.print(totalVotes);
    tft.setTextColor(TEXT_MUTED);
    tft.print(" votes on chain");
  }

  // "New Voter" button — premium
  drawButton(SCREEN_W - 110, SCREEN_H - 28, 100, 22, "NEW VOTER", PRIMARY_COLOR, TEXT_COLOR);

  resultShownTime = millis();
  lastResultRefresh = millis();
}

void refreshResults() {
  // Only refresh the bar area, not the whole screen
  unsigned long now = millis();
  if (now - lastResultRefresh < 5000) return;
  lastResultRefresh = now;

  // Re-fetch and redraw results area
  HTTPClient http;
  http.begin(serverUrl("/api/results"));
  int httpCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, response);
    JsonArray arr = doc["candidates"].as<JsonArray>();
    int totalVotes = doc["totalVotes"].as<int>();

    int barY = 116;
    int barH = 18;
    int barGap = 5;
    int maxBarW = SCREEN_W - 130;

    uint16_t barColors[] = {PRIMARY_COLOR, ACCENT_PURPLE, ACCENT_TEAL, SUCCESS_COLOR, WARNING_COLOR, ERROR_COLOR};

    int maxVotes = 1;
    for (JsonObject c : arr) {
      int v = c["voteCount"].as<int>();
      if (v > maxVotes) maxVotes = v;
    }

    // Clear bar area
    tft.fillRect(80, barY, SCREEN_W - 80, 5 * (barH + barGap), BG_COLOR);

    int idx = 0;
    for (JsonObject c : arr) {
      if (idx >= 5) break;
      int y = barY + idx * (barH + barGap);
      int votes = c["voteCount"].as<int>();
      int barW = max(4, (int)((float)votes / maxVotes * maxBarW));

      tft.fillRoundRect(80, y, maxBarW + 5, barH, 4, CARD_COLOR);
      tft.fillRoundRect(80, y, barW, barH, 4, barColors[idx % 6]);

      if (idx == selectedCandidate) {
        tft.drawRoundRect(79, y - 1, barW + 2, barH + 2, 5, GOLD_COLOR);
      }

      tft.setTextColor(TEXT_COLOR, BG_COLOR);
      tft.setCursor(80 + maxBarW + 10, y + 5);
      tft.print(votes);
      tft.print("  ");

      idx++;
    }

    // Update total
    tft.fillRect(10, SCREEN_H - 26, 200, 10, BG_COLOR);
    tft.setTextSize(1);
    tft.setTextColor(TEXT_MUTED);
    tft.setCursor(10, SCREEN_H - 26);
    tft.print("Total: ");
    tft.setTextColor(GOLD_COLOR);
    tft.print(totalVotes);
    tft.setTextColor(TEXT_MUTED);
    tft.print(" votes on chain");
  }
}

void handleResultTouch(int tx, int ty) {
  // NEW VOTER button
  if (isTouched(tx, ty, SCREEN_W - 110, SCREEN_H - 28, 100, 22)) {
    resetForNewVoter();
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Already Voted (Enhanced Warning)
// ═══════════════════════════════════════════════════════════════

void drawAlreadyVotedScreen() {
  tft.fillScreen(BG_COLOR);

  // Top accent — amber warning
  tft.fillRect(0, 0, SCREEN_W, 4, WARNING_COLOR);

  // Warning icon
  int cx = SCREEN_W / 2;
  tft.fillCircle(cx, 65, 30, WARNING_COLOR);
  tft.fillCircle(cx, 65, 26, BG_COLOR);
  tft.fillCircle(cx, 65, 24, WARNING_COLOR);
  tft.setTextColor(BG_COLOR);
  tft.setTextSize(3);
  tft.setCursor(cx - 7, 52);
  tft.print("!");

  drawCenteredText("ALREADY VOTED", 105, WARNING_COLOR, 2);

  // Voter info
  tft.fillRoundRect(40, 132, SCREEN_W - 80, 25, 6, CARD_COLOR);
  drawCenteredText(voterName.c_str(), 137, TEXT_COLOR, 1);

  drawCenteredText("has already cast a vote.", 165, TEXT_DIM, 1);
  drawCenteredText("Each voter can only vote once.", 180, TEXT_MUTED, 1);

  drawButton(60, 205, 200, 30, "NEW VOTER", PRIMARY_COLOR, TEXT_COLOR);
}

void handleAlreadyVotedTouch(int tx, int ty) {
  if (isTouched(tx, ty, 60, 205, 200, 30)) {
    resetForNewVoter();
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Error (Enhanced)
// ═══════════════════════════════════════════════════════════════

void drawErrorScreen(String msg) {
  tft.fillScreen(BG_COLOR);

  // Top accent — red
  tft.fillRect(0, 0, SCREEN_W, 4, ERROR_COLOR);

  // Error icon
  int cx = SCREEN_W / 2;
  tft.fillCircle(cx, 65, 30, ERROR_COLOR);
  tft.fillCircle(cx, 65, 26, BG_COLOR);
  tft.fillCircle(cx, 65, 24, ERROR_COLOR);
  tft.setTextColor(TEXT_COLOR);
  tft.setTextSize(3);
  tft.setCursor(cx - 7, 52);
  tft.print("X");

  drawCenteredText("ERROR", 105, ERROR_COLOR, 2);

  // Error message card
  tft.fillRoundRect(20, 132, SCREEN_W - 40, 25, 6, CARD_COLOR);
  drawCenteredText(msg.c_str(), 137, TEXT_DIM, 1);

  drawButton(60, 200, 200, 30, "TRY AGAIN", PRIMARY_COLOR, TEXT_COLOR);
}

void handleErrorTouch(int tx, int ty) {
  if (isTouched(tx, ty, 60, 200, 200, 30)) {
    resetForNewVoter();
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Admin Menu (after admin fingerprint auth)
// ═══════════════════════════════════════════════════════════════

void drawAdminMenu() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, ACCENT_PURPLE);

  drawCenteredText("ADMIN PANEL", 14, ACCENT_PURPLE, 2);
  tft.drawFastHLine(30, 34, SCREEN_W - 60, DIVIDER_COLOR);
  drawCenteredText("Authenticated via fingerprint", 40, TEXT_MUTED, 1);

  // Button 1: Register New Voter
  tft.fillRoundRect(20, 65, SCREEN_W - 40, 50, 10, CARD_COLOR);
  tft.drawRoundRect(20, 65, SCREEN_W - 40, 50, 10, SUCCESS_COLOR);
  tft.fillRoundRect(30, 75, 30, 30, 6, SUCCESS_COLOR);
  tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
  tft.setCursor(37, 81); tft.print("+");
  tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
  tft.setCursor(70, 78); tft.print("Register Voter");
  tft.setTextColor(TEXT_DIM); tft.setTextSize(1);
  tft.setCursor(70, 98); tft.print("Enroll fingerprint + name");

  // Button 2: View Voters
  tft.fillRoundRect(20, 125, SCREEN_W - 40, 50, 10, CARD_COLOR);
  tft.drawRoundRect(20, 125, SCREEN_W - 40, 50, 10, PRIMARY_COLOR);
  tft.fillRoundRect(30, 135, 30, 30, 6, PRIMARY_COLOR);
  tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
  tft.setCursor(35, 141); tft.print("?");
  tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
  tft.setCursor(70, 138); tft.print("View Results");
  tft.setTextColor(TEXT_DIM); tft.setTextSize(1);
  tft.setCursor(70, 158); tft.print("Live voting results");

  // Button 3: Exit
  drawButton(80, 190, 160, 35, "EXIT TO VOTING", ERROR_COLOR, TEXT_COLOR);
}

void handleAdminMenuTouch(int tx, int ty) {
  // Register New Voter
  if (isTouched(tx, ty, 20, 65, SCREEN_W - 40, 50)) {
    currentScreen = SCREEN_ADMIN_NAME_ENTRY;
    nameLen = 0;
    nameBuffer[0] = '\0';
    kbPage = 0;
    drawNameEntryScreen();
    return;
  }
  // View Results
  if (isTouched(tx, ty, 20, 125, SCREEN_W - 40, 50)) {
    // Fetch and show results (reuse existing)
    selectedCandidate = -1;
    txHash = "Admin View";
    currentScreen = SCREEN_RESULT;
    fetchAndDrawResults();
    return;
  }
  // Exit
  if (isTouched(tx, ty, 80, 190, 160, 35)) {
    resetForNewVoter();
    return;
  }
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Name Entry (On-screen keyboard for voter name)
// ═══════════════════════════════════════════════════════════════

void drawNameEntryScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 3, SUCCESS_COLOR);

  drawCenteredText("ENTER VOTER NAME", 8, SUCCESS_COLOR, 2);

  // Name display box
  tft.fillRoundRect(10, 30, SCREEN_W - 20, 28, 6, CARD_COLOR);
  tft.drawRoundRect(10, 30, SCREEN_W - 20, 28, 6, PRIMARY_COLOR);
  tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
  tft.setCursor(18, 36);
  tft.print(nameBuffer);
  tft.setTextColor(PRIMARY_COLOR);
  tft.print("_");

  // Keyboard layout
  const char* rows[3];
  if (kbPage == 0) {
    rows[0] = "ABCDEFGHIJ";
    rows[1] = "KLMNOPQRST";
    rows[2] = "UVWXYZ .-";
  } else if (kbPage == 1) {
    rows[0] = "abcdefghij";
    rows[1] = "klmnopqrst";
    rows[2] = "uvwxyz .-";
  } else {
    rows[0] = "1234567890";
    rows[1] = "          ";
    rows[2] = "         ";
  }

  int keyW = 28;
  int keyH = 28;
  int startY = 65;
  int gap = 4;

  for (int r = 0; r < 3; r++) {
    int rowLen = strlen(rows[r]);
    int startX = (SCREEN_W - rowLen * (keyW + gap)) / 2;
    for (int c = 0; c < rowLen; c++) {
      int kx = startX + c * (keyW + gap);
      int ky = startY + r * (keyH + gap);
      char ch = rows[r][c];
      if (ch == ' ') {
        tft.fillRoundRect(kx, ky, keyW, keyH, 4, CARD_COLOR);
        tft.setTextColor(TEXT_DIM); tft.setTextSize(1);
        tft.setCursor(kx + 4, ky + 10); tft.print("SP");
      } else {
        tft.fillRoundRect(kx, ky, keyW, keyH, 4, CARD_COLOR);
        tft.setTextColor(TEXT_COLOR); tft.setTextSize(2);
        tft.setCursor(kx + 8, ky + 6); tft.print(ch);
      }
    }
  }

  // Bottom buttons: DEL | ABC/abc/123 | NEXT
  int btnY = startY + 3 * (keyH + gap) + 5;
  drawButton(10, btnY, 70, 30, "DEL", ERROR_COLOR, TEXT_COLOR);

  const char* modeLabel = (kbPage == 0) ? "abc" : (kbPage == 1) ? "123" : "ABC";
  drawButton(90, btnY, 70, 30, modeLabel, ACCENT_PURPLE, TEXT_COLOR);

  drawButton(170, btnY, 140, 30, "NEXT >>", SUCCESS_COLOR, TEXT_COLOR);
}

void handleNameEntryTouch(int tx, int ty) {
  int keyW = 28;
  int keyH = 28;
  int startY = 65;
  int gap = 4;

  const char* rows[3];
  if (kbPage == 0) {
    rows[0] = "ABCDEFGHIJ";
    rows[1] = "KLMNOPQRST";
    rows[2] = "UVWXYZ .-";
  } else if (kbPage == 1) {
    rows[0] = "abcdefghij";
    rows[1] = "klmnopqrst";
    rows[2] = "uvwxyz .-";
  } else {
    rows[0] = "1234567890";
    rows[1] = "          ";
    rows[2] = "         ";
  }

  // Check keyboard keys
  for (int r = 0; r < 3; r++) {
    int rowLen = strlen(rows[r]);
    int startX = (SCREEN_W - rowLen * (keyW + gap)) / 2;
    for (int c = 0; c < rowLen; c++) {
      int kx = startX + c * (keyW + gap);
      int ky = startY + r * (keyH + gap);
      if (isTouched(tx, ty, kx, ky, keyW, keyH)) {
        char ch = rows[r][c];
        if (nameLen < 18) {
          nameBuffer[nameLen++] = ch;
          nameBuffer[nameLen] = '\0';
          drawNameEntryScreen();
        }
        return;
      }
    }
  }

  int btnY = startY + 3 * (keyH + gap) + 5;

  // DEL button
  if (isTouched(tx, ty, 10, btnY, 70, 30)) {
    if (nameLen > 0) {
      nameBuffer[--nameLen] = '\0';
      drawNameEntryScreen();
    }
    return;
  }

  // Mode toggle (ABC/abc/123)
  if (isTouched(tx, ty, 90, btnY, 70, 30)) {
    kbPage = (kbPage + 1) % 3;
    drawNameEntryScreen();
    return;
  }

  // NEXT button — proceed to fingerprint enrollment
  if (isTouched(tx, ty, 170, btnY, 140, 30)) {
    if (nameLen < 2) {
      // Name too short
      tft.fillRoundRect(10, 30, SCREEN_W - 20, 28, 6, ERROR_COLOR);
      drawCenteredText("Name too short!", 38, TEXT_COLOR, 1);
      delay(1000);
      drawNameEntryScreen();
      return;
    }
    enrollName = String(nameBuffer);
    int nextSlot = getNextEnrollSlot();
    if (nextSlot > 0) {
      startEnrollmentWithName(nextSlot, enrollName);
    } else {
      drawErrorScreen("Sensor memory full!");
      currentScreen = SCREEN_ERROR;
    }
    return;
  }
}

// Enrollment that also registers the name on the server
void startEnrollmentWithName(int id, String name) {
  enrollId = id;
  enrollName = name;
  currentScreen = SCREEN_ENROLL_WAIT;
  drawEnrollWaitScreen();

  Serial.printf("Enrolling voter '%s' at slot %d\n", name.c_str(), id);

  unsigned long startTime = millis();
  int p = -1;

  // Step 1: Capture first image
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("Image taken (1)");
    } else if (p == FINGERPRINT_NOFINGER) {
      uint16_t ttx, tty;
      if (tft.getTouch(&ttx, &tty)) {
        if (isTouched(ttx, tty, 10, SCREEN_H - 28, 100, 22)) {
          currentScreen = SCREEN_ADMIN_MENU;
          drawAdminMenu();
          return;
        }
      }
      if (millis() - startTime > 30000) {
        drawEnrollDoneScreen(false, "Timeout - no finger");
        currentScreen = SCREEN_ENROLL_DONE;
        return;
      }
      delay(100);
    } else {
      delay(100);
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Image processing failed");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 2: Remove finger
  currentScreen = SCREEN_ENROLL_REMOVE;
  drawEnrollRemoveScreen();
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(100);
  }

  // Step 3: Place same finger again
  currentScreen = SCREEN_ENROLL_AGAIN;
  drawEnrollAgainScreen();
  p = -1;
  startTime = millis();
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - startTime > 30000) {
        drawEnrollDoneScreen(false, "Timeout");
        currentScreen = SCREEN_ENROLL_DONE;
        return;
      }
      delay(100);
    } else if (p != FINGERPRINT_OK) {
      delay(100);
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Image processing failed");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 4: Create model
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Fingers did not match");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 5: Store model
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Storage failed");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  Serial.printf("Fingerprint stored in slot %d\n", id);

  // Step 6: Register on server
  tft.fillScreen(BG_COLOR);
  drawCenteredText("Registering on server...", 110, TEXT_DIM, 2);

  HTTPClient http;
  http.begin(serverUrl("/api/register"));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["fingerprintId"] = id;
  doc["name"] = name;
  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    Serial.printf("Voter '%s' registered on server!\n", name.c_str());
    drawEnrollDoneScreen(true, "");
    // Override the done screen to show the name
    tft.fillRoundRect(30, 148, SCREEN_W - 60, 20, 4, CARD_COLOR);
    drawCenteredText(name.c_str(), 151, GOLD_COLOR, 1);
  } else {
    Serial.printf("Server registration failed: %d\n", httpCode);
    drawEnrollDoneScreen(true, "");  // FP stored OK, server might have issue
    tft.fillRoundRect(30, 168, SCREEN_W - 60, 12, 4, CARD_COLOR);
    drawCenteredText("Server reg failed - use web", 170, WARNING_COLOR, 1);
  }
  currentScreen = SCREEN_ENROLL_DONE;
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Admin Setup (first boot — enroll admin fingerprint)
// ═══════════════════════════════════════════════════════════════

void drawAdminSetupScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);

  drawCenteredText("FIRST TIME SETUP", 14, GOLD_COLOR, 2);
  tft.drawFastHLine(30, 34, SCREEN_W - 60, DIVIDER_COLOR);

  drawCenteredText("No admin registered!", 50, WARNING_COLOR, 1);
  drawCenteredText("Register your admin", 75, TEXT_COLOR, 2);
  drawCenteredText("fingerprint now", 97, TEXT_COLOR, 2);

  // Fingerprint icon
  int cx = SCREEN_W / 2;
  int cy = 150;
  tft.drawCircle(cx, cy, 25, GOLD_COLOR);
  tft.drawCircle(cx, cy, 23, GOLD_COLOR);
  tft.fillCircle(cx, cy, 3, ACCENT_CYAN);

  drawCenteredText("Place finger on sensor", 190, TEXT_DIM, 1);
  drawCenteredText("Hold steady...", 205, TEXT_MUTED, 1);
}

void enrollAdmin() {
  drawAdminSetupScreen();
  currentScreen = SCREEN_ADMIN_SETUP;

  Serial.println("ADMIN SETUP: Enrolling admin fingerprint at slot 127");

  unsigned long startTime = millis();
  int p = -1;

  // Step 1: Wait for finger
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - startTime > 60000) {
        drawErrorScreen("Timeout - no finger");
        currentScreen = SCREEN_ERROR;
        return;
      }
      delay(100);
    } else if (p != FINGERPRINT_OK) {
      delay(100);
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    drawErrorScreen("Image processing failed");
    currentScreen = SCREEN_ERROR;
    return;
  }

  // Step 2: Remove finger
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);
  drawCenteredText("Step 1 captured!", 80, SUCCESS_COLOR, 2);
  drawCenteredText("Remove finger, then", 120, TEXT_COLOR, 2);
  drawCenteredText("place it again", 142, TEXT_COLOR, 2);

  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(100);
  }

  // Step 3: Place again
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);
  drawCenteredText("Place SAME finger", 80, TEXT_COLOR, 2);
  drawCenteredText("again on sensor", 105, TEXT_COLOR, 2);

  p = -1;
  startTime = millis();
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - startTime > 30000) {
        drawErrorScreen("Timeout");
        currentScreen = SCREEN_ERROR;
        return;
      }
      delay(100);
    } else if (p != FINGERPRINT_OK) {
      delay(100);
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    drawErrorScreen("Image processing failed");
    currentScreen = SCREEN_ERROR;
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    drawErrorScreen("Fingers did not match");
    currentScreen = SCREEN_ERROR;
    return;
  }

  p = finger.storeModel(ADMIN_FP_ID);
  if (p != FINGERPRINT_OK) {
    drawErrorScreen("Storage failed");
    currentScreen = SCREEN_ERROR;
    return;
  }

  // Success!
  adminExists = true;
  Serial.println("Admin fingerprint enrolled successfully!");

  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, SUCCESS_COLOR);

  tft.fillCircle(SCREEN_W / 2, 70, 28, SUCCESS_COLOR);
  tft.setTextColor(BG_COLOR); tft.setTextSize(3);
  tft.setCursor(SCREEN_W / 2 - 12, 57); tft.print("OK");

  drawCenteredText("ADMIN REGISTERED!", 115, SUCCESS_COLOR, 2);
  drawCenteredText("You can now manage voters", 145, TEXT_DIM, 1);
  drawCenteredText("by placing your finger", 160, TEXT_DIM, 1);

  delay(3000);
  resetForNewVoter();
}

// Check if admin fingerprint exists in sensor
bool checkAdminExists() {
  uint8_t p = finger.loadModel(ADMIN_FP_ID);
  return (p == FINGERPRINT_OK);
}

// ═══════════════════════════════════════════════════════════════
//   SCREEN: Enrollment (Admin Mode — Enhanced)
// ═══════════════════════════════════════════════════════════════

void drawEnrollWaitScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, ACCENT_PURPLE);

  drawCenteredText("ADMIN MODE", 14, ACCENT_PURPLE, 2);
  tft.drawFastHLine(30, 34, SCREEN_W - 60, DIVIDER_COLOR);
  drawCenteredText("Fingerprint Enrollment", 42, TEXT_DIM, 1);

  // Slot ID display
  tft.fillRoundRect(100, 60, 120, 30, 8, CARD_COLOR);
  tft.drawRoundRect(100, 60, 120, 30, 8, ACCENT_PURPLE);
  String idText = "Slot #" + String(enrollId);
  drawCenteredText(idText.c_str(), 68, GOLD_COLOR, 2);

  // Fingerprint icon
  int cx = SCREEN_W / 2;
  int cy = 135;
  tft.drawCircle(cx, cy, 30, ACCENT_PURPLE);
  tft.drawCircle(cx, cy, 28, ACCENT_PURPLE);
  for (int i = -12; i <= 12; i += 5) {
    int halfW = sqrt(max(0, 15 * 15 - i * i));
    tft.drawFastHLine(cx - halfW, cy + i, halfW * 2, TEXT_DIM);
  }

  drawCenteredText("Place finger on sensor", 180, TEXT_COLOR, 2);
  drawCenteredText("Hold steady...", 202, TEXT_DIM, 1);

  drawButton(10, SCREEN_H - 28, 100, 22, "CANCEL", ERROR_COLOR, TEXT_COLOR);
}

void drawEnrollRemoveScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, ACCENT_PURPLE);
  drawCenteredText("ADMIN MODE", 14, ACCENT_PURPLE, 2);
  drawCenteredText("Step 1 captured!", 80, SUCCESS_COLOR, 2);
  drawCenteredText("Remove your finger", 120, TEXT_COLOR, 2);
  drawCenteredText("then place it again", 145, TEXT_DIM, 1);
}

void drawEnrollAgainScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, ACCENT_PURPLE);
  drawCenteredText("ADMIN MODE", 14, ACCENT_PURPLE, 2);
  drawCenteredText("Place SAME finger", 80, TEXT_COLOR, 2);
  drawCenteredText("again on the sensor", 105, TEXT_COLOR, 2);
  drawCenteredText("Hold steady...", 140, TEXT_DIM, 1);
}

void drawEnrollDoneScreen(bool success, String msg) {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, ACCENT_PURPLE);
  drawCenteredText("ADMIN MODE", 14, ACCENT_PURPLE, 2);

  int cx = SCREEN_W / 2;

  if (success) {
    tft.fillCircle(cx, 80, 28, SUCCESS_COLOR);
    tft.fillCircle(cx, 80, 24, BG_COLOR);
    tft.fillCircle(cx, 80, 22, SUCCESS_COLOR);
    tft.setTextColor(BG_COLOR);
    tft.setTextSize(2);
    tft.setCursor(cx - 12, 73);
    tft.print("OK");

    drawCenteredText("ENROLLED!", 120, SUCCESS_COLOR, 2);
    String slotMsg = "Fingerprint ID: " + String(enrollId);
    drawCenteredText(slotMsg.c_str(), 150, GOLD_COLOR, 1);
    drawCenteredText("Register name on web panel", 170, TEXT_DIM, 1);
  } else {
    tft.fillCircle(cx, 80, 28, ERROR_COLOR);
    tft.fillCircle(cx, 80, 24, BG_COLOR);
    tft.fillCircle(cx, 80, 22, ERROR_COLOR);
    tft.setTextColor(TEXT_COLOR);
    tft.setTextSize(2);
    tft.setCursor(cx - 6, 73);
    tft.print("X");

    drawCenteredText("FAILED", 120, ERROR_COLOR, 2);
    drawCenteredText(msg.c_str(), 150, TEXT_DIM, 1);
  }

  drawButton(60, 200, 200, 30, "BACK TO VOTING", PRIMARY_COLOR, TEXT_COLOR);
}

// ═══════════════════════════════════════════════════════════════
//   Fingerprint Functions
// ═══════════════════════════════════════════════════════════════

void initFingerprint() {
  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor found!");
    Serial.print("Sensor capacity: ");
    Serial.println(finger.capacity);
  } else {
    Serial.println("Fingerprint sensor NOT found!");
    Serial.println("Check wiring: TX->GPIO16, RX->GPIO17");
  }
}

// Non-blocking fingerprint check — returns fingerprint ID or -1
int checkFingerprint() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    Serial.printf("Found fingerprint ID: %d, confidence: %d\n", finger.fingerID, finger.confidence);
    return finger.fingerID;
  }

  return -2;  // Finger detected but not recognized
}

// Authenticate fingerprint with server
void authenticateFingerprint(int fpId) {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 3, ACCENT_CYAN);
  drawCenteredText("VERIFYING", 90, PRIMARY_COLOR, 2);
  drawCenteredText("Authenticating fingerprint...", 115, TEXT_DIM, 1);

  // Animated dots
  for (int i = 0; i < 3; i++) {
    tft.fillCircle(SCREEN_W / 2 - 15 + i * 15, 145, 4, (i == 0) ? PRIMARY_COLOR : TEXT_DIM);
  }

  HTTPClient http;
  http.begin(serverUrl("/api/auth/fingerprint"));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["fingerprintId"] = fpId;
  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  String response = http.getString();
  http.end();

  if (httpCode == 200) {
    StaticJsonDocument<512> resDoc;
    deserializeJson(resDoc, response);
    voterId = resDoc["voter"]["voterId"].as<String>();
    voterName = resDoc["voter"]["name"].as<String>();
    voterFingerprintId = fpId;

    currentScreen = SCREEN_WELCOME;
    drawWelcomeScreen();
  } else if (httpCode == 403) {
    StaticJsonDocument<512> resDoc;
    deserializeJson(resDoc, response);
    voterName = resDoc["voter"]["name"].as<String>();
    voterId = resDoc["voter"]["voterId"].as<String>();

    currentScreen = SCREEN_ALREADY_VOTED;
    drawAlreadyVotedScreen();
  } else if (httpCode == 404) {
    tft.fillScreen(BG_COLOR);
    tft.fillRect(0, 0, SCREEN_W, 4, ERROR_COLOR);
    drawCenteredText("NOT REGISTERED", 80, ERROR_COLOR, 2);
    drawCenteredText("This fingerprint is not", 115, TEXT_DIM, 1);
    drawCenteredText("registered in the system.", 130, TEXT_DIM, 1);
    drawCenteredText("Please register first.", 150, TEXT_MUTED, 1);
    drawButton(60, 200, 200, 30, "TRY AGAIN", PRIMARY_COLOR, TEXT_COLOR);
    currentScreen = SCREEN_ERROR;
    resultMessage = "Not registered";
  } else {
    resultMessage = "Auth failed!";
    currentScreen = SCREEN_ERROR;
    drawErrorScreen(resultMessage);
  }
}

// Enrollment process (blocking)
void startEnrollment(int id) {
  enrollId = id;
  currentScreen = SCREEN_ENROLL_WAIT;
  drawEnrollWaitScreen();

  Serial.printf("Starting enrollment for slot %d\n", id);

  // Wait for finger (with timeout and cancel)
  unsigned long startTime = millis();
  int p = -1;

  // Step 1: Capture first image
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("Image taken (1)");
    } else if (p == FINGERPRINT_NOFINGER) {
      // Check for cancel touch
      uint16_t tx, ty;
      if (tft.getTouch(&tx, &ty)) {
        if (isTouched(tx, ty, 10, SCREEN_H - 28, 100, 22)) {
          resetForNewVoter();
          return;
        }
      }
      // Timeout after 30 seconds
      if (millis() - startTime > 30000) {
        drawEnrollDoneScreen(false, "Timeout - no finger");
        currentScreen = SCREEN_ENROLL_DONE;
        return;
      }
      delay(100);
    } else {
      delay(100);
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Image processing failed");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 2: Remove finger
  currentScreen = SCREEN_ENROLL_REMOVE;
  drawEnrollRemoveScreen();

  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(100);
  }

  // Step 3: Place same finger again
  currentScreen = SCREEN_ENROLL_AGAIN;
  drawEnrollAgainScreen();

  p = -1;
  startTime = millis();
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - startTime > 30000) {
        drawEnrollDoneScreen(false, "Timeout - no finger");
        currentScreen = SCREEN_ENROLL_DONE;
        return;
      }
      delay(100);
    } else if (p != FINGERPRINT_OK) {
      delay(100);
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Image processing failed");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 4: Create model
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    drawEnrollDoneScreen(false, "Fingers did not match");
    currentScreen = SCREEN_ENROLL_DONE;
    return;
  }

  // Step 5: Store model
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.printf("Fingerprint stored in slot %d\n", id);
    drawEnrollDoneScreen(true, "");
  } else {
    drawEnrollDoneScreen(false, "Storage failed");
  }
  currentScreen = SCREEN_ENROLL_DONE;
}

// Find next available slot for enrollment
int getNextEnrollSlot() {
  // Check slots 1-126 for an empty one (127 is reserved for admin)
  for (int i = 1; i < ADMIN_FP_ID; i++) {
    uint8_t p = finger.loadModel(i);
    if (p != FINGERPRINT_OK) {
      return i;  // Empty slot
    }
  }
  return -1;  // All full
}

// ═══════════════════════════════════════════════════════════════
//   SERVER-TRIGGERED ENROLLMENT (Browser Admin Panel → ESP32)
// ═══════════════════════════════════════════════════════════════

void pollServerForEnrollment() {
  unsigned long now = millis();
  if (now - lastServerPoll < SERVER_POLL_INTERVAL) return;
  lastServerPoll = now;

  HTTPClient http;
  http.begin(serverUrl("/api/enroll/pending"));
  http.setTimeout(3000);
  int httpCode = http.GET();
  String response = http.getString();
  http.end();

  if (httpCode != 200) return;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return;

  bool hasPending = doc["hasPending"].as<bool>();
  if (!hasPending) return;

  // We have a pending enrollment from the browser!
  serverEnrollId = doc["enrollment"]["id"].as<int>();
  serverEnrollName = doc["enrollment"]["name"].as<String>();
  serverEnrollUserId = doc["enrollment"]["userId"].as<String>();

  Serial.printf("Server enrollment request: %s (ID: %s, enrollment #%d)\n",
                serverEnrollName.c_str(), serverEnrollUserId.c_str(), serverEnrollId);

  // Start the server-triggered enrollment
  currentScreen = SCREEN_SERVER_ENROLL;
  executeServerEnrollment();
}

void drawServerEnrollScreen() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);

  drawCenteredText("ADMIN ENROLLMENT", 12, GOLD_COLOR, 2);
  tft.drawFastHLine(30, 32, SCREEN_W - 60, DIVIDER_COLOR);
  drawCenteredText("Requested from browser", 38, TEXT_MUTED, 1);

  // Voter info card
  tft.fillRoundRect(20, 55, SCREEN_W - 40, 40, 8, CARD_COLOR);
  tft.drawRoundRect(20, 55, SCREEN_W - 40, 40, 8, ACCENT_CYAN);
  drawCenteredText(serverEnrollName.c_str(), 62, TEXT_COLOR, 2);
  String idStr = "ID: " + serverEnrollUserId;
  drawCenteredText(idStr.c_str(), 82, TEXT_DIM, 1);

  // Fingerprint icon
  int cx = SCREEN_W / 2;
  int cy = 140;
  tft.drawCircle(cx, cy, 30, GOLD_COLOR);
  tft.drawCircle(cx, cy, 28, GOLD_COLOR);
  for (int i = -12; i <= 12; i += 5) {
    int halfW = sqrt(max(0, 15 * 15 - i * i));
    tft.drawFastHLine(cx - halfW, cy + i, halfW * 2, TEXT_DIM);
  }
  tft.fillCircle(cx, cy, 3, ACCENT_CYAN);

  drawCenteredText("Place finger on sensor", 182, TEXT_COLOR, 2);
  drawCenteredText("Hold steady...", 204, TEXT_DIM, 1);

  drawButton(10, SCREEN_H - 28, 100, 22, "CANCEL", ERROR_COLOR, TEXT_COLOR);
}

void executeServerEnrollment() {
  // Find next available slot
  int slot = getNextEnrollSlot();
  if (slot < 0) {
    reportEnrollmentComplete(false, -1, "Sensor memory full");
    drawErrorScreen("Sensor memory full!");
    currentScreen = SCREEN_ERROR;
    return;
  }

  enrollId = slot;
  enrollName = serverEnrollName;
  drawServerEnrollScreen();

  Serial.printf("Server enrollment: enrolling '%s' at slot %d\n", serverEnrollName.c_str(), slot);

  unsigned long startTime = millis();
  int p = -1;

  // Step 1: Capture first image
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println("Server enroll: Image taken (1)");
    } else if (p == FINGERPRINT_NOFINGER) {
      // Check for cancel touch
      uint16_t ttx, tty;
      if (tft.getTouch(&ttx, &tty)) {
        if (isTouched(ttx, tty, 10, SCREEN_H - 28, 100, 22)) {
          reportEnrollmentComplete(false, -1, "Cancelled on device");
          resetForNewVoter();
          return;
        }
      }
      if (millis() - startTime > 60000) {
        reportEnrollmentComplete(false, -1, "Timeout - no finger detected");
        drawErrorScreen("Timeout!");
        currentScreen = SCREEN_ERROR;
        return;
      }
      delay(100);
    } else {
      delay(100);
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    reportEnrollmentComplete(false, -1, "Image processing failed");
    drawErrorScreen("Image processing failed");
    currentScreen = SCREEN_ERROR;
    return;
  }

  // Step 2: Remove finger
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);
  drawCenteredText("Step 1 captured!", 80, SUCCESS_COLOR, 2);
  drawCenteredText("Remove finger, then", 120, TEXT_COLOR, 2);
  drawCenteredText("place it again", 142, TEXT_COLOR, 2);

  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(100);
  }

  // Step 3: Place same finger again
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, GOLD_COLOR);
  drawCenteredText("Place SAME finger", 80, TEXT_COLOR, 2);
  drawCenteredText("again on sensor", 105, TEXT_COLOR, 2);
  drawCenteredText("for " + serverEnrollName, 140, ACCENT_CYAN, 1);

  p = -1;
  startTime = millis();
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - startTime > 30000) {
        reportEnrollmentComplete(false, -1, "Timeout on second scan");
        drawErrorScreen("Timeout!");
        currentScreen = SCREEN_ERROR;
        return;
      }
      delay(100);
    } else if (p != FINGERPRINT_OK) {
      delay(100);
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    reportEnrollmentComplete(false, -1, "Image processing failed");
    drawErrorScreen("Image processing failed");
    currentScreen = SCREEN_ERROR;
    return;
  }

  // Step 4: Create model
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    reportEnrollmentComplete(false, -1, "Fingers did not match");
    drawErrorScreen("Fingers did not match!");
    currentScreen = SCREEN_ERROR;
    return;
  }

  // Step 5: Store model
  p = finger.storeModel(slot);
  if (p != FINGERPRINT_OK) {
    reportEnrollmentComplete(false, -1, "Storage failed");
    drawErrorScreen("Storage failed!");
    currentScreen = SCREEN_ERROR;
    return;
  }

  Serial.printf("Server enrollment: Fingerprint stored in slot %d\n", slot);

  // Step 6: Report success to server (server will register the voter)
  reportEnrollmentComplete(true, slot, "");

  // Show success on TFT
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, SUCCESS_COLOR);

  int cx = SCREEN_W / 2;
  tft.fillCircle(cx, 60, 28, SUCCESS_COLOR);
  tft.fillCircle(cx, 60, 24, BG_COLOR);
  tft.fillCircle(cx, 60, 22, SUCCESS_COLOR);
  tft.setTextColor(BG_COLOR); tft.setTextSize(2);
  tft.setCursor(cx - 12, 53); tft.print("OK");

  drawCenteredText("ENROLLED!", 100, SUCCESS_COLOR, 2);
  drawCenteredText(serverEnrollName.c_str(), 125, GOLD_COLOR, 2);
  String fpMsg = "Fingerprint ID: " + String(slot);
  drawCenteredText(fpMsg.c_str(), 155, TEXT_DIM, 1);
  drawCenteredText("Registered via admin panel", 175, TEXT_MUTED, 1);

  drawButton(60, 200, 200, 30, "BACK TO VOTING", PRIMARY_COLOR, TEXT_COLOR);
  currentScreen = SCREEN_ENROLL_DONE;
}

void reportEnrollmentComplete(bool success, int fpId, String errorMsg) {
  HTTPClient http;
  http.begin(serverUrl("/api/enroll/complete"));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["enrollmentId"] = serverEnrollId;
  doc["fingerprintId"] = fpId;
  doc["success"] = success;
  if (!success) {
    doc["error"] = errorMsg;
  }

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    Serial.println("Enrollment completion reported to server");
  } else {
    Serial.printf("Failed to report enrollment: HTTP %d\n", httpCode);
  }
  http.end();

  // Reset server enrollment state
  serverEnrollId = -1;
  serverEnrollName = "";
  serverEnrollUserId = "";
}

// ═══════════════════════════════════════════════════════════════
//   Reset & WiFi (Enhanced Connection Screen)
// ═══════════════════════════════════════════════════════════════

void resetForNewVoter() {
  voterId = "";
  voterName = "";
  voterFingerprintId = -1;
  selectedCandidate = -1;
  txHash = "";
  resultMessage = "";
  currentScreen = SCREEN_FINGERPRINT;
  drawFingerprintScreen();
}

void connectWiFi() {
  tft.fillScreen(BG_COLOR);
  tft.fillRect(0, 0, SCREEN_W, 4, PRIMARY_COLOR);

  drawCenteredText("SECURE E-VOTE", 25, PRIMARY_COLOR, 2);
  tft.drawFastHLine(40, 45, SCREEN_W - 80, DIVIDER_COLOR);
  drawCenteredText("Initializing System...", 55, TEXT_DIM, 1);

  // WiFi connection
  tft.fillRoundRect(20, 75, SCREEN_W - 40, 35, 8, CARD_COLOR);
  tft.setTextSize(1);
  tft.setTextColor(TEXT_MUTED);
  tft.setCursor(30, 80);
  tft.print("WiFi: ");
  tft.setTextColor(ACCENT_CYAN);
  tft.print(WIFI_SSID);
  tft.setTextColor(TEXT_MUTED);
  tft.setCursor(30, 95);
  tft.print("Connecting");

  // Force station mode and clear any stale connections
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  Serial.println("NOTE: ESP32 only supports 2.4GHz WiFi!");

  int maxAttempts = 3;
  bool connected = false;

  for (int attempt = 1; attempt <= maxAttempts && !connected; attempt++) {
    Serial.printf("Attempt %d/%d...\n", attempt, maxAttempts);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Wait up to 15 seconds per attempt
    unsigned long startAttempt = millis();
    int dots = 0;

    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
      delay(500);
      Serial.printf("  WiFi status: %d\n", WiFi.status());
      tft.setCursor(90 + dots * 6, 95);
      tft.setTextColor(PRIMARY_COLOR);
      tft.print(".");
      dots++;
      if (dots > 18) {
        dots = 0;
        tft.fillRect(90, 95, 130, 10, CARD_COLOR);
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    } else {
      Serial.printf("  Attempt %d failed. Status: %d\n", attempt, WiFi.status());
      WiFi.disconnect(true);
      delay(1000);

      // Show retry on screen
      tft.fillRect(30, 95, 250, 10, CARD_COLOR);
      tft.setCursor(30, 95);
      tft.setTextColor(WARNING_COLOR);
      String retryMsg = "Retry " + String(attempt) + "/" + String(maxAttempts) + "...";
      tft.print(retryMsg.c_str());
    }
  }

  if (!connected) {
    Serial.println("WiFi FAILED after all attempts!");
    Serial.println("Check: SSID, password, and 2.4GHz band");

    tft.fillRoundRect(20, 75, SCREEN_W - 40, 50, 8, CARD_COLOR);
    tft.drawRoundRect(20, 75, SCREEN_W - 40, 50, 8, ERROR_COLOR);
    tft.setTextColor(ERROR_COLOR);
    tft.setCursor(30, 80);
    tft.print("WiFi FAILED!");
    tft.setTextColor(TEXT_DIM);
    tft.setCursor(30, 95);
    tft.print("Check SSID/password");
    tft.setCursor(30, 108);
    tft.setTextColor(WARNING_COLOR);
    tft.print("ESP32 = 2.4GHz ONLY!");

    drawCenteredText("Rebooting in 5s...", 145, TEXT_MUTED, 1);
    delay(5000);
    ESP.restart();
    return;
  }

  // Connected!
  Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

  tft.fillRoundRect(20, 75, SCREEN_W - 40, 35, 8, CARD_COLOR);
  tft.drawRoundRect(20, 75, SCREEN_W - 40, 35, 8, SUCCESS_COLOR);
  tft.setTextSize(1);
  tft.setTextColor(SUCCESS_COLOR);
  tft.setCursor(30, 80);
  tft.print("WiFi Connected!");
  tft.setTextColor(TEXT_DIM);
  tft.setCursor(30, 95);
  tft.print("IP: ");
  tft.setTextColor(TEXT_COLOR);
  tft.print(WiFi.localIP().toString().c_str());

  // Server info
  tft.fillRoundRect(20, 120, SCREEN_W - 40, 25, 8, CARD_COLOR);
  tft.setTextSize(1);
  tft.setTextColor(TEXT_MUTED);
  tft.setCursor(30, 128);
  tft.print("Server: ");
  tft.setTextColor(ACCENT_CYAN);
  tft.print(SERVER_IP);
  tft.print(":");
  tft.print(SERVER_PORT);

  // Fingerprint sensor init
  tft.fillRoundRect(20, 155, SCREEN_W - 40, 25, 8, CARD_COLOR);
  drawCenteredText("Init fingerprint sensor...", 162, TEXT_DIM, 1);
  initFingerprint();

  if (finger.verifyPassword()) {
    tft.fillRoundRect(20, 155, SCREEN_W - 40, 25, 8, CARD_COLOR);
    tft.drawRoundRect(20, 155, SCREEN_W - 40, 25, 8, SUCCESS_COLOR);
    drawCenteredText("Fingerprint: OK", 162, SUCCESS_COLOR, 1);
  } else {
    tft.fillRoundRect(20, 155, SCREEN_W - 40, 25, 8, CARD_COLOR);
    tft.drawRoundRect(20, 155, SCREEN_W - 40, 25, 8, ERROR_COLOR);
    drawCenteredText("Fingerprint: FAIL", 162, ERROR_COLOR, 1);
  }

  // System ready
  tft.fillRoundRect(40, 195, SCREEN_W - 80, 30, 8, SUCCESS_COLOR);
  drawCenteredText("SYSTEM READY", 203, TEXT_COLOR, 2);

  delay(2000);
}

// ═══════════════════════════════════════════════════════════════
//   SETUP & LOOP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Blockchain Voting System (Fingerprint Auth) ===\n");

  // Init display
  tft.init();
  tft.setRotation(1);  // Landscape
  tft.fillScreen(BG_COLOR);

  // Calibrate touch (adjust these values for your specific display)
  // uint16_t calData[5] = {275, 3620, 264, 3532, 1};
  // tft.setTouch(calData);

  // Connect WiFi
  connectWiFi();

  // Check if admin fingerprint exists
  adminExists = checkAdminExists();
  Serial.printf("Admin fingerprint: %s\n", adminExists ? "EXISTS" : "NOT FOUND");

  if (!adminExists) {
    // First time setup — enroll admin fingerprint
    Serial.println("No admin found! Starting admin setup...");
    enrollAdmin();
    return;
  }

  // Show fingerprint scan screen
  currentScreen = SCREEN_FINGERPRINT;
  drawFingerprintScreen();
}

unsigned long lastTouchTime = 0;
const unsigned long DEBOUNCE_MS = 250;

void loop() {
  // Reconnect WiFi if connection dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost! Reconnecting...");
    tft.fillScreen(BG_COLOR);
    tft.fillRect(0, 0, SCREEN_W, 4, ERROR_COLOR);
    drawCenteredText("WiFi Lost!", 80, ERROR_COLOR, 2);
    drawCenteredText("Reconnecting...", 110, TEXT_DIM, 1);
    WiFi.reconnect();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi reconnected!");
      switch (currentScreen) {
        case SCREEN_FINGERPRINT:   drawFingerprintScreen(); break;
        case SCREEN_WELCOME:       drawWelcomeScreen(); break;
        case SCREEN_CANDIDATES:    drawCandidateScreen(); break;
        case SCREEN_CONFIRM:       drawConfirmScreen(); break;
        case SCREEN_RESULT:        fetchAndDrawResults(); break;
        case SCREEN_ALREADY_VOTED: drawAlreadyVotedScreen(); break;
        case SCREEN_ERROR:         drawErrorScreen(resultMessage); break;
        default: drawFingerprintScreen(); break;
      }
    } else {
      drawErrorScreen("WiFi connection failed!");
      currentScreen = SCREEN_ERROR;
    }
    return;
  }

  // ─── Fingerprint scanning (non-blocking) ───
  if (currentScreen == SCREEN_FINGERPRINT) {
    // Animate the pulse
    animateFingerprintPulse();

    // Poll server for browser-triggered enrollments
    pollServerForEnrollment();

    // Check fingerprint every 200ms
    unsigned long now = millis();
    if (now - lastScanTime >= 200) {
      lastScanTime = now;
      int fpId = checkFingerprint();
      if (fpId > 0) {
        if (fpId == ADMIN_FP_ID) {
          // Admin fingerprint detected — show admin menu
          Serial.println("Admin authenticated!");
          currentScreen = SCREEN_ADMIN_MENU;
          drawAdminMenu();
        } else {
          // Regular voter fingerprint
          authenticateFingerprint(fpId);
        }
        return;
      } else if (fpId == -2) {
        // Finger detected but not recognized
        tft.fillRect(0, 218, SCREEN_W, 22, BG_COLOR);
        drawCenteredText("Not recognized! Try again.", 222, ERROR_COLOR, 1);
        delay(1500);
        drawFingerprintScreen();
      }
    }
    return;
  }

  // ─── Welcome screen auto-advance ───
  if (currentScreen == SCREEN_WELCOME) {
    if (millis() - welcomeShownTime > 2500) {
      fetchCandidates();
    }
    return;
  }

  // ─── Result screen live refresh ───
  if (currentScreen == SCREEN_RESULT) {
    refreshResults();

    // Auto-return to fingerprint after 15 seconds
    if (millis() - resultShownTime > 15000) {
      resetForNewVoter();
      return;
    }
  }

  // ─── Touch handling ───
  uint16_t tx = 0, ty = 0;

  if (tft.getTouch(&tx, &ty)) {
    unsigned long now = millis();
    if (now - lastTouchTime < DEBOUNCE_MS) return;
    lastTouchTime = now;

    Serial.printf("Touch: x=%d, y=%d, screen=%d\n", tx, ty, currentScreen);

    switch (currentScreen) {
      case SCREEN_CANDIDATES:
        handleCandidateTouch(tx, ty);
        break;
      case SCREEN_CONFIRM:
        handleConfirmTouch(tx, ty);
        break;
      case SCREEN_RESULT:
        handleResultTouch(tx, ty);
        break;
      case SCREEN_ALREADY_VOTED:
        handleAlreadyVotedTouch(tx, ty);
        break;
      case SCREEN_ERROR:
        handleErrorTouch(tx, ty);
        break;
      case SCREEN_ENROLL_DONE:
        // "Back to voting" or "Back to admin"
        if (isTouched(tx, ty, 60, 200, 200, 30)) {
          currentScreen = SCREEN_ADMIN_MENU;
          drawAdminMenu();
        }
        break;
      case SCREEN_ADMIN_MENU:
        handleAdminMenuTouch(tx, ty);
        break;
      case SCREEN_ADMIN_NAME_ENTRY:
        handleNameEntryTouch(tx, ty);
        break;
      default:
        break;
    }
  }
}
