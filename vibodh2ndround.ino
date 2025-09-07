#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- WiFi AP Mode ----------
const char* ssid = "GasMonitorAP";
const char* password = "12345678";

// ---------- LCD Screen ----------
LiquidCrystal_I2C lcd(0x3B, 16, 2); 
int lcdSensorIndex = 0;

// ---------- MQ Sensor Pins ----------
#define MQ2_PIN   33 // Smoke
#define MQ4_PIN   32 // Methane
#define MQ5_PIN   35 // LPG
#define MQ7_PIN   34 // Carbon Monoxide
#define MQ135_PIN 36 // Air Quality (VP)

// ---------- Constants for Calculation ----------
const float RL_VALUE = 5.0;  // Load resistor value in kΩ (standard on most modules)
const float V_SUPPLY = 5.0;  // Supply voltage for the sensor module (usually 5V)
const int   ADC_MAX = 4095;
const float ADC_REF = 3.3; // ADC reference voltage on ESP32

// ---------- Ro Calibration Values (Resistance in Clean Air) ----------
// These are typical starting points. For best accuracy, you should calibrate these values
// by running the sensors in clean air for several hours and finding the average Rs value.
float Ro_MQ2   = 9.8;  // kΩ
float Ro_MQ4   = 4.4;  // kΩ
float Ro_MQ5   = 6.5;  // kΩ
float Ro_MQ7   = 27.0; // kΩ
float Ro_MQ135 = 76.0; // kΩ

// ---------- Curve-fit Parameters from Your Datasheets (Rs/Ro = a * ppm^m) ----------
// This formula is derived from the log-log scale graphs in the datasheets.
struct MQCurve { float a; float m; const char* gas; float max_ppm; };
MQCurve mq2_curve   = {102.73, -0.45, "Smoke", 1000};
MQCurve mq4_curve   = {1012.7, -2.78, "Methane", 5000};
MQCurve mq5_curve   = {97.65,  -0.64, "LPG", 2000};
MQCurve mq7_curve   = {99.05,  -1.5,  "CO", 1500};
MQCurve mq135_curve = {110.47, -2.76, "AirQual", 500};

// ---------- Web Server ----------
WebServer server(80);

struct MQReading {
  float volts;
  float Rs;
  float ratio;
  float ppm;
};

// --- Global variables to hold the latest sensor readings ---
MQReading mq2, mq4, mq5, mq7, mq135;

// --- Millis() timer for non-blocking updates ---
unsigned long previousMillis = 0;
const long interval = 2000; // 2 seconds

// Function to calculate PPM from Rs/Ro ratio
float getPPM(float ratio, MQCurve curve) {
  if (ratio <= 0) return 0.0;
  return pow(10, (log10(ratio) - log10(curve.a)) / curve.m);
}

// Function to read a sensor and perform all calculations
MQReading readMQ(int pin, float Ro) {
  MQReading r;
  int adc_raw = analogRead(pin);
  r.volts = (adc_raw / (float)ADC_MAX) * ADC_REF;
  r.Rs = RL_VALUE * (V_SUPPLY - r.volts) / max(0.001f, r.volts);
  r.ratio = (Ro > 0) ? (r.Rs / Ro) : 0.0;
  return r;
}

// ---- JSON API to send data to the webpage ----
String buildJSON() {
  String json = "{";
  json += "\"mq2\":{\"gas\":\"" + String(mq2_curve.gas) + "\",\"ratio\":" + String(mq2.ratio,2) + ",\"ppm\":" + String(mq2.ppm,1) + ",\"max_ppm\":"+String(mq2_curve.max_ppm)+"},";
  json += "\"mq4\":{\"gas\":\"" + String(mq4_curve.gas) + "\",\"ratio\":" + String(mq4.ratio,2) + ",\"ppm\":" + String(mq4.ppm,1) + ",\"max_ppm\":"+String(mq4_curve.max_ppm)+"},";
  json += "\"mq5\":{\"gas\":\"" + String(mq5_curve.gas) + "\",\"ratio\":" + String(mq5.ratio,2) + ",\"ppm\":" + String(mq5.ppm,1) + ",\"max_ppm\":"+String(mq5_curve.max_ppm)+"},";
  json += "\"mq7\":{\"gas\":\"" + String(mq7_curve.gas) + "\",\"ratio\":" + String(mq7.ratio,2) + ",\"ppm\":" + String(mq7.ppm,1) + ",\"max_ppm\":"+String(mq7_curve.max_ppm)+"},";
  json += "\"mq135\":{\"gas\":\"" + String(mq135_curve.gas) + "\",\"ratio\":" + String(mq135.ratio,2) + ",\"ppm\":" + String(mq135.ppm,1) + ",\"max_ppm\":"+String(mq135_curve.max_ppm)+"}";
  json += "}";
  return json;
}

// ---- Webpage UI ----
String buildHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Gas Monitor Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <link href='https://fonts.googleapis.com/css2?family=Poppins:wght@400;600&display=swap' rel='stylesheet'>
  <style>
    body { font-family: 'Poppins', sans-serif; background-color: #1a1c20; color: #e0e0e0; margin: 0; padding: 10px; }
    h1 { color: #4a90e2; text-align: center; font-size: 1.5em; margin-bottom: 10px; }
    .container { display: grid; grid-template-columns: 1fr; gap: 10px; }
    .sensor-card { background-color: #2c2f36; border-radius: 12px; padding: 15px; box-shadow: 0 4px 15px rgba(0,0,0,0.2); }
    .title { font-size: 1em; font-weight: bold; color: #cccccc; }
    .ppm-container { display: flex; align-items: baseline; justify-content: space-between; margin: 5px 0; }
    .ppm { font-size: 2em; font-weight: 600; color: #50e3c2; }
    .details { font-size: 0.8em; color: #888; }
    .bar-container { background-color: #1a1c20; border-radius: 5px; height: 10px; overflow: hidden; }
    .bar { background-color: #4a90e2; height: 100%; width: 0%; border-radius: 5px; transition: width 0.5s ease-in-out; }
  </style>
</head>
<body>
  <h1>Multi-Gas Sensor Dashboard</h1>
  <div class="container">
    <div class="sensor-card"><div class="title" id="mq2_gas">--</div><div class="ppm-container"><div class="ppm" id="mq2_ppm">-- ppm</div><div class="details">Rs/Ro: <span id="mq2_ratio">-</span></div></div><div class="bar-container"><div class="bar" id="mq2_bar"></div></div></div>
    <div class="sensor-card"><div class="title" id="mq4_gas">--</div><div class="ppm-container"><div class="ppm" id="mq4_ppm">-- ppm</div><div class="details">Rs/Ro: <span id="mq4_ratio">-</span></div></div><div class="bar-container"><div class="bar" id="mq4_bar"></div></div></div>
    <div class="sensor-card"><div class="title" id="mq5_gas">--</div><div class="ppm-container"><div class="ppm" id="mq5_ppm">-- ppm</div><div class="details">Rs/Ro: <span id="mq5_ratio">-</span></div></div><div class="bar-container"><div class="bar" id="mq5_bar"></div></div></div>
    <div class="sensor-card"><div class="title" id="mq7_gas">--</div><div class="ppm-container"><div class="ppm" id="mq7_ppm">-- ppm</div><div class="details">Rs/Ro: <span id="mq7_ratio">-</span></div></div><div class="bar-container"><div class="bar" id="mq7_bar"></div></div></div>
    <div class="sensor-card"><div class="title" id="mq135_gas">--</div><div class="ppm-container"><div class="ppm" id="mq135_ppm">-- ppm</div><div class="details">Rs/Ro: <span id="mq135_ratio">-</span></div></div><div class="bar-container"><div class="bar" id="mq135_bar"></div></div></div>
  </div>
  <script>
    async function updateData(){
      try {
        const res = await fetch('/data');
        if (!res.ok) return;
        const j = await res.json();
        for (let key in j){
          let data = j[key];
          document.getElementById(key+"_gas").textContent = "MQ-" + key.slice(2) + " (" + data.gas + ")";
          document.getElementById(key+"_ppm").textContent = data.ppm + " ppm";
          document.getElementById(key+"_ratio").textContent = data.ratio;
          let bar_width = Math.min(100, (data.ppm / data.max_ppm) * 100);
          document.getElementById(key+"_bar").style.width = bar_width + "%";
        }
      } catch(e){ console.log("Update error:", e); }
    }
    setInterval(updateData, 2000);
    window.onload = updateData;
  </script>
</body>
</html>
)rawliteral";
  return html;
}

void handleRoot() { server.send(200, "text/html", buildHTML()); }
void handleData() { server.send(200, "application/json", buildJSON()); }

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Gas Monitor");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(2000);
  lcd.clear();
  
  WiFi.softAP(ssid, password);
  Serial.print("AP IP Address: "); Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  
  Serial.println("Web server started! Please allow 1-2 minutes for sensors to warm up.");
}

void loop() {
  server.handleClient(); // Keep the web server responsive

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    // Read all sensors and calculate PPM
    mq2   = readMQ(MQ2_PIN, Ro_MQ2);
    mq2.ppm = getPPM(mq2.ratio, mq2_curve);
    
    mq4   = readMQ(MQ4_PIN, Ro_MQ4);
    mq4.ppm = getPPM(mq4.ratio, mq4_curve);
    
    mq5   = readMQ(MQ5_PIN, Ro_MQ5);
    mq5.ppm = getPPM(mq5.ratio, mq5_curve);
    
    mq7   = readMQ(MQ7_PIN, Ro_MQ7);
    mq7.ppm = getPPM(mq7.ratio, mq7_curve);

    mq135 = readMQ(MQ135_PIN, Ro_MQ135);
    mq135.ppm = getPPM(mq135.ratio, mq135_curve);

    // Update LCD with one sensor reading per cycle
    lcd.clear();
    lcd.setCursor(0, 0);
    switch (lcdSensorIndex) {
      case 0: lcd.print("MQ-2 (Smoke):"); lcd.setCursor(0, 1); lcd.print(mq2.ppm, 1); lcd.print(" ppm"); break;
      case 1: lcd.print("MQ-4 (Methane):"); lcd.setCursor(0, 1); lcd.print(mq4.ppm, 1); lcd.print(" ppm"); break;
      case 2: lcd.print("MQ-5 (LPG):"); lcd.setCursor(0, 1); lcd.print(mq5.ppm, 1); lcd.print(" ppm"); break;
      case 3: lcd.print("MQ-7 (CO):"); lcd.setCursor(0, 1); lcd.print(mq7.ppm, 1); lcd.print(" ppm"); break;
      case 4: lcd.print("MQ135 AirQual:"); lcd.setCursor(0, 1); lcd.print(mq135.ppm, 1); lcd.print(" ppm"); break;
    }
    lcdSensorIndex = (lcdSensorIndex + 1) % 5; // Cycle from 0 to 4
  }
}