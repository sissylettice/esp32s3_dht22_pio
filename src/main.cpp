#include <Arduino.h>
/*
  ESP32-S3 + DHT22 -> ThingsBoard (MQTT) with web-based configuration

  Libraries (Arduino Library Manager):
    - DHT sensor library (Adafruit)  + Adafruit Unified Sensor
    - PubSubClient (Nick O'Leary)
  Built in: WiFi, WebServer, DNSServer, Preferences

  Wiring: DHT22 data -> GPIO4 (10k pull-up to 3V3 if your module has none),
          VCC -> 3V3, GND -> GND.

  First boot / no WiFi: device starts an access point "ESP32-Sensor-Setup"
  (password below). Connect, a config page opens (or browse to 192.168.4.1).
  Once on your WiFi, the same page is at the device's IP address.

  Factory reset: with the sketch running, hold the BOOT button (GPIO0) for
  3 seconds until the LED turns white, then release. Do NOT hold it during
  power-up/reset: that enters download mode.
*/

#define hubtest 0


#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <esp_system.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif

// ---------- Hardware ----------
#define DHTPIN      4
#define DHTTYPE     DHT22
#define RESET_PIN   0          // BOOT button on most ESP32-S3 dev boards

#define LED_PIN     21         // Waveshare ESP32-S3-Zero onboard WS2812 RGB LED
#define LED_BRIGHT  20         // 0-255, keep low: the LED is very bright
#define LED_SWAP_RG 1          // 1 = red and green channels swapped (this LED is GRB order)

// Set to 1 to send fake readings (for testing before the DHT22 is connected)
#define SIMULATE_SENSOR 0

// ---------- Setup AP ----------
const char* AP_SSID = "ESP32-Sensor-Setup";
const char* AP_PASS = "configure-me";   // min 8 chars

// ---------- Globals ----------
struct Config {
  String ssid;
  String pass;
  String server = "thingsboard.cloud";
  uint16_t port = 1883;
  bool tls = false;
  String clientId;     // "Device ID" - MQTT client id
  String token;        // ThingsBoard device access token (MQTT username)
  uint32_t intervalS = 30;
  bool fahrenheit = false;   // temperature unit for display and telemetry
} cfg;

Preferences prefs;
WebServer web(80);
DNSServer dns;
DHT dht(DHTPIN, DHTTYPE);
WiFiClient plainClient;
WiFiClientSecure secureClient;
PubSubClient mqtt;

bool apMode = false;
unsigned long lastPublish = 0;
unsigned long lastMqttTry = 0;
unsigned long lastWifiTry = 0;
float lastT = NAN, lastH = NAN;
String lastStatus = "Starting...";
bool sensorOk = true;

// Readings are kept in Celsius internally; convert for display/telemetry
float toDisplay(float c) { return cfg.fahrenheit ? c * 9.0f / 5.0f + 32.0f : c; }

// Read the station MAC straight from the chip. The WiFi library's own
// MAC call returns all zeros while the board is in AP-only (setup) mode.
String macString() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

// ---------- Config storage ----------
void loadConfig() {
  prefs.begin("sensor", true);
  cfg.ssid      = prefs.getString("ssid", "");
  cfg.pass      = prefs.getString("pass", "");
  cfg.server    = prefs.getString("server", "thingsboard.cloud");
  cfg.port      = prefs.getUShort("port", 1883);
  cfg.tls       = prefs.getBool("tls", false);
  cfg.clientId  = prefs.getString("cid", "");
  cfg.token     = prefs.getString("token", "");
  cfg.intervalS = prefs.getUInt("interval", 30);
  cfg.fahrenheit = prefs.getBool("fahr", false);
  prefs.end();
}

void saveConfig() {
  prefs.begin("sensor", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("server", cfg.server);
  prefs.putUShort("port", cfg.port);
  prefs.putBool("tls", cfg.tls);
  prefs.putString("cid", cfg.clientId);
  prefs.putString("token", cfg.token);
  prefs.putUInt("interval", cfg.intervalS);
  prefs.putBool("fahr", cfg.fahrenheit);
  prefs.end();
}

// GPIO0 is a strapping pin: holding BOOT while the chip resets puts it in
// download mode, so the sketch never runs. Instead, hold BOOT for 3 s
// while the sketch is already running.
void checkResetButton() {
  static unsigned long pressStart = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (pressStart == 0) {
      pressStart = millis();
    } else if (millis() - pressStart > 3000) {
      Serial.println("BOOT held 3s: clearing saved settings...");
      neopixelWrite(LED_PIN, LED_BRIGHT, LED_BRIGHT, LED_BRIGHT);  // white = release now
      prefs.begin("sensor", false);
      prefs.clear();
      prefs.end();
      while (digitalRead(RESET_PIN) == LOW) delay(10);  // wait for release
      delay(300);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }
}

// ---------- Web UI ----------
String esc(const String& s) {
  String o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

String deviceDefaultId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "esp32s3-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

void handleRoot() {
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String h;
  h.reserve(4000);
  h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Sensor setup</title><style>"
         "body{font-family:system-ui,sans-serif;max-width:460px;margin:1.5rem auto;padding:0 1rem;color:#222}"
         "label{display:block;margin-top:.9rem;font-weight:600;font-size:.9rem}"
         "input[type=text],input[type=password],input[type=number]{width:100%;padding:.55rem;margin-top:.25rem;box-sizing:border-box;font-size:1rem}"
         "button{margin-top:1.4rem;padding:.7rem 1.2rem;font-size:1rem;cursor:pointer}"
         ".card{background:#f3f3f3;padding:.8rem 1rem;border-radius:8px;margin-bottom:1rem;font-size:.9rem}"
         "small{color:#666;font-weight:400}</style></head><body>");
  h += F("<h2>ESP32-S3 Sensor Setup</h2><div class='card'>");
  h += "<b>Status:</b> " + esc(lastStatus) + "<br>";
  h += "<b>IP:</b> " + ip + "<br>";
  h += "<b>MAC:</b> " + macString() + "<br>";
  h += "<b>Temperature:</b> " + (isnan(lastT) ? String("--") : String(toDisplay(lastT), 1) + (cfg.fahrenheit ? " &deg;F" : " &deg;C")) + "<br>";
  h += "<b>Humidity:</b> " + (isnan(lastH) ? String("--") : String(lastH, 1) + " %");
  h += F("</div><form method='POST' action='/save'>");

  h += F("<label>WiFi SSID</label><input type='text' name='ssid' value='");
  h += esc(cfg.ssid);
  h += F("' required>");

  h += F("<label>WiFi password <small>(leave blank to keep current)</small></label>"
         "<input type='password' name='pass' autocomplete='off'>");

  h += F("<label>ThingsBoard server</label><input type='text' name='server' value='");
  h += esc(cfg.server);
  h += F("' required>");

  h += F("<label>MQTT port <small>(1883 plain, 8883 TLS)</small></label>"
         "<input type='number' name='port' min='1' max='65535' value='");
  h += String(cfg.port);
  h += F("'>");

  h += F("<label><input type='checkbox' name='tls' value='1'");
  if (cfg.tls) h += F(" checked");
  h += F("> Use TLS (certificate not verified)</label>");

  h += F("<label>Device ID <small>(MQTT client ID)</small></label><input type='text' name='cid' value='");
  h += esc(cfg.clientId.length() ? cfg.clientId : deviceDefaultId());
  h += F("'>");

  h += F("<label>Access token <small>(ThingsBoard device credentials)</small></label>"
         "<input type='text' name='token' value='");
  h += esc(cfg.token);
  h += F("' required>");

  h += F("<label>Temperature unit</label><select name='unit' "
         "style='width:100%;padding:.55rem;margin-top:.25rem;font-size:1rem'><option value='C'");
  if (!cfg.fahrenheit) h += F(" selected");
  h += F(">Celsius (&deg;C)</option><option value='F'");
  if (cfg.fahrenheit) h += F(" selected");
  h += F(">Fahrenheit (&deg;F)</option></select>");

  h += F("<label>Publish interval (seconds)</label><input type='number' name='interval' min='5' max='86400' value='");
  h += String(cfg.intervalS);
  h += F("'>");

  h += F("<button type='submit'>Save &amp; reboot</button></form></body></html>");
  web.send(200, "text/html", h);
}

void handleSave() {
  String newSsid = web.arg("ssid");
  newSsid.trim();
  if (newSsid.length() == 0) {
    web.send(400, "text/plain", "SSID required");
    return;
  }
  cfg.ssid = newSsid;
  if (web.arg("pass").length() > 0) cfg.pass = web.arg("pass");

  cfg.server = web.arg("server");  cfg.server.trim();
  cfg.port = (uint16_t)constrain(web.arg("port").toInt(), 1, 65535);
  cfg.tls = web.hasArg("tls");
  cfg.clientId = web.arg("cid");   cfg.clientId.trim();
  cfg.token = web.arg("token");    cfg.token.trim();
  cfg.intervalS = (uint32_t)constrain(web.arg("interval").toInt(), 5, 86400);
  cfg.fahrenheit = (web.arg("unit") == "F");

  saveConfig();
  web.send(200, "text/html",
           "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
           "<body style='font-family:system-ui;padding:1.5rem'><h3>Saved.</h3>"
           "<p>Rebooting and connecting to your WiFi. Find the new IP address in your router's "
           "client list or the serial monitor. This device's MAC address is:</p>"
           "<p style='font-size:1.2rem;font-family:monospace'>" + macString() +
           "</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleStatus() {
  String j = "{\"temperature\":";
  j += isnan(lastT) ? "null" : String(toDisplay(lastT), 1);
  j += ",\"humidity\":";
  j += isnan(lastH) ? "null" : String(lastH, 1);
  j += ",\"unit\":\"" + String(cfg.fahrenheit ? "F" : "C") + "\"";
  j += ",\"status\":\"" + lastStatus + "\"}";
  web.send(200, "application/json", j);
}

void handleNotFound() {
  if (apMode) {  // captive portal redirect
    web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    web.send(302, "text/plain", "");
  } else {
    web.send(404, "text/plain", "Not found");
  }
}

void startWebServer() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/status", HTTP_GET, handleStatus);
  web.onNotFound(handleNotFound);
  web.begin();
}

// ---------- WiFi ----------
void startAP() {
  apMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  lastStatus = "Setup mode (AP)";
  Serial.printf("AP started: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

bool connectWiFi(uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  Serial.printf("Connecting to %s", cfg.ssid.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

// ---------- MQTT ----------
void configureMqtt() {
  if (cfg.tls) {
    secureClient.setInsecure();  // skip cert validation; load a CA cert for production
    mqtt.setClient(secureClient);
  } else {
    mqtt.setClient(plainClient);
  }
  mqtt.setServer(cfg.server.c_str(), cfg.port);
  mqtt.setBufferSize(512);
}

bool mqttConnect() {
  String cid = cfg.clientId.length() ? cfg.clientId : deviceDefaultId();
  // ThingsBoard: username = access token, empty password
  bool ok = mqtt.connect(cid.c_str(), cfg.token.c_str(), nullptr);
  if (ok) {
    lastStatus = "Connected to " + cfg.server;
  } else {
    lastStatus = "MQTT failed (rc=" + String(mqtt.state()) + ")";
  }
  Serial.println(lastStatus);
  return ok;
}

void readAndPublish() {
#if SIMULATE_SENSOR
  float h = 45.0f + random(-50, 50) / 10.0f;
  float t = 21.0f + random(-30, 30) / 10.0f;
#else
  float h = dht.readHumidity();
  float t = dht.readTemperature();  // Celsius
#endif
  if (isnan(h) || isnan(t)) {
    lastT = NAN;
    lastH = NAN;
    sensorOk = false;
    Serial.println("DHT22 read failed");
    if (mqtt.connected()) {
      // Report the failure to ThingsBoard as a telemetry key
      if (mqtt.publish("v1/devices/me/telemetry", "{\"sensorStatus\":\"DHT22 read failed\"}")) {
        Serial.println("Published: sensorStatus = DHT22 read failed");
      }
    }
    return;
  }
  lastT = t;
  lastH = h;
  sensorOk = true;

  if (!mqtt.connected()) return;
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%.1f,\"humidity\":%.1f,\"sensorStatus\":\"OK\"}", toDisplay(t), h);
  if (mqtt.publish("v1/devices/me/telemetry", payload)) {
    Serial.printf("Published: %s\n", payload);
  } else {
    Serial.println("Publish failed");
  }
}

// ---------- Status LED ----------
// Blue blinking = setup AP | Yellow = WiFi connecting/lost | Purple = MQTT down
// Red = DHT22 read failed  | Green = all good
uint32_t lastLedColor = 0xFFFFFFFF;
unsigned long lastLedUpdate = 0;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c == lastLedColor) return;  // only write on change
  lastLedColor = c;
  uint8_t rr = (uint8_t)(r * LED_BRIGHT / 255);
  uint8_t gg = (uint8_t)(g * LED_BRIGHT / 255);
  uint8_t bb = (uint8_t)(b * LED_BRIGHT / 255);
#if LED_SWAP_RG
  neopixelWrite(LED_PIN, gg, rr, bb);
#else
  neopixelWrite(LED_PIN, rr, gg, bb);
#endif
}

void updateLed() {
  if (apMode) {
    if ((millis() / 500) % 2 == 0) setLed(0, 0, 255);
    else setLed(0, 0, 0);
  } else if (WiFi.status() != WL_CONNECTED) {
    setLed(255, 180, 0);
  } else if (!mqtt.connected()) {
    setLed(255, 0, 255);
  } else if (!sensorOk) {
    setLed(255, 0, 0);
  } else {
    setLed(0, 255, 0);
  }
}

// ---------- Periodic serial status ----------
unsigned long lastStatusPrint = 0;

void printStatus() {
  if (apMode) {
    Serial.printf("[status] Setup AP '%s' - browse to http://%s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[status] WiFi OK - http://%s | MQTT %s | %s\n",
                  WiFi.localIP().toString().c_str(),
                  mqtt.connected() ? "connected" : "disconnected",
                  lastStatus.c_str());
  } else {
    Serial.println("[status] WiFi not connected");
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  unsigned long st = millis();
  while (!Serial && millis() - st < 3000) delay(10);  // wait for monitor (USB CDC)
  delay(300);
  Serial.printf("MAC address: %s\n", macString().c_str());
  pinMode(RESET_PIN, INPUT_PULLUP);
  loadConfig();
  dht.begin();
  setLed(255, 180, 0);  // yellow while connecting

  if (cfg.ssid.length() == 0 || !connectWiFi()) {
    startAP();
  } else {
    lastStatus = "WiFi connected";
    configureMqtt();
  }
  startWebServer();
}

void loop() {
  web.handleClient();
  checkResetButton();

  if (millis() - lastLedUpdate > 250) {
    lastLedUpdate = millis();
    updateLed();
  }

  if (millis() - lastStatusPrint > 10000) {
    lastStatusPrint = millis();
    printStatus();
  }

  if (apMode) {
    dns.processNextRequest();
    return;
  }

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    lastStatus = "WiFi lost, reconnecting...";
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
    return;
  }

  // MQTT keep-alive / reconnect
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry > 5000) {
      lastMqttTry = millis();
      mqttConnect();
    }
  } else {
    mqtt.loop();
  }

  // Periodic telemetry (DHT22 needs >= 2 s between reads)
  if (millis() - lastPublish >= cfg.intervalS * 1000UL || lastPublish == 0) {
    lastPublish = millis();
    readAndPublish();
  }
}
