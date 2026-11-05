/*
 * Telegram connection tester for TinyScreen+ + WiFi TinyShield (ASD2123-R)
 * Open Serial Monitor at 115200 baud, then watch which STEP fails.
 *
 * Needs the same libraries as study_box.ino (WiFi101, ArduinoJson 6.x, UniversalTelegramBot)
 * and a copy of YOUR filled-in secrets.h in this sketch's folder.
 */

#include <SPI.h>
#include <WiFi101.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include "secrets.h"

WiFiSSLClient wifiClient;
UniversalTelegramBot bot(BOT_TOKEN, wifiClient);

void setup() {
  SerialUSB.begin(115200);
  unsigned long t0 = millis();
  while (!SerialUSB && millis() - t0 < 8000) {}      // wait up to 8 s for the Serial Monitor
  SerialUSB.println("=== Telegram test ===");

  WiFi.setPins(8, 2, A3, -1);
  SerialUSB.print("STEP 1 WiFi shield present: ");
  SerialUSB.println(WiFi.status() == WL_NO_SHIELD ? "NO (check stacking / setPins)" : "yes");

  SerialUSB.print("STEP 2 Connecting to "); SerialUSB.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); SerialUSB.print("."); }
  SerialUSB.println();
  if (WiFi.status() != WL_CONNECTED) {
    SerialUSB.println("  FAILED: not connected (wrong name/password, or network is 5 GHz-only / needs a login page)");
    return;
  }
  SerialUSB.print("  connected, IP "); SerialUSB.print(WiFi.localIP());
  SerialUSB.print(", RSSI "); SerialUSB.println(WiFi.RSSI());

  SerialUSB.print("STEP 3 Clock (epoch): ");
  unsigned long ep = 0;
  for (int i = 0; i < 10 && ep < 1600000000UL; i++) { ep = WiFi.getTime(); delay(500); }
  SerialUSB.println(ep);

  SerialUSB.print("STEP 4 Secure connection to api.telegram.org:443: ");
  bool ok = wifiClient.connect("api.telegram.org", 443);
  SerialUSB.println(ok ? "OK" : "FAILED (SSL certificate missing on the WiFi module, or no internet)");
  wifiClient.stop();

  SerialUSB.print("STEP 5 Bot token check (getMe): ");
  SerialUSB.println(bot.getMe() ? "OK" : "FAILED (wrong token, or step 4 failed)");

  SerialUSB.print("STEP 6 Send test message: ");
  SerialUSB.println(bot.sendMessage(CHAT_ID, "Test message from the TinyScreen+", "") ? "OK (check Telegram)" : "FAILED (wrong chat ID, or you haven't pressed Start on the bot)");

  SerialUSB.println("STEP 7 Now send /start 25 to your bot. Polling every 3 s...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { delay(1000); return; }
  int n = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < n; i++) {
    SerialUSB.print("  message from chat "); SerialUSB.print(bot.messages[i].chat_id);
    SerialUSB.print(": "); SerialUSB.println(bot.messages[i].text);
  }
  delay(3000);
}
