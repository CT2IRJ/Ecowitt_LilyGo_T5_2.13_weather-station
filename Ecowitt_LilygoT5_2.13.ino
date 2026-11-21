#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
SET_LOOP_TASK_STACK_SIZE(16384);

// LilyGo T5 2.13" V2.3 / V2.4 Pinout
#define EPD_CS     5
#define EPD_DC     17
#define EPD_RST    16
#define EPD_BUSY   4
#define EPD_POWER  12 // Used on V2.4 boards; harmless on V2.3
#define BTN_CONFIG 39 // Onboard programmable button (IO39)

// Display driver for DEPG0213BN / GDEM0213B74 (122x250 B&W)
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

Preferences prefs;
WebServer server(80);

// Configuration variables (input here your details)
String appKey = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";  //Ecowitt application Key 32 Chars
String apiKey = "";  //Ecowitt API Key
String macAddr = "XX:XX:XX:XX:XX:XX";  //Ecowitt Weather Station MAC Address
int refreshMins = 15;  // Intervalo de actualização

unsigned long lastFetch = 0;
bool forceRefresh = true;

// Weather Data Structure
struct WeatherData {
  String temp = "--";
  String humidity = "--";
  String wind = "--";
  String windDir = "--";
  String rain = "--";
  String solar = "--";
  String battery = "--";
  String status = "OK";
  String lastUpdate = "--/-- --:--";
} weather;

String lastDebugInfo = "No fetch yet";

void loadConfig() {
  prefs.begin("ecowitt", true);
  appKey = prefs.getString("appKey", "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
  apiKey = prefs.getString("apiKey", "");
  macAddr = prefs.getString("macAddr", "98:F4:AB:D2:53:3B");
  refreshMins = prefs.getInt("refresh", 15);
  prefs.end();
}

void saveConfig() {
  prefs.begin("ecowitt", false);
  prefs.putString("appKey", appKey);
  prefs.putString("apiKey", apiKey);
  prefs.putString("macAddr", macAddr);
  prefs.putInt("refresh", refreshMins);
  prefs.end();
}

// Local Web Configuration Homepage (accessible at http://<ESP32-IP>/)
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 15px;}";
  html += "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box;}";
  html += "button{width:100%;padding:10px;background:#111;color:#fff;border:none;font-size:16px;cursor:pointer;}</style></head><body>";
  html += "<h2>Ecowitt E-Paper Config</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Application Key:</label><input name='appKey' value='" + appKey + "' required>";
  html += "<label>API Key (from Ecowitt Profile):</label><input name='apiKey' value='" + apiKey + "' required>";
  html += "<label>Station MAC Address:</label><input name='macAddr' value='" + macAddr + "' required>";
  html += "<label>Update Interval (minutes):</label><input type='number' name='refresh' min='1' max='120' value='" + String(refreshMins) + "'>";
  html += "<button type='submit'>Save & Refresh Display</button></form>";
  html += "<br><form action='/resetwifi' method='POST'><button type='submit' style='background:#a00;'>Reset Wi-Fi & Open AP</button></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("appKey")) appKey = server.arg("appKey");
  if (server.hasArg("apiKey")) apiKey = server.arg("apiKey");
  if (server.hasArg("macAddr")) macAddr = server.arg("macAddr");
  if (server.hasArg("refresh")) refreshMins = max(1, (int)server.arg("refresh").toInt());

  appKey.trim();
  apiKey.trim();
  macAddr.trim();

  saveConfig();
  forceRefresh = true;
  server.send(200, "text/html", "<h3>Saved! Refreshing E-Paper display...</h3><a href='/'>Back</a>");
}

void handleResetWifi() {
  server.send(200, "text/html", "<h3>Wi-Fi cleared. Rebooting into AP mode (LilyGo-Weather-Setup)...</h3>");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

String degToCardinal(const char* degStr) {
  if (!degStr || strlen(degStr) == 0) return "--";
  int deg = atoi(degStr);
  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (int)((deg + 22.5) / 45.0) % 8;
  return String(dirs[index]);
}



void fetchEcowittData() {
  lastDebugInfo = "fetchEcowittData() started. apiKey len=" + String(apiKey.length());

  if (apiKey.length() < 5) {
    weather.status = "Set API Key at " + WiFi.localIP().toString();
    lastDebugInfo += "\nAborted: apiKey too short.";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String host = "https://ecowitt.net";
  String path = "/api/v3/device/real_time?application_key=";
  String url = host + path + appKey +
               "&api_key=" + apiKey +
               "&mac=" + macAddr +
               "&call_back=all" +
               "&temp_unitid=1&wind_speed_unitid=7&rainfall_unitid=12&solarradiation_unitid=10";

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  lastDebugInfo = "Attempting URL:\n" + url + "\n";

  if (!http.begin(client, url)) {
    lastDebugInfo += "\nERROR: http.begin() returned FALSE!";
    weather.status = "HTTP Begin Fail";
    return;
  }

  int httpCode = http.GET();
  String payload = http.getString();
  lastDebugInfo += "\nHTTP Code: " + String(httpCode) + "\n\nPayload:\n" + payload;

  if (httpCode == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err && doc["code"].as<int>() == 0) {
      JsonObject data = doc["data"];

      const char* t = data["outdoor"]["temperature"]["value"];
      const char* h = data["outdoor"]["humidity"]["value"];
      const char* w = data["wind"]["wind_speed"]["value"];
      const char* wd = data["wind"]["wind_direction"]["value"];
      const char* r = data["rainfall"]["daily"]["value"];
      const char* s = data["solar_and_uvi"]["solar"]["value"];
      const char* b = data["battery"]["sensor_array"]["value"];

      weather.temp     = t ? String(t) : "--";
      weather.humidity = h ? String(h) : "--";
      weather.wind     = w ? String(w) : "--";
      weather.windDir = degToCardinal(wd);
      weather.rain     = r ? String(r) : "0.0";
      weather.solar    = s ? String(s) : "--";

      if (!b) {
        weather.battery = "--";
      } else if (strcmp(b, "0") == 0) {
        weather.battery = "OK";
      } else if (strcmp(b, "1") == 0) {
        weather.battery = "LOW";
      } else {
        weather.battery = String(b);
      }

      weather.status = WiFi.localIP().toString();

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 2000)) {
char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%d/%m %H:%M", &timeinfo);
        weather.lastUpdate = String(timeStr);
      } else {
        const char* apiTime = doc["time"];
        if (apiTime) {
          time_t rawTime = (time_t)atol(apiTime);
          struct tm *ptm = localtime(&rawTime);
          if (ptm) {
char timeStr[20];
            strftime(timeStr, sizeof(timeStr), "%d/%m %H:%M", ptm);
            weather.lastUpdate = String(timeStr);
          }
        }
      }
    } else {
      const char* msg = doc["msg"];
      weather.status = "API Err: " + String(msg ? msg : "Parse");
    }
  } else {
    weather.status = "HTTP Err: " + String(httpCode);
  }
  http.end();
}





void drawDisplay() {
  display.setRotation(1); // Landscape: 250x122 px
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Divider lines
    display.drawLine(0, 56, 250, 56, GxEPD_BLACK);
    display.drawLine(125, 0, 125, 104, GxEPD_BLACK);
    display.drawLine(0, 104, 250, 104, GxEPD_BLACK);

    // Top-Left: Outdoor Temperature with "Temp" header
    display.setFont(&FreeSans9pt7b);
    display.setCursor(6, 16);
    display.print("Temp");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(6, 48);
    display.print(weather.temp);
    display.setFont(&FreeSansBold9pt7b);
    display.print(" C");

       // Top-Right: Wind + Cardinal Direction
    display.setFont(&FreeSans9pt7b);
    display.setCursor(133, 16);
    display.print("Wind (" + weather.windDir + ")");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(133, 48);
    display.print(weather.wind);
    display.setFont(&FreeSansBold9pt7b);
    display.print(" km/h");


    // Bottom-Left: Rain & Humidity
    display.setFont(&FreeSans9pt7b);
    display.setCursor(6, 76);
    display.print("Rain: " + weather.rain + " mm");
    display.setCursor(6, 97);
    display.print("Hum: " + weather.humidity + "%");

    // Bottom-Right: Solar & Station Battery
    display.setCursor(133, 76);
    display.print("Sol: " + weather.solar + " W/m2");
    display.setCursor(133, 97);
    display.print("Stn Batt: " + weather.battery);

    // Footer bar: IP + Last Fetch Date/Time
    display.setFont(NULL); // Built-in crisp 6x8 font
    display.setCursor(6, 110);
    display.print(weather.status + " | " + weather.lastUpdate);

  } while (display.nextPage());
  display.hibernate();
}



void setup() {
  Serial.begin(115200);
  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);
  pinMode(BTN_CONFIG, INPUT);

  display.init(115200, true, 2, false);
  loadConfig();

  // Show setup instructions on screen while connecting
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(10, 35);
    display.print("Connecting to Wi-Fi...");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 65);
    display.print("If first run, connect to AP:");
    display.setCursor(10, 88);
    display.print("LilyGo-Weather-Setup");
  } while (display.nextPage());

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("LilyGo-Weather-Setup")) {
    ESP.restart();
  }
  // Sync local time for Portugal (WET/WEST automatic daylight saving)
  configTzTime("WET0WEST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.nist.gov");
  delay(1500);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/resetwifi", HTTP_POST, handleResetWifi);
/*
  server.on("/debug", []() {
    server.send(200, "text/plain", lastDebugInfo);
  });
  */
  server.begin();

}

void loop() {
  server.handleClient();

  // Pressing onboard button (GPIO39) triggers an immediate refresh
  if (digitalRead(BTN_CONFIG) == LOW) {
    delay(200);
    forceRefresh = true;
  }

  unsigned long intervalMs = (unsigned long)refreshMins * 60UL * 1000UL;
  if (forceRefresh || (millis() - lastFetch >= intervalMs)) {
    forceRefresh = false;
    lastFetch = millis();
    fetchEcowittData();
    drawDisplay();
  }
}
