/*
 * Attendance Kiosk — ESP32 + 2.4" ILI9341 TFT (XPT2046 touch)
 *
 * Pin Wiring:
 * ┌──────────────┬───────┬─────────────────────────────────┐
 * │ Display Pin  │ ESP32 │ Notes                           │
 * ├──────────────┼───────┼─────────────────────────────────┤
 * │ VCC          │ 3.3V  │                                 │
 * │ GND          │ GND   │                                 │
 * │ SCK          │ 18    │ SPI CLK (shared with touch)     │
 * │ SDI (MOSI)   │ 23    │ SPI MOSI (shared with touch)    │
 * │ SDO (MISO)   │ 19    │ SPI MISO (shared with touch)    │
 * │ DC           │ 21    │ Data/Command select             │
 * │ CS           │ 4     │ TFT chip select                 │
 * │ RESET        │ 22    │ Display reset (NOT GPIO0!)      │
 * │ LED          │ 15    │ Backlight (HIGH = ON)           │
 * ├──────────────┼───────┼─────────────────────────────────┤
 * │ T_CLK        │ 18    │ shared SPI CLK                  │
 * │ T_DIN        │ 23    │ shared SPI MOSI                 │
 * │ T_DO         │ 19    │ shared SPI MISO                 │
 * │ T_CS         │ 5     │ Touch chip select               │
 * │ T_IRQ        │ 36    │ Touch interrupt (input only)    │
 * └──────────────┴───────┴─────────────────────────────────┘
 *
 * Odoo 19 JSON-2 API:  POST /json/2/<model>/<method>
 * Hold screen at boot → re-runs touch calibration
 */

// ─── Debug: 1 = enable, 0 = strip all debug prints from binary ───────────────
#define DEBUG 1
#if DEBUG
  #define DBG(...)      Serial.print(__VA_ARGS__)
  #define DBGLN(...)    Serial.println(__VA_ARGS__)
  #define DBGF(...)     Serial.printf(__VA_ARGS__)
  #define DBG_SEC(s)    Serial.println("\n[" s "]")
#else
  #define DBG(...)
  #define DBGLN(...)
  #define DBGF(...)
  #define DBG_SEC(s)
#endif

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <time.h>

// ─── Config ───────────────────────────────────────────────────────────────────
#define WIFI_SSID      "GRIDFLOW_2.4G"
#define WIFI_PASSWORD  "Gridflow@2024"
#define ODOO_HOST      "192.168.100.204"
#define ODOO_PORT      8069
#define ODOO_APIKEY    "fded62823e989fa8ff80f53304b9817b65ddf796"
#define NTP_SERVER     "pool.ntp.org"
#define TZ_OFFSET_SEC  19800   // IST = UTC + 5:30

#define TFT_BL_PIN   15
#define BUZZER_PIN   25

// ─── Objects ──────────────────────────────────────────────────────────────────
TFT_eSPI   tft;
Preferences prefs;

// ─── Touch calibration defaults (update after first calibration run) ──────────
uint16_t g_cal[5] = { 339, 3498, 380, 3507, 7 };

// ─── Screen dimensions (landscape 320×240) ────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  240

// ─── Colours (RGB565) ─────────────────────────────────────────────────────────
#define CLR_BG        TFT_BLACK
#define CLR_HDR       TFT_NAVY
#define CLR_TEXT      TFT_WHITE
#define CLR_SUB       TFT_LIGHTGREY
#define CLR_IN        TFT_GREEN
#define CLR_OUT       0xFD60   // Orange
#define CLR_ROW_ALT   0x2104   // Dark grey for alternating rows
#define CLR_ROW_SEL   0x0B4D   // Blue highlight on tap
#define CLR_BTN       0x4208
#define CLR_BTN_HI    TFT_SILVER
#define CLR_OK        TFT_GREEN
#define CLR_ERR       TFT_RED
#define CLR_INFO      0xFD60   // Orange

// ─── UI layout (all values in pixels, landscape) ──────────────────────────────
// HDR  : 0 – 39   (40px)
// STA  : 40 – 55  (16px)  ← status bar
// COL  : 56 – 67  (12px)  ← column headers
// ROWS : 68 – 207 (5 × 28px = 140px)
// NAV  : 208 – 239 (32px) ← Prev / page indicator / Next
#define HDR_Y   0
#define HDR_H   40
#define STA_Y   40
#define STA_H   16
#define COL_Y   56
#define COL_H   12
#define ROW_Y   68
#define ROW_H   28
#define ROWS_PP 5                              // rows per page
#define NAV_Y   (ROW_Y + ROW_H * ROWS_PP)    // 208
#define NAV_H   32

// Refresh button — inside header, right side
#define BTN_REF_X   248
#define BTN_REF_Y     6
#define BTN_REF_W    66
#define BTN_REF_H    28

// Prev / Next — inside nav bar
#define BTN_PRV_X    4
#define BTN_PRV_Y   (NAV_Y + 4)
#define BTN_PRV_W    70
#define BTN_PRV_H    24

#define BTN_NXT_X   (SCREEN_W - 74)
#define BTN_NXT_Y   (NAV_Y + 4)
#define BTN_NXT_W    70
#define BTN_NXT_H    24

// ─── Employee data ────────────────────────────────────────────────────────────
#define MAX_EMP  60

struct Employee {
  int  id;
  char name[32];
  int  attendId;    // hr.attendance record id for today; 0 = no record
  char checkIn[6];  // "HH:MM" IST, or "--:--"
  bool present;     // checked in but not yet checked out
};

Employee g_emp[MAX_EMP];
int  g_empCount = 0;
int  g_page     = 0;
bool g_wifiOk   = false;
bool g_ntpOk    = false;
bool g_busy     = false;

// ─── Forward declarations ─────────────────────────────────────────────────────
void loadCalibration();
void saveCalibration();
void runCalibration();
bool connectWiFi();
bool syncNTP();
void fetchAll();
bool fetchEmployees();
bool fetchTodayAttendance();
bool doCheckIn(int idx);
bool doCheckOut(int idx);
String odooPost(const String& endpoint, const String& body);
String getCurrentUTCStr();
String getTodayUTCDate();
String extractTime(const String& dt);
bool inRect(int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t px, int16_t py);
void drawAll();
void drawHeader();
void drawStatus(const char* msg, uint16_t clr);
void drawColHeaders();
void drawRows();
void drawNav();
void drawRefBtn(bool pressed);
void drawPrvBtn(bool pressed);
void drawNxtBtn(bool pressed);
void flashRow(int pageIdx, uint16_t clr);
void beep(int count, int onMs, int offMs);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  DBG_SEC("BOOT");
  DBGF("Free heap: %u\n", ESP.getFreeHeap());

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Backlight first — without it screen is always black
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  DBGLN("Backlight ON");

  // TFT init
  DBG_SEC("TFT");
  tft.init();
  tft.setRotation(3);
  DBGF("Size: %d x %d\n", tft.width(), tft.height());
  tft.fillScreen(CLR_BG);

  // Touch calibration — 3-second window shown on screen, hold to enter cal
  DBG_SEC("TOUCH");
  loadCalibration();
  tft.setTouch(g_cal);
  {
    const int BX = 20, BY = SCREEN_H/2 + 20, BW = SCREEN_W - 40, BH = 12;
    const int WIN = 3000, STEP = 50;
    tft.fillScreen(CLR_BG);
    tft.setTextColor(CLR_TEXT); tft.setTextSize(2); tft.setTextDatum(MC_DATUM);
    tft.drawString("Hold to Calibrate", SCREEN_W/2, SCREEN_H/2 - 10);
    tft.setTextSize(1); tft.setTextColor(CLR_SUB);
    tft.drawString("or wait to continue...", SCREEN_W/2, SCREEN_H/2 + 10);
    tft.setTextDatum(TL_DATUM);
    tft.drawRoundRect(BX, BY, BW, BH, 4, CLR_SUB);
    bool doCalibrate = false;
    for (int elapsed = 0; elapsed < WIN; elapsed += STEP) {
      uint16_t tx, ty;
      if (tft.getTouch(&tx, &ty)) { doCalibrate = true; break; }
      int filled = BW - (BW * elapsed / WIN);
      tft.fillRoundRect(BX + 2, BY + 2, (filled > 4 ? filled - 4 : 0), BH - 4, 3, CLR_INFO);
      delay(STEP);
    }
    tft.fillScreen(CLR_BG);
    if (doCalibrate) { DBGLN("Calibrating..."); runCalibration(); }
    else              { DBGLN("Skipped calibration"); }
  }

  // Draw initial UI shell
  drawAll();
  drawStatus("Connecting to WiFi...", CLR_INFO);

  // WiFi
  DBG_SEC("WIFI");
  if (!connectWiFi()) {
    drawStatus("WiFi FAILED — check credentials", CLR_ERR);
    return;
  }

  // NTP
  DBG_SEC("NTP");
  drawStatus("Syncing time (NTP)...", CLR_INFO);
  g_ntpOk = syncNTP();
  if (!g_ntpOk) {
    drawStatus("NTP failed — times may be off", CLR_ERR);
    delay(1500);
  }

  // Load employee list + today's attendance
  drawStatus("Loading employees...", CLR_INFO);
  fetchAll();

  beep(1, 100, 0);   // single short beep — ready
  DBG_SEC("SETUP DONE");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return;

  DBGF("[TOUCH] %u,%u\n", tx, ty);

  int totalPages = max(1, (g_empCount + ROWS_PP - 1) / ROWS_PP);

  // ── Refresh ────────────────────────────────────────────────────────────────
  if (inRect(BTN_REF_X, BTN_REF_Y, BTN_REF_W, BTN_REF_H, tx, ty)) {
    if (g_busy) goto debounce;
    drawRefBtn(true); delay(100); drawRefBtn(false);
    if (g_wifiOk) {
      drawStatus("Refreshing...", CLR_INFO);
      fetchAll();
    }
    goto debounce;
  }

  // ── Previous page ──────────────────────────────────────────────────────────
  if (inRect(BTN_PRV_X, BTN_PRV_Y, BTN_PRV_W, BTN_PRV_H, tx, ty)) {
    if (g_page > 0) {
      drawPrvBtn(true); delay(100); drawPrvBtn(false);
      g_page--;
      drawRows();
      drawNav();
    }
    goto debounce;
  }

  // ── Next page ──────────────────────────────────────────────────────────────
  if (inRect(BTN_NXT_X, BTN_NXT_Y, BTN_NXT_W, BTN_NXT_H, tx, ty)) {
    if (g_page < totalPages - 1) {
      drawNxtBtn(true); delay(100); drawNxtBtn(false);
      g_page++;
      drawRows();
      drawNav();
    }
    goto debounce;
  }

  // ── Row tap → check-in / check-out ────────────────────────────────────────
  if (ty >= ROW_Y && ty < NAV_Y && !g_busy) {
    int r   = (ty - ROW_Y) / ROW_H;
    int idx = g_page * ROWS_PP + r;
    if (idx < g_empCount) {
      DBGF("[TAP] row=%d  emp[%d] %s  present=%d  attendId=%d\n",
           r, idx, g_emp[idx].name, g_emp[idx].present, g_emp[idx].attendId);

      flashRow(r, CLR_ROW_SEL);

      if (g_emp[idx].present) {
        // Checked in → check out
        drawStatus("Checking out...", CLR_INFO);
        if (doCheckOut(idx)) {
          g_emp[idx].present = false;
          beep(1, 200, 0);   // single long beep — checked out
          char msg[48];
          snprintf(msg, sizeof(msg), "Checked out: %.24s", g_emp[idx].name);
          drawStatus(msg, CLR_OK);
        } else {
          // Resync in case Odoo state differs from kiosk
          fetchTodayAttendance();
          drawStatus("Check-out FAILED — synced state", CLR_ERR);
        }
      } else {
        // Not present → check in (creates a new record)
        g_emp[idx].attendId = 0;
        drawStatus("Checking in...", CLR_INFO);
        if (doCheckIn(idx)) {
          g_emp[idx].present = true;
          beep(2, 80, 60);   // double beep — checked in
          char msg[48];
          snprintf(msg, sizeof(msg), "Checked in: %.25s", g_emp[idx].name);
          drawStatus(msg, CLR_OK);
        } else {
          // Odoo may have an open record we don't know about — resync
          fetchTodayAttendance();
          drawStatus("Check-in FAILED — synced state", CLR_ERR);
        }
      }
      drawRows();
    }
  }

debounce:
  while (tft.getTouch(&tx, &ty)) delay(10);
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
bool connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); DBG(".");
  }
  DBGLN();
  g_wifiOk = (WiFi.status() == WL_CONNECTED);
  if (g_wifiOk) DBGF("IP: %s  RSSI: %d dBm\n",
                       WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else           DBGLN("WiFi FAILED");
  return g_wifiOk;
}

// ─── NTP ──────────────────────────────────────────────────────────────────────
bool syncNTP() {
  configTime(0, 0, NTP_SERVER);  // UTC time
  struct tm t;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&t) && t.tm_year > 100) {
      DBGF("NTP UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
      return true;
    }
    delay(500);
  }
  DBGLN("NTP sync timeout");
  return false;
}

// ─── Fetch everything ─────────────────────────────────────────────────────────
void fetchAll() {
  if (!g_wifiOk) return;
  g_busy = true;
  bool ok = fetchEmployees();
  if (ok) {
    fetchTodayAttendance();
    drawRows();
    drawNav();
    drawStatus("Ready — tap a name to check in/out", CLR_OK);
  } else {
    drawStatus("Failed to load employees", CLR_ERR);
  }
  g_busy = false;
}

// ─── Fetch hr.employee list ───────────────────────────────────────────────────
bool fetchEmployees() {
  DBG_SEC("FETCH EMPLOYEES");
  String resp = odooPost("/json/2/hr.employee/search_read",
    "{\"domain\":[[\"active\",\"=\",true]],"
    "\"fields\":[\"id\",\"name\"],"
    "\"limit\":60,"
    "\"order\":\"name asc\"}");

  if (resp.isEmpty()) { DBGLN("Empty response"); return false; }

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    DBGLN("JSON parse error (employees)");
    return false;
  }
  if (!doc.is<JsonArray>()) {
    DBGLN("Expected array");
    return false;
  }

  g_empCount = 0;
  for (JsonObject e : doc.as<JsonArray>()) {
    if (g_empCount >= MAX_EMP) break;
    g_emp[g_empCount].id       = e["id"] | 0;
    g_emp[g_empCount].attendId = 0;
    g_emp[g_empCount].present  = false;
    strlcpy(g_emp[g_empCount].checkIn, "--:--", 6);
    strlcpy(g_emp[g_empCount].name, e["name"] | "", 32);
    DBGF("  [%d] id=%d  %s\n",
         g_empCount, g_emp[g_empCount].id, g_emp[g_empCount].name);
    g_empCount++;
  }
  DBGF("Loaded %d employees\n", g_empCount);
  return (g_empCount > 0);
}

// ─── Fetch today's hr.attendance records ─────────────────────────────────────
bool fetchTodayAttendance() {
  DBG_SEC("FETCH ATTENDANCE");
  String today = getTodayUTCDate();
  DBGF("Today UTC: %s\n", today.c_str());

  // Reset all attendance state before re-populating
  for (int i = 0; i < g_empCount; i++) {
    g_emp[i].attendId = 0;
    g_emp[i].present  = false;
    strlcpy(g_emp[i].checkIn, "--:--", 6);
  }

  String body =
    "{\"domain\":[[\"check_in\",\">=\",\"" + today + " 00:00:00\"]],"
    "\"fields\":[\"id\",\"employee_id\",\"check_in\",\"check_out\"],"
    "\"order\":\"check_in desc\","
    "\"limit\":200}";

  String resp = odooPost("/json/2/hr.attendance/search_read", body);
  if (resp.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    DBGLN("JSON parse error (attendance)");
    return false;
  }
  if (!doc.is<JsonArray>()) return false;

  // Records arrive newest-first (order: check_in desc).
  // For each employee we keep the first (newest) record we see.
  // If that record is already open (present), we never overwrite it
  // with an older closed record — this prevents stale attendId mismatches.
  for (JsonObject rec : doc.as<JsonArray>()) {
    int empId = rec["employee_id"][0] | 0;
    for (int i = 0; i < g_empCount; i++) {
      if (g_emp[i].id != empId) continue;

      // Already have an open record for this employee — skip older ones
      if (g_emp[i].present) break;

      JsonVariant coVar    = rec["check_out"];
      bool hasCheckout     = !(coVar.is<bool>() || coVar.isNull());

      // Skip closed records if we already have any record stored
      // (open records always take priority over closed ones)
      if (g_emp[i].attendId != 0 && hasCheckout) break;

      g_emp[i].attendId = rec["id"] | 0;
      g_emp[i].present  = !hasCheckout;

      // Convert UTC check_in → IST display time
      String ci = rec["check_in"] | "";
      String t  = extractTime(ci);
      if (t != "--:--") {
        int h = t.substring(0, 2).toInt();
        int m = t.substring(3, 5).toInt();
        m += 30; if (m >= 60) { m -= 60; h++; }
        h = (h + 5) % 24;
        snprintf(g_emp[i].checkIn, 6, "%02d:%02d", h, m);
      }

      DBGF("  emp[%d] %s  attendId=%d  in=%s  present=%d\n",
           i, g_emp[i].name, g_emp[i].attendId,
           g_emp[i].checkIn, g_emp[i].present);
      break;
    }
  }
  return true;
}

// ─── Check IN ─────────────────────────────────────────────────────────────────
bool doCheckIn(int idx) {
  DBG_SEC("CHECK IN");
  DBGF("Employee: %s  id=%d\n", g_emp[idx].name, g_emp[idx].id);

  String utc  = getCurrentUTCStr();
  String body = "{\"vals_list\":[{\"employee_id\":" + String(g_emp[idx].id) +
                ",\"check_in\":\"" + utc + "\"}]}";

  String resp = odooPost("/json/2/hr.attendance/create", body);
  if (resp.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

  // Odoo returns new record ID as integer (or array of one)
  int newId = 0;
  if (doc.is<int>())       newId = doc.as<int>();
  else if (doc.is<JsonArray>()) newId = doc[0] | 0;

  DBGF("New attendId: %d\n", newId);
  if (newId <= 0) return false;

  g_emp[idx].attendId = newId;

  // Set IST display time from current time
  time_t now = time(nullptr);
  time_t ist = now + TZ_OFFSET_SEC;
  struct tm t;
  gmtime_r(&ist, &t);
  snprintf(g_emp[idx].checkIn, 6, "%02d:%02d", t.tm_hour, t.tm_min);
  return true;
}

// ─── Check OUT ────────────────────────────────────────────────────────────────
bool doCheckOut(int idx) {
  DBG_SEC("CHECK OUT");
  DBGF("Employee: %s  attendId=%d\n", g_emp[idx].name, g_emp[idx].attendId);

  if (g_emp[idx].attendId == 0) {
    DBGLN("No attendId — cannot write checkout");
    return false;
  }

  String utc  = getCurrentUTCStr();
  String body = "{\"ids\":[" + String(g_emp[idx].attendId) +
                "],\"vals\":{\"check_out\":\"" + utc + "\"}}";

  String resp = odooPost("/json/2/hr.attendance/write", body);
  if (resp.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

  bool ok = doc.as<bool>();
  DBGF("write result: %s\n", ok ? "true" : "false");
  return ok;
}

// ─── Shared Odoo HTTP helper ──────────────────────────────────────────────────
String odooPost(const String& endpoint, const String& body) {
  HTTPClient http;
  String url = "http://" + String(ODOO_HOST) + ":" +
               String(ODOO_PORT) + endpoint;
  DBGF("POST %s\n", url.c_str());
  DBGF("Body: %s\n", body.c_str());

  http.begin(url);
  http.addHeader("Authorization", String("bearer ") + ODOO_APIKEY);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int code = http.POST(body);
  DBGF("HTTP %d\n", code);

  String result = "";
  if (code == 200) {
    result = http.getString();
    DBGF("Resp: %s\n", result.c_str());
  } else {
    DBGF("Error: %s\n", http.getString().c_str());
  }
  http.end();
  return result;
}

// ─── Time helpers ─────────────────────────────────────────────────────────────
String getCurrentUTCStr() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String getTodayUTCDate() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday);
  return String(buf);
}

String extractTime(const String& dt) {
  if (dt.length() < 16) return "--:--";
  return dt.substring(11, 16);
}

// ─── Touch calibration ────────────────────────────────────────────────────────
// Rotation the calibration was saved for — clear NVS if this changes
#define CAL_ROTATION  3

void loadCalibration() {
  prefs.begin("touch", false);
  uint8_t savedRot = prefs.getUChar("rot", 255);
  if (savedRot == CAL_ROTATION && prefs.getBytesLength("cal") == sizeof(g_cal)) {
    prefs.getBytes("cal", g_cal, sizeof(g_cal));
    DBGF("Cal loaded (rot=%d): {%u,%u,%u,%u,%u}\n",
         savedRot, g_cal[0], g_cal[1], g_cal[2], g_cal[3], g_cal[4]);
  } else {
    // Rotation changed or no saved cal — wipe stale data, force recal at boot
    prefs.clear();
    DBGF("No valid cal for rotation %d — will recalibrate\n", CAL_ROTATION);
  }
  prefs.end();
}

void saveCalibration() {
  prefs.begin("touch", false);
  prefs.putUChar("rot", CAL_ROTATION);
  prefs.putBytes("cal", g_cal, sizeof(g_cal));
  prefs.end();
  DBGF("Cal saved (rot=%d): {%u,%u,%u,%u,%u}\n",
       CAL_ROTATION, g_cal[0], g_cal[1], g_cal[2], g_cal[3], g_cal[4]);
}

void runCalibration() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch Calibration", SCREEN_W/2, SCREEN_H/2 - 20);
  tft.setTextSize(1);
  tft.drawString("Tap each corner marker", SCREEN_W/2, SCREEN_H/2 + 10);
  tft.setTextDatum(TL_DATUM);
  delay(1500);
  tft.calibrateTouch(g_cal, TFT_WHITE, TFT_RED, 15);
  tft.setTouch(g_cal);
  saveCalibration();
  DBGF("Cal: {%u,%u,%u,%u,%u}\n",
       g_cal[0], g_cal[1], g_cal[2], g_cal[3], g_cal[4]);
  tft.fillScreen(CLR_BG);
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────
void drawAll() {
  tft.fillScreen(CLR_BG);
  drawHeader();
  drawColHeaders();
  drawRows();
  drawNav();
}

void drawHeader() {
  tft.fillRect(0, HDR_Y, SCREEN_W, HDR_H, CLR_HDR);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Odoo 19 Attendance", 8, HDR_Y + HDR_H/2);
  tft.setTextDatum(TL_DATUM);
  drawRefBtn(false);
}

void drawStatus(const char* msg, uint16_t clr) {
  tft.fillRect(0, STA_Y, SCREEN_W, STA_H, CLR_BG);
  tft.setTextSize(1);
  tft.setTextColor(clr);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(msg, 4, STA_Y + STA_H/2);
  tft.setTextDatum(TL_DATUM);
}

void drawColHeaders() {
  tft.fillRect(0, COL_Y, SCREEN_W, COL_H, 0x2965);
  tft.setTextSize(1);
  tft.setTextColor(CLR_SUB);
  tft.drawString("Employee",  6,   COL_Y + 2);
  tft.drawString("In (IST)", 210,  COL_Y + 2);
  tft.drawString("Action",   268,  COL_Y + 2);
}

void drawRows() {
  tft.fillRect(0, ROW_Y, SCREEN_W, ROW_H * ROWS_PP, CLR_BG);

  int start = g_page * ROWS_PP;
  int end   = min(start + ROWS_PP, g_empCount);

  if (g_empCount == 0) {
    tft.setTextColor(CLR_SUB);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("No employees loaded", SCREEN_W/2, ROW_Y + 70);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  for (int i = start; i < end; i++) {
    int r   = i - start;
    int y   = ROW_Y + r * ROW_H;
    int mid = y + ROW_H/2;

    // Row background (alternating)
    tft.fillRect(0, y, SCREEN_W, ROW_H - 1, (r % 2) ? CLR_ROW_ALT : CLR_BG);

    // Employee name (max 20 chars)
    char name[21];
    strlcpy(name, g_emp[i].name, 21);
    tft.setTextColor(CLR_TEXT);
    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(name, 6, mid);

    // Check-in time (IST)
    if (g_emp[i].attendId != 0) {
      tft.setTextColor(CLR_IN);
      tft.drawString(g_emp[i].checkIn, 210, mid);
    }

    // Action badge
    if (g_emp[i].present) {
      // Checked in — show "CHECK OUT" badge in orange
      tft.fillRoundRect(263, y + ROW_H/2 - 7, 52, 14, 3, CLR_OUT);
      tft.setTextColor(TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("CHECK OUT", 289, mid);
    } else {
      // Not present → CHECK IN
      tft.fillRoundRect(263, y + ROW_H/2 - 7, 52, 14, 3, 0x0329);
      tft.setTextColor(CLR_IN);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("CHECK IN", 289, mid);
    }
    tft.setTextDatum(TL_DATUM);
  }
}

void drawNav() {
  tft.fillRect(0, NAV_Y, SCREEN_W, NAV_H, 0x1082);

  int totalPages = max(1, (g_empCount + ROWS_PP - 1) / ROWS_PP);

  // Prev button
  bool hasPrev = (g_page > 0);
  tft.fillRoundRect(BTN_PRV_X, BTN_PRV_Y, BTN_PRV_W, BTN_PRV_H, 5,
                    hasPrev ? CLR_BTN : 0x2104);
  tft.setTextColor(hasPrev ? CLR_TEXT : CLR_SUB);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("< Prev", BTN_PRV_X + BTN_PRV_W/2, BTN_PRV_Y + BTN_PRV_H/2);

  // Page indicator
  char pg[12];
  snprintf(pg, sizeof(pg), "%d / %d", g_page + 1, totalPages);
  tft.setTextColor(CLR_SUB);
  tft.drawString(pg, SCREEN_W/2, NAV_Y + NAV_H/2);

  // Next button
  bool hasNext = (g_page < totalPages - 1);
  tft.fillRoundRect(BTN_NXT_X, BTN_NXT_Y, BTN_NXT_W, BTN_NXT_H, 5,
                    hasNext ? CLR_BTN : 0x2104);
  tft.setTextColor(hasNext ? CLR_TEXT : CLR_SUB);
  tft.drawString("Next >", BTN_NXT_X + BTN_NXT_W/2, BTN_NXT_Y + BTN_NXT_H/2);

  tft.setTextDatum(TL_DATUM);
}

void drawRefBtn(bool pressed) {
  tft.fillRoundRect(BTN_REF_X, BTN_REF_Y, BTN_REF_W, BTN_REF_H, 5,
                    pressed ? CLR_BTN_HI : 0x2965);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Refresh", BTN_REF_X + BTN_REF_W/2, BTN_REF_Y + BTN_REF_H/2);
  tft.setTextDatum(TL_DATUM);
}

void drawPrvBtn(bool pressed) {
  tft.fillRoundRect(BTN_PRV_X, BTN_PRV_Y, BTN_PRV_W, BTN_PRV_H, 5,
                    pressed ? CLR_BTN_HI : CLR_BTN);
  tft.setTextColor(CLR_TEXT);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("< Prev", BTN_PRV_X + BTN_PRV_W/2, BTN_PRV_Y + BTN_PRV_H/2);
  tft.setTextDatum(TL_DATUM);
}

void drawNxtBtn(bool pressed) {
  tft.fillRoundRect(BTN_NXT_X, BTN_NXT_Y, BTN_NXT_W, BTN_NXT_H, 5,
                    pressed ? CLR_BTN_HI : CLR_BTN);
  tft.setTextColor(CLR_TEXT);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Next >", BTN_NXT_X + BTN_NXT_W/2, BTN_NXT_Y + BTN_NXT_H/2);
  tft.setTextDatum(TL_DATUM);
}

void flashRow(int r, uint16_t clr) {
  int y = ROW_Y + r * ROW_H;
  tft.fillRect(0, y, SCREEN_W, ROW_H - 1, clr);
  delay(80);
}

// ─── Buzzer ───────────────────────────────────────────────────────────────────
void beep(int count, int onMs, int offMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(offMs);
  }
}

// ─── Hit test ─────────────────────────────────────────────────────────────────
bool inRect(int16_t bx, int16_t by, int16_t bw, int16_t bh,
            int16_t px, int16_t py) {
  return (px >= bx && px <= bx + bw && py >= by && py <= by + bh);
}
