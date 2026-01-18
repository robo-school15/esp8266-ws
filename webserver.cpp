/* *
 *
 * Проєкт веб-сервера для ESP8266, який опитує датчик і виводить дані на сторінку з графіком цих даних.
 * Веб-сервер також містить сторінку для налаштувань WiFi. Налаштування зберігаються в EEPROM.
 *
 *
 * */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// Конфігурація:

#define EEPROM_SIZE 96
#define STREAM_INTERVAL 50   // ms

ESP8266WebServer server(80);
WiFiClient streamClient;

const char* ap_ssid = "ESP_Config";
const char* ap_pass = "12345678";

struct WifiConfig {
  char ssid[32];
  char password[32];
};

WifiConfig wifiCfg;
unsigned long lastSend = 0;


// Читання і запис  конфігурації на флеш (EEPROM):
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiCfg);

  if (wifiCfg.ssid[0] == 0xFF || wifiCfg.ssid[0] == '\0') {
    wifiCfg.ssid[0] = 0;
    wifiCfg.password[0] = 0;
  }
}

void saveConfig() {
  EEPROM.put(0, wifiCfg);
  EEPROM.commit();
}


// wifi:  AP + STA
void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass);

  if (strlen(wifiCfg.ssid) > 0) {
    WiFi.begin(wifiCfg.ssid, wifiCfg.password);
  }
}


// HTML-сторінка з графіком:
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Realtime Monitor</title>
<style>
body { font-family: sans-serif; background:#111; color:#eee; }
canvas { background:#000; }
</style>
</head>
<body>
<h2>Realtime data</h2>
<canvas id="c" width="600" height="300"></canvas>

<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const maxPoints = 200;
let data = [];

const src = new EventSource('/stream');
src.onmessage = e => {
  data.push(parseFloat(e.data));
  if (data.length > maxPoints) data.shift();
};

function draw() {
  ctx.clearRect(0,0,canvas.width,canvas.height);
  ctx.beginPath();
  ctx.strokeStyle = "#0f0";

  data.forEach((v,i)=>{
    let x = i * canvas.width / maxPoints;
    let y = canvas.height - v * canvas.height / 100;
    if(i===0) ctx.moveTo(x,y);
    else ctx.lineTo(x,y);
  });

  ctx.stroke();
  requestAnimationFrame(draw);
}
draw();
</script>
</body>
</html>
)rawliteral";


// SSE-стрім:
void handleStream() {
  if (streamClient && streamClient.connected()) {
    server.send(409, "text/plain", "Stream already open");
    return;
  }

  streamClient = server.client();

  streamClient.println("HTTP/1.1 200 OK");
  streamClient.println("Content-Type: text/event-stream");
  streamClient.println("Cache-Control: no-cache");
  streamClient.println("Connection: keep-alive");
  streamClient.println();

  lastSend = millis();
}

// HTTP-handlers:
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleConfig() {
  String page =
    "<form method='POST' action='/save'>"
    "SSID:<br><input name='s' value='" + String(wifiCfg.ssid) + "'><br>"
    "PASS:<br><input name='p' type='password'><br><br>"
    "<input type='submit' value='Save'></form>";
  server.send(200, "text/html", page);
}

void handleSave() {
  strncpy(wifiCfg.ssid, server.arg("s").c_str(), 31);
  strncpy(wifiCfg.password, server.arg("p").c_str(), 31);
  saveConfig();
  server.send(200, "text/html", "Saved. Reboot device.");
}

// Читаємо сенсори (заглушка):
float readSensor() {
  // приклад
  static float v = 0;
  v += random(-3,4);
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  return v;
}



//=============================================================

void setup() {
  Serial.begin(115200);

  loadConfig();
  setupWiFi();

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/stream", handleStream);

  server.begin();
}

void loop() {
  server.handleClient();

  if (streamClient && streamClient.connected()) {
    unsigned long now = millis();
    if (now - lastSend >= STREAM_INTERVAL) {
      lastSend = now;

      streamClient.print("data:");
      streamClient.println(readSensor());
      streamClient.println();

      yield();
    }
  }
}



