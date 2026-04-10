/* *
 *
 * Проєкт веб-сервера для ESP8266, який опитує датчик і виводить дані на сторінку з графіком цих даних.
 * Веб-сервер також містить сторінку для налаштувань WiFi. Налаштування зберігаються в EEPROM.
 * 
 * Після підключення до внутрішньої точки доступу wifi (ESP_Config), вебсервер буде доступний з браузера по
 * адресі http://192.168.4.1.
 * Для підключення до модуля до іншого роутера, необхідно зайти на сторінку:
 * http://192.168.4.1/config та ввести параметри підключення до іншого wifi-роутера. Ввести SSID, Password та активувати чекбокс 'Enable WiFi STA'.
 * 
 * Після збереження та перезавантаження, вебсервер esp8266 стане доступним у внутрішній мережі за тією адресою,
 * яку видав йому wifi-роутер вашої мережі.
 * 
 *
 * */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// Бібліотеки для датчика температури ds18b20
#include <OneWire.h>
#include <DallasTemperature.h>


// Конфігурація:
#define EEPROM_SIZE 4096
#define STREAM_MIN_INTERVAL 20     // ms
#define STREAM_MAX_INTERVAL 10000  // ms
#define STREAM_DEF_INTERVAL 100    // ms


//Створюємо глобальні змінні для датчика температури:
// для зв’язку з пристроями OneWire
OneWire* oneWire = nullptr;
// для Dallas Temperature. 
DallasTemperature* sensors = nullptr;

ESP8266WebServer server(80);
WiFiClient streamClient;

// Параметри вбудованої в модуль ESP8266 wifi точки доступу:
const char* ap_ssid = "ESP-Sensor";
const char* ap_pass = "12345678";

const char* def_graph_title = "Sensor value";

// кодове слово для перевірки цілісності конфіга в EEPROM
#define CONFIG_MAGIC 0xDEADBEEF

enum SensorMode {
  MODE_DEMO = 0,
  MODE_GPIO0 = 1,
  MODE_GPIO2 = 2
};

struct Config {

  uint32_t magic;

  struct wifi {
    char ssid[32];      // SSID зовнішньої точки доступу wifi
    char password[32];  // пароль зовнішньої точки доступу wifi
    bool enabled;       // признак використання зовнішньої точки доступу
  } wifi;
  
  struct filter {
    bool  enabled;   // увімкнено / вимкнено
    float alpha;    // коефіцієнт згладжування
  } filter;

  struct sensor {
    uint16_t intervalMs;   // інтервал опитування сенсора (мс)
  } sensor;

  struct ui {
    char deviceName[32];   // SSID точки доступу
    char graphTitle[96];   // заголовок графіка
  } ui;
  
  SensorMode mode; 
};

Config cfg;

void initSensor() {
  int pin;

  switch (cfg.mode) {
    case MODE_GPIO0: pin = 0; break;
    case MODE_GPIO2: pin = 2; break;
    default: return;
  }

  if (oneWire) delete oneWire;
  if (sensors) delete sensors;

  oneWire = new OneWire(pin);
  sensors = new DallasTemperature(oneWire);
  sensors->begin();

  Serial.println("Sensor initialized on GPIO" + String(pin));
};



//Ініціалізація конфіга значеннями по змовчуванню
void setDefaults( Config& cfg ) {
  memset(&cfg, 0, sizeof(cfg));

  cfg.magic = CONFIG_MAGIC;

  // WiFi
  cfg.wifi.enabled = false;

  // filter
  cfg.filter.enabled = false;
  cfg.filter.alpha = 0.2;

  // sensor
  cfg.sensor.intervalMs = STREAM_DEF_INTERVAL;

  strcpy(cfg.ui.deviceName, ap_ssid);
  strcpy(cfg.ui.graphTitle, def_graph_title);

  cfg.mode = MODE_DEMO;   // за замовчуванням – демо
}



// Глобальні змінні для фільтра даних
float filteredValue;
bool filterInitialized;

// Ініціалізація глобальних змінних
void globalInit() {
  filteredValue = 0;
  filterInitialized = false;
  cfg.filter.enabled = false;
  cfg.filter.alpha = 0.2;
  cfg.sensor.intervalMs = STREAM_DEF_INTERVAL;
  strcpy(cfg.ui.deviceName, ap_ssid);
  strcpy(cfg.ui.graphTitle, def_graph_title);   
  cfg.mode = MODE_DEMO;
}


// Функція фільтрації даних
float applyFilter(float newValue) {
  if (!cfg.filter.enabled) return newValue;

  if (!filterInitialized) {
    filteredValue = newValue;
    filterInitialized = true;
  } else {
    filteredValue = cfg.filter.alpha * newValue +
                    (1.0 - cfg.filter.alpha) * filteredValue;
  }

  return filteredValue;
}


unsigned long lastSend = 0;

// Читання і запис  конфігурації на флеш (EEPROM):
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
 
  EEPROM.get(0, cfg);

  // перевірка magic
  if (cfg.magic != CONFIG_MAGIC) {
    setDefaults(cfg);
    saveConfig();
    return;
  }

  if (cfg.wifi.ssid[0] == 0xFF || cfg.wifi.ssid[0] == '\0') {
    cfg.wifi.ssid[0] = 0;
    cfg.wifi.password[0] = 0;
    cfg.wifi.enabled = false;
  }

  // alpha
  if (cfg.filter.alpha <= 0 || cfg.filter.alpha > 1) {
    cfg.filter.alpha = 0.2;
  } 

  cfg.wifi.enabled   = cfg.wifi.enabled ? true : false;
  cfg.filter.enabled = cfg.filter.enabled ? true : false;

  // interval
  if (cfg.sensor.intervalMs < STREAM_MIN_INTERVAL || cfg.sensor.intervalMs > STREAM_MAX_INTERVAL) {
    cfg.sensor.intervalMs = STREAM_DEF_INTERVAL;
  }

  if (cfg.ui.deviceName[0] == 0xFF || cfg.ui.deviceName[0] == '\0') {
    strcpy(cfg.ui.deviceName, ap_ssid);
  }
  if (cfg.ui.graphTitle[0] == 0xFF || cfg.ui.graphTitle[0] == '\0') {
    strcpy(cfg.ui.graphTitle, def_graph_title);
  }
}

void saveConfig() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}


// wifi:  AP + STA
// Встановлюємо режим роботи wifi так, щоб esp8266 працював одночасно і як точка доступу (AP), 
// і як клієнт, який з'єднується з мережею через інший wifi-роутер, параметри доступу якого
// задані в wificfg.  
void setupWiFi(Config& cfg) {
        
    // Завжди піднімаємо AP з фіксованим IP
    WiFi.mode(WIFI_AP_STA); // AP + STA
    IPAddress apIP(192,168,4,1);
    IPAddress netMsk(255,255,255,0);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(cfg.ui.deviceName, ap_pass);

    Serial.println("AP started");
    Serial.println("AP IP: " + WiFi.softAPIP().toString());

    //Якщо WiFi увімкнено і задано SSID, підключаємося до STA
    if (cfg.wifi.enabled && strlen(cfg.wifi.ssid) > 0) {
        Serial.println("Connecting to external WiFi...");
        Serial.println("SSID: " + String(cfg.wifi.ssid));
        //Serial.println("Password: " + String(cfg.wifi.password));

        WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);

        unsigned long start = millis();
        const unsigned long timeout = 15000; // 15 сек
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout) {
            delay(500);
            Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println();
            Serial.println("STA Connected!");
            Serial.println("STA IP: " + WiFi.localIP().toString());

            //Перезапускаємо AP після підключення STA
            WiFi.softAPdisconnect(true);
            delay(500);
            WiFi.softAP(cfg.ui.deviceName, ap_pass);
            Serial.println("AP restarted after STA connect");
            Serial.println("AP IP: " + WiFi.softAPIP().toString());
        } else {
            Serial.println();
            Serial.println("STA connection failed, staying in AP mode");
        }
    }

    //Додатково вимикаємо sleep для стабільності
    WiFi.setSleep(false);
}



const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>%TITLE%</title>
  <style>
    body { font-family: sans-serif; }
    canvas { border: 1px solid black; }
  </style>
</head>
<body>
  <h3>Sensor value over time</h3>
  <canvas id="graph"></canvas>

  <script>
    const canvas = document.getElementById('graph');
    const ctx = canvas.getContext('2d');

    let width, height;
    width = canvas.width;
    height = canvas.height;

    let data = []; // масив {time, value}
    const maxPoints = 200; // кількість точок на графіку
    
    // 🔹 функція адаптації розміру
    function resizeCanvas() {
      const w = window.innerWidth;
      const h = window.innerHeight;

      canvas.width  = Math.max(300, w * 0.8);
      canvas.height = Math.max(200, h * 0.7);

      width  = canvas.width;
      height = canvas.height;
    }

    // встановлення розміру графіка при старті
    resizeCanvas();

    // зміна розміру графіка при зміні розміру вікна
    window.addEventListener('resize', () => {
      resizeCanvas();
      drawGraph();
    });

    // SSE підключення
    const evtSource = new EventSource("/stream");
    evtSource.onmessage = function(e) {
      const val = parseFloat(e.data);
      const now = Date.now();
      data.push({time: now, value: val});
      if (data.length > maxPoints) data.shift();
      drawGraph();
    };

    function getFontSize() {
      // % від висоти canvas
      return Math.max(10, Math.floor(height * 0.04));
    }

    function drawGraph() {
      if (data.length === 0) return;

      // 🔹 гарантуємо актуальні розміри
      width = canvas.width;
      height = canvas.height;

      ctx.clearRect(0, 0, width, height);

      // знаходимо min/max для автоскейлу
      let min = Math.min(...data.map(d=>d.value));
      let max = Math.max(...data.map(d=>d.value));

      // невеликий запас зверху/знизу
      const range = max - min || 1;
      min -= 0.1*range;
      max += 0.1*range;

      // шрифти
      const fontSize = getFontSize();
      ctx.font = fontSize + "px Arial";

      // 🔹 функція форматування
      function formatValue(v) {
        if (range > 100) return v.toFixed(0);
        if (range > 10)  return v.toFixed(1);
        return v.toFixed(2);
      }

      // 🔹 визначаємо ширину підписів
      let maxLabelWidth = 0;
      const steps = 5;

      for (let i = 0; i <= steps; i++) {
        const val = min + (max - min) * i / steps;
        const text = formatValue(val);
        const w = ctx.measureText(text).width;
        if (w > maxLabelWidth) maxLabelWidth = w;
      }
      // 🔹 динамічний padding
      const padding = Math.max(30, maxLabelWidth + 10);

      
      // ---------- сітка --------------------
      ctx.strokeStyle = "#ddd";   // світло-сірий
      ctx.lineWidth = 1;

      ctx.beginPath();

      // горизонтальні лінії (OY)
      const stepsY = 5;
      for (let i = 0; i <= stepsY; i++) {
        const y = padding + (height - 2 * padding) * i / stepsY;
        ctx.moveTo(padding, y);
        ctx.lineTo(width, y);
      }

      // вертикальні лінії (OX)
      const stepsX = 5;
      for (let i = 0; i <= stepsX; i++) {
        const x = padding + (width - 2 * padding) * i / stepsX;
        ctx.moveTo(x, 0);
        ctx.lineTo(x, height - padding);
      }

      ctx.stroke();

      // -------------------------------------
      // координатні осі
      ctx.strokeStyle = "#000";
      ctx.beginPath();
      ctx.moveTo(padding, 0);
      ctx.lineTo(padding, height-padding);
      ctx.lineTo(width, height-padding);
      ctx.stroke();

      // підписи OY
      ctx.fillStyle = "#000";
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.font = (fontSize * 1.0) + "px Arial";
      const step = 5;
      for (let i=0; i<=step; i++) {
        const y = padding + (height - 2*padding)*(step-i)/step;
        const val = min + (max-min)*i/step;
        ctx.fillText(val.toFixed(1), padding-5, y);
        ctx.beginPath();
        ctx.moveTo(padding-3, y);
        ctx.lineTo(padding, y);
        ctx.stroke();
      }

      // підписи OX (час)
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      ctx.font = (fontSize * 0.9) + "px Arial";
      const timeStep = 5; // 5 поділок
      const startTime = data[0].time;
      const endTime = data[data.length-1].time;
      for (let i=0; i<=timeStep; i++) {
        const x = padding + (width-2*padding)*i/timeStep;
        const t = new Date(startTime + (endTime-startTime)*i/timeStep);
        const label = t.getMinutes() + ":" + t.getSeconds();
        ctx.fillText(label, x, height-padding+5);
      }

      // малюємо графік
      ctx.strokeStyle = "#f00";
      ctx.beginPath();
      data.forEach((d,i) => {
        const x = padding + (width-2*padding)*(i/(data.length-1));
        const y = padding + (height-2*padding)*(1-(d.value-min)/(max-min));
        if (i===0) ctx.moveTo(x,y);
        else ctx.lineTo(x,y);
      });
      ctx.stroke();
    }
  </script> 
</body>
</html>
)rawliteral";

// SSE-стрім:
void handleStream() {
  //WiFiClient = server.client();

  //streamClient = server.client();

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
  String page = FPSTR(INDEX_HTML);
  page.replace("%TITLE%", String(cfg.ui.graphTitle));
  server.send(200, "text/html", page);
  //server.send_P(200, "text/html", INDEX_HTML);
  
}

void handleConfig() {
  String checked = cfg.wifi.enabled ? "checked" : "";

  String page =
    "<form method='POST' action='/save'>"
    "Device name (AP SSID):<br>"
    "<input name='dn' value='" + String(cfg.ui.deviceName) + "'><br><br>"

    "Graph title:<br>"
    "<input name='gt' value='" + String(cfg.ui.graphTitle) + "'><br><br>"
    
    "SSID:<br><input name='s' value='" + String(cfg.wifi.ssid) + "'><br>"
    "PASS:<br><input name='p' type='password'><br><br>"

    "<b>Sensor mode:</b><br>"
    "<label><input type='radio' name='mode' value='0' " + String(cfg.mode == MODE_DEMO ? "checked" : "") + "> Demo</label><br>"
    "<label><input type='radio' name='mode' value='1' " + String(cfg.mode == MODE_GPIO0 ? "checked" : "") + "> GPIO0</label><br>"
    "<label><input type='radio' name='mode' value='2' " + String(cfg.mode == MODE_GPIO2 ? "checked" : "") + "> GPIO2</label><br><br>"
    
    "<label>"
    "<input type='checkbox' name='en' " + checked + ">"
    " Enable WiFi STA"
    "</label><br><br>"
    "<label><input type='checkbox' name='f' " + String(cfg.filter.enabled ? "checked" : "") + "> Enable filter</label><br><br>"
    "Alpha (0..1):<br><input name='a' value='" + String(cfg.filter.alpha, 2) + "'><br><br>"

    "Interval (ms) [20..10000]: <br>"
    "<input name='i' value='" + String(cfg.sensor.intervalMs) + "'><br><br>"
    "<input type='submit' value='Save'></form>";
    
  server.send(200, "text/html", page);
}

/*
void handleInfo() {
  String page =
    "<br> SSID: " + String(cfg.wifi.ssid) + "<br><br>"
    "<br> Password: " + String(cfg.wifi.password) + "<br><br>"
    "<br> IP: " + WiFi.localIP().toString() + "<br><br>";
  server.send(200, "text/html", page);
}
*/

void handleInfo() {

  String page =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Device Info</title>"
    "<style>"
    "body{font-family:sans-serif;background:#f5f5f5;margin:0;padding:20px;}"
    ".card{background:#fff;padding:15px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);max-width:400px;margin:auto;}"
    "h2{text-align:center;}"
    ".row{margin:8px 0;}"
    ".label{color:#555;font-size:14px;}"
    ".value{font-size:16px;font-weight:bold;word-break:break-all;}"
    ".links{margin-top:15px;text-align:center;}"
    "a{display:inline-block;margin:5px;padding:8px 12px;background:#007BFF;color:white;text-decoration:none;border-radius:5px;}"
    "a:hover{background:#0056b3;}"
    "</style>"
    "</head><body>";

  page +=
    "<div class='card'>"
    "<h2>Device Info</h2>"

    "<div class='row'><div class='label'>Device name</div>"
    "<div class='value'>" + String(cfg.ui.deviceName) + "</div></div>"

    "<div class='row'><div class='label'>WiFi SSID</div>"
    "<div class='value'>" + String(cfg.wifi.ssid) + "</div></div>"

    "<div class='row'><div class='label'>WiFi Password</div>"
    "<div class='value'>" + String(cfg.wifi.password) + "</div></div>"

    "<div class='row'><div class='label'>IP Address</div>"
    "<div class='value'>" + WiFi.localIP().toString() + "</div></div>";

  page +=
    "<div class='links'>"
    "<a href='/'>📊&#128202; Graph</a>"
    "<a href='/config'>&#9881; Config</a>"
    "</div>"

    "</div></body></html>";

  server.send(200, "text/html", page);
}


void handleSave() {

  if (server.hasArg("dn")) {
    strncpy(cfg.ui.deviceName, server.arg("dn").c_str(), sizeof(cfg.ui.deviceName));
  }

  if (server.hasArg("gt")) {
    strncpy(cfg.ui.graphTitle , server.arg("gt").c_str(), sizeof(cfg.ui.graphTitle)-1);
    cfg.ui.graphTitle[sizeof(cfg.ui.graphTitle) - 1] = '\0';
  }

  if (server.hasArg("s")) {
    strncpy(cfg.wifi.ssid, server.arg("s").c_str(), sizeof(cfg.wifi.ssid));
  }

  if (server.hasArg("p") && server.arg("p").length() > 0) {
    strncpy(cfg.wifi.password, server.arg("p").c_str(), sizeof(cfg.wifi.password));
  }
  
  if (server.hasArg("p") && server.arg("p").length() > 0) {
    strncpy(cfg.wifi.password, server.arg("p").c_str(), sizeof(cfg.wifi.password));
  }

  if (server.hasArg("mode")) {
    cfg.mode = (SensorMode) server.arg("mode").toInt();
  }

  // зберігаємо наявність чек-бокса "en" на сторінці конфіга
  // (якщо чек-бокс вимкнений, то параметр взагалі не приходить.
  cfg.wifi.enabled = server.hasArg("en");

  // зберігаємо наявність чек-бокса "f" (фільтрування даних) на сторінці конфіга
  cfg.filter.enabled = server.hasArg("f");

  // зберігаємо значення альфа з сторінки
  if (server.hasArg("a")) {
    cfg.filter.alpha = server.arg("a").toFloat();
    if (cfg.filter.alpha < 0) cfg.filter.alpha = 0;
    if (cfg.filter.alpha > 1) cfg.filter.alpha = 1;
  }

  filterInitialized = false;

  //читаємо значення інтервала опитування датчика з сторінки:
  if (server.hasArg("i")) {
    cfg.sensor.intervalMs = server.arg("i").toInt();
  }
  // обмежуємо інтервал опитування датчика: STREAM_MIN_INTERVAL..STREAM_MAX_INTERVAL мс
  if (cfg.sensor.intervalMs < STREAM_MIN_INTERVAL) cfg.sensor.intervalMs = STREAM_MIN_INTERVAL;
  if (cfg.sensor.intervalMs > STREAM_MAX_INTERVAL) cfg.sensor.intervalMs = STREAM_MAX_INTERVAL;
  
  saveConfig();
  initSensor();
  
  server.send(200, "text/html", "Saved. Reboot device.");
}


float readTemperature()
{  
  float tempC = 0.0;
  if (!sensors) return NAN;
  
  Serial.print("Requesting temperatures...");
  sensors->requestTemperatures(); // Команда для отримання температури
  Serial.println("DONE");
  // Отримавши температуру, ми можемо надрукувати її тут.
  // Ми використовуємо функцію ByIndex,
  // Отримуємо температуру тільки з першого датчика.
  tempC = sensors->getTempCByIndex(0);

  // Перевірка, чи успішно прочитано
  if(tempC != DEVICE_DISCONNECTED_C) 
  {
    Serial.print("Temperature for the device 1 (index 0) is: ");
    Serial.println(tempC);
  } 
  else
  {
    Serial.println("Error: Could not read temperature data");
    return NAN;
  }
  return tempC;
}

// Читаємо сенсори
float readSensor() {
  switch (cfg.mode) {

    case MODE_DEMO: {
      static float v = 50;
      v += random(-3,4);
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      return v;
    }

    case MODE_GPIO0:
      return readTemperature(); // або твоя логіка

    case MODE_GPIO2:
      return readTemperature();

    default:
      return 0;
  }
}

//=============================================================

void setup() {
  
  globalInit();
  
  Serial.begin(9600);
  // Ініціалізація бібліотеки датчика температури
  
  loadConfig();

  initSensor();
  
  setupWiFi(cfg);

  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/stream", handleStream);
  server.on("/info", handleInfo);

  server.begin();
}


unsigned long lastWifiCheck = 0;
const unsigned long WIFI_INTERVAL = 5000; // 5 сек; інтервал перевірки з'єднання wifi


void loop() {
  
  server.handleClient();

  // Якщо активована необхідність з'єднання до зовнішньої точки доступу wifi
  if (cfg.wifi.enabled) {
    // читаємо таймер
    unsigned long now = millis();

    // якщо пройшов таймаут WIFI_INTERVAL, то перевіряємо з'єднання до wifi точки доступу
    if (now - lastWifiCheck >= WIFI_INTERVAL) {
      lastWifiCheck = now;

      static bool connecting = false;
      
      // перевіряємо з'єднання до зовнішньої точки доступу wifi
      wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        connecting = false;
      }
      else {
        if (!connecting) {
          WiFi.begin(cfg.wifi.ssid, cfg.wifi.password);
          connecting = true;
        }
      }
    }
  }

  // Для забезпечення SSE з'єднання при великих інтервалах опитування датчиків, посилаємо ping
  // (щоб браузер не закрив сокет по неактивності)
  
  static unsigned long lastPing = 0;

  if (millis() - lastPing > 2000) {
    if (streamClient && streamClient.connected()) {
      streamClient.println(":ping");
      streamClient.println();
    }
    lastPing = millis();
  }
   
  if (streamClient && streamClient.connected()) {
    unsigned long now = millis();
    if (now - lastSend >= cfg.sensor.intervalMs) {
      lastSend = now;

      // читаємо дані з сенсора:
      float raw = readSensor();
      // пропускаємо дані через фільтр (для злагодження графіку)
      float value = applyFilter(raw);
      
      streamClient.print("data:");
      streamClient.println(value);
      streamClient.println();

      lastPing = now;

      yield();
    }
  }

  // Звільняємо ресурс streamClient, якщо сокетне з'єднання обірвалось
  if (streamClient && !streamClient.connected()) {
    streamClient.stop();
    streamClient = WiFiClient();  // обнулюємо клієнт
  }
    
}
  
