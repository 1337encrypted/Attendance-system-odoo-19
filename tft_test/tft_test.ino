/*
 * Minimal TFT SPI test — no WiFi, no touch
 * Cycles: RED → GREEN → BLUE → WHITE → BLACK (3s each)
 * If you see colour changes, SPI is working.
 * If screen stays white/unchanged, check wiring.
 *
 * Current pin assignments (User_Setup.h):
 *   MOSI → GPIO23   MISO → GPIO19   SCK  → GPIO18
 *   CS   → GPIO4    DC   → GPIO21   RST  → GPIO22
 *   BL   → GPIO15
 */
#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== TFT minimal test ===");

  // Backlight
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  Serial.println("Backlight ON (GPIO15 HIGH)");

  // Init display
  tft.init();
  tft.setRotation(1);
  Serial.printf("Display size: %d x %d\n", tft.width(), tft.height());
}

void fillAndReport(uint16_t colour, const char* name) {
  Serial.printf(">> %s\n", name);
  tft.fillScreen(colour);
  tft.setTextColor(~colour);   // Inverted colour for contrast
  tft.setTextSize(4);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(name, tft.width() / 2, tft.height() / 2);
  delay(3000);
}

void loop() {
  fillAndReport(TFT_RED,   "RED");
  fillAndReport(TFT_GREEN, "GREEN");
  fillAndReport(TFT_BLUE,  "BLUE");
  fillAndReport(TFT_WHITE, "WHITE");
  fillAndReport(TFT_BLACK, "BLACK");
}
