/*
 * Kinetic Blooming Sculpture -- WiFi Controller Edition
 * Arduino Uno R4 WiFi + PCA9685 + 2x Servos + WS2812B
 *
 * HOW TO USE:
 *   1. Turn on your phone hotspot (MayPhone)
 *   2. Power the Arduino -- it connects automatically
 *   3. Open  https://angel-control.netlify.app/  on your phone
 *      Auto-redirects to http://172.20.10.10 -- no IP typing needed!
 *
 * Wiring:
 *   PCA9685 SDA  -> Arduino A4
 *   PCA9685 SCL  -> Arduino A5
 *   PCA9685 VCC  -> Arduino 5V
 *   PCA9685 GND  -> Arduino GND
 *   PCA9685 V+   -> 5V charger positive
 *   PCA9685 GND  -> Charger GND + Arduino GND
 *   Servo 1      -> CH0
 *   Servo 2      -> CH1
 *   LED DIN      -> D6
 *   LED 5V       -> Arduino 5V
 *   LED GND      -> Arduino GND
 */

#include <Adafruit_NeoPixel.h>
#include <Adafruit_PWMServoDriver.h>
#include <WiFiS3.h>
#include <Wire.h>

// ── WiFi credentials ──────────────────────────────────────────
const char *WIFI_SSID     = "MayPhone";
const char *WIFI_PASSWORD = "fcfo7zacczmf";

// Fixed static IP -- Arduino is ALWAYS at 172.20.10.10 on iPhone hotspot
// iPhone hotspot uses 172.20.10.x subnet (gateway .1, /28)
IPAddress STATIC_IP(172, 20, 10, 10);
IPAddress GATEWAY  (172, 20, 10,  1);
IPAddress SUBNET   (255, 255, 255, 240);
// ─────────────────────────────────────────────────────────────

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40, Wire);
WiFiServer server(80);

#define LED_PIN  6
#define NUM_LEDS 8
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

const int SERVO_1_CH   = 0;
const int SERVO_2_CH   = 1;
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2500;
const int positionZero = 90;
const int positionOpen = 67;

const uint8_t COLOR_CLOSED_R = 30,  COLOR_CLOSED_G = 0,   COLOR_CLOSED_B = 60;
const uint8_t COLOR_OPEN_R   = 255, COLOR_OPEN_G   = 120, COLOR_OPEN_B   = 0;

// ── State ─────────────────────────────────────────────────────
enum Mode { BREATHE, PAUSE_MODE, OPEN_MODE, MANUAL };
Mode currentMode  = BREATHE;
int  speedLevel   = 5;
int  pauseSec     = 2;
int  bloomPct     = 0;
bool stateChanged = false;
int  cycleCount   = 0;
const int cyclesBeforePause = 3;
unsigned long lastWifiCheck = 0;

// ── Embedded HTML app ─────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"/>
<title>Angel</title>
<link href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap" rel="stylesheet">
<style>
:root{--bg:#0E0C0A;--s:#1A1713;--b:#2E2A24;--t:#F0EDE8;--m:#7A7368;--a:#D4820A;--g:#3A7D5B;--r:#C0392B}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'DM Sans',-apple-system,sans-serif;background:var(--bg);color:var(--t);padding:1.25rem;max-width:420px;margin:0 auto;min-height:100vh;-webkit-font-smoothing:antialiased}
.topbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:1.75rem;padding-top:.25rem}
.title{font-size:20px;font-weight:500}.title em{font-style:italic;font-weight:300;color:var(--a)}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--m);background:var(--s);padding:5px 10px;border-radius:99px;border:1px solid var(--b)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--b);flex-shrink:0;transition:background .3s}
.dot.on{background:var(--g)}.dot.err{background:var(--r)}
.lbl{font-size:11px;text-transform:uppercase;letter-spacing:.07em;color:var(--m);margin-bottom:.65rem}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:1rem}
.mb{padding:14px 12px;border:1.5px solid var(--b);border-radius:14px;background:var(--s);cursor:pointer;text-align:left;-webkit-tap-highlight-color:transparent;transition:all .15s;width:100%}
.mb:active{opacity:.75}
.mb.on{border-color:var(--a);background:rgba(212,130,10,.12)}
.mb .ico{font-size:22px;margin-bottom:6px;display:block}
.mb .nm{font-size:14px;font-weight:500;display:block;color:var(--t)}
.mb .ds{font-size:11px;color:var(--m);margin-top:2px;display:block;line-height:1.4}
.mb.busy{opacity:.5;pointer-events:none}
.card{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1rem 1.25rem;margin-bottom:.75rem}
.row{display:flex;align-items:center;gap:12px;margin-top:.75rem}
input[type=range]{-webkit-appearance:none;flex:1;height:4px;border-radius:2px;background:var(--b);outline:none;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:var(--a);cursor:pointer;box-shadow:0 1px 6px rgba(212,130,10,.4)}
.val{font-size:14px;font-weight:500;min-width:36px;text-align:right;color:var(--t)}
.edge{font-size:12px;color:var(--m);white-space:nowrap}
.dim{opacity:.3;pointer-events:none;transition:opacity .2s}
.btn{display:block;width:100%;padding:15px;font-size:15px;font-weight:500;border:none;border-radius:14px;background:var(--a);color:#fff;cursor:pointer;font-family:inherit;letter-spacing:.01em;transition:opacity .15s,transform .1s;margin-bottom:.75rem}
.btn:active{opacity:.85;transform:scale(.98)}
.btn:disabled{opacity:.45;cursor:default;transform:none}
.toast{position:fixed;bottom:28px;left:50%;transform:translateX(-50%);font-size:13px;font-family:inherit;padding:10px 22px;border-radius:99px;background:var(--t);color:var(--bg);opacity:0;transition:opacity .25s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}.toast.err{background:var(--r);color:#fff}
</style>
</head>
<body>
<div class="topbar">
  <div class="title">Angel <em>sculpture</em></div>
  <div class="pill"><span class="dot" id="dot"></span><span id="st">connecting...</span></div>
</div>
<p class="lbl">Mode - tap to switch instantly</p>
<div class="grid">
  <button class="mb on" data-m="breathe" onclick="switchMode('breathe',this)">
    <span class="ico">&#127756;</span><span class="nm">Breathe</span><span class="ds">Auto open &amp; close</span>
  </button>
  <button class="mb" data-m="pause" onclick="switchMode('pause',this)">
    <span class="ico">&#9208;</span><span class="nm">Pause</span><span class="ds">Hold position</span>
  </button>
  <button class="mb" data-m="open" onclick="switchMode('open',this)">
    <span class="ico">&#9728;</span><span class="nm">Open</span><span class="ds">Fully open</span>
  </button>
  <button class="mb" data-m="manual" onclick="switchMode('manual',this)">
    <span class="ico">&#9881;</span><span class="nm">Manual</span><span class="ds">Set position</span>
  </button>
</div>
<div class="card">
  <p class="lbl">Bloom speed <span id="spdSending" style="color:var(--a);font-size:10px;opacity:0;transition:opacity .2s">sending...</span></p>
  <div class="row">
    <span class="edge">Slow</span>
    <input type="range" id="spd" min="1" max="10" value="5" step="1"
      oninput="V('sv',this.value)"
      onchange="quickSpeed(this.value)"/>
    <span class="edge">Fast</span>
    <span class="val" id="sv">5</span>
  </div>
</div>
<div class="card">
  <p class="lbl">Pause between cycles</p>
  <div class="row">
    <span class="edge">0s</span>
    <input type="range" id="psd" min="0" max="10" value="2" step="1" oninput="V('pv',this.value+'s')"/>
    <span class="edge">10s</span>
    <span class="val" id="pv">2s</span>
  </div>
</div>
<div class="card dim" id="mc">
  <p class="lbl">Bloom position - manual only</p>
  <div class="row">
    <span class="edge">Closed</span>
    <input type="range" id="bl" min="0" max="100" value="0" step="1" oninput="V('bv',this.value+'%')"/>
    <span class="edge">Open</span>
    <span class="val" id="bv">0%</span>
  </div>
</div>
<button class="btn" id="applyBtn" onclick="applySliders()">Apply speed &amp; position</button>
<div class="toast" id="toast"></div>
<script>
let mode='breathe',busy=false;
const V=(id,v)=>document.getElementById(id).textContent=v;
function setStatus(ok,txt){
  document.getElementById('dot').className='dot'+(ok===true?' on':ok===false?' err':'');
  document.getElementById('st').textContent=txt;
}
function showToast(msg,err){
  const t=document.getElementById('toast');
  t.textContent=msg;
  t.className='toast'+(err?' err':'')+' show';
  setTimeout(()=>t.className='toast'+(err?' err':''),2500);
}
function getParams(){
  return new URLSearchParams({mode:mode,speed:document.getElementById('spd').value,pause:document.getElementById('psd').value,bloom:document.getElementById('bl').value});
}
async function quickSpeed(v){
  const ind=document.getElementById('spdSending');
  ind.style.opacity='1';
  try{
    await fetch('/speed?v='+v,{signal:AbortSignal.timeout(2000)});
    setStatus(true,'connected');
  }catch(e){setStatus(false,'lost connection');}
  ind.style.opacity='0';
}
async function send(p,silent){
  try{
    const r=await fetch('/control?'+p,{signal:AbortSignal.timeout(5000)});
    if(r.ok){setStatus(true,'connected');if(!silent)showToast('Applied!');}
    else{setStatus(false,'error');if(!silent)showToast('Error from sculpture',true);}
  }catch(e){setStatus(false,'lost connection');if(!silent)showToast('Cannot reach sculpture',true);}
}
async function switchMode(m,el){
  if(busy)return;
  busy=true;
  mode=m;
  document.querySelectorAll('.mb').forEach(b=>b.classList.toggle('on',b.dataset.m===m));
  document.getElementById('mc').className='card'+(m==='manual'?'':' dim');
  el.classList.add('busy');
  await send(getParams(),false);
  el.classList.remove('busy');
  busy=false;
}
async function applySliders(){
  const btn=document.getElementById('applyBtn');
  btn.disabled=true;btn.textContent='Applying...';
  await send(getParams(),false);
  btn.disabled=false;btn.textContent='Apply speed & position';
}
async function sync(){
  try{
    const r=await fetch('/status',{signal:AbortSignal.timeout(4000)});
    if(r.ok){
      const d=await r.json();
      setStatus(true,'connected');
      mode=d.mode;
      document.querySelectorAll('.mb').forEach(b=>b.classList.toggle('on',b.dataset.m===d.mode));
      document.getElementById('mc').className='card'+(d.mode==='manual'?'':' dim');
      document.getElementById('spd').value=d.speed;V('sv',d.speed);
      document.getElementById('psd').value=d.pause;V('pv',d.pause+'s');
      document.getElementById('bl').value=d.bloom;V('bv',d.bloom+'%');
    }
  }catch(e){setStatus(false,'lost connection');}
}
sync();
setInterval(sync,10000);
</script>
</body>
</html>)rawhtml";

// ── WiFi ──────────────────────────────────────────────────────
void connectWiFi() {
  Serial.println("\nConnecting to hotspot...");
  Serial.print("SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.disconnect();
  delay(500);

  // Set fixed static IP before connecting
  WiFi.config(STATIC_IP, GATEWAY, SUBNET);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 30000) {
      Serial.println("\nTimeout - retrying...");
      WiFi.disconnect();
      delay(2000);
      WiFi.config(STATIC_IP, GATEWAY, SUBNET);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }

  Serial.println();
  Serial.println("===================================");
  Serial.println("   CONNECTED  -  FIXED IP ACTIVE");
  Serial.println("===================================");
  Serial.println("   http://172.20.10.10");
  Serial.println("   Open angel-control.netlify.app");
  Serial.println("   on your phone -- no IP needed!");
  Serial.println("===================================");

  server.begin();
}

// ── Servo & LED helpers ───────────────────────────────────────
int currentStepDelay() { return map(speedLevel, 1, 10, 20, 1); }

uint16_t angleToPulse(int angle) {
  angle = constrain(angle, 0, 180);
  float us = SERVO_MIN_US + (angle / 180.0f) * (SERVO_MAX_US - SERVO_MIN_US);
  return (uint16_t)((us / 20000.0f) * 4096);
}

void writeServo(uint8_t ch, int angle) {
  pca.setPWM(ch, 0, angleToPulse(angle));
}

void setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

void updateLEDs(float bloom) {
  float bright = 0.4f + 0.6f * bloom;
  setAllLEDs(
    (uint8_t)((COLOR_CLOSED_R + (COLOR_OPEN_R - COLOR_CLOSED_R) * bloom) * bright),
    (uint8_t)((COLOR_CLOSED_G + (COLOR_OPEN_G - COLOR_CLOSED_G) * bloom) * bright),
    (uint8_t)((COLOR_CLOSED_B + (COLOR_OPEN_B - COLOR_CLOSED_B) * bloom) * bright));
}

void fadeTo(uint8_t tr, uint8_t tg, uint8_t tb, int durationMs) {
  uint32_t c = strip.getPixelColor(0);
  uint8_t sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
  const int STEPS = 40;
  for (int i = 1; i <= STEPS; i++) {
    float t = (float)i / STEPS;
    setAllLEDs(sr + (uint8_t)((tr - sr) * t),
               sg + (uint8_t)((tg - sg) * t),
               sb + (uint8_t)((tb - sb) * t));
    delay(durationMs / STEPS);
  }
}

// ── Poll server (safe to call anywhere) ──────────────────────
void handleClient(WiFiClient &client); // forward declaration

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client = server.available();
  if (client) handleClient(client);
}

void ultraSmoothMoveMirrored(int startPos, int endPos) {
  float distance = abs(endPos - startPos);
  int direction = (endPos > startPos) ? 1 : -1;
  int steps = (int)(distance / 0.05f);
  if (steps < 1) steps = 1;

  int lastPos1 = startPos, lastPos2 = 90 - startPos;
  uint8_t lastR = 0, lastG = 0, lastB = 0;

  for (int i = 0; i <= steps; i++) {
    if (stateChanged) return;
    if (i % 10 == 0) pollServer();  // poll often so speed changes feel instant

    float p = (float)i / steps;
    float ep = (p < 0.5f)
      ? 8.0f * p * p * p * p
      : -8.0f * (p-1.0f)*(p-1.0f)*(p-1.0f)*(p-1.0f) + 1.0f;

    int cp1 = (int)round(startPos + distance * ep * direction);
    int cp2 = 90 - cp1;
    if (cp1 != lastPos1) { writeServo(SERVO_1_CH, cp1); lastPos1 = cp1; }
    if (cp2 != lastPos2) { writeServo(SERVO_2_CH, cp2); lastPos2 = cp2; }

    float bloom = constrain((float)(positionZero - cp1) /
                            (float)(positionZero - positionOpen), 0.0f, 1.0f);
    uint8_t r = (uint8_t)((COLOR_CLOSED_R+(COLOR_OPEN_R-COLOR_CLOSED_R)*bloom)*(0.4f+0.6f*bloom));
    uint8_t g = (uint8_t)((COLOR_CLOSED_G+(COLOR_OPEN_G-COLOR_CLOSED_G)*bloom)*(0.4f+0.6f*bloom));
    uint8_t b = (uint8_t)((COLOR_CLOSED_B+(COLOR_OPEN_B-COLOR_CLOSED_B)*bloom)*(0.4f+0.6f*bloom));
    if (r != lastR || g != lastG || b != lastB) {
      setAllLEDs(r, g, b); lastR = r; lastG = g; lastB = b;
    }
    delay(currentStepDelay());
  }
  writeServo(SERVO_1_CH, endPos);
  writeServo(SERVO_2_CH, 90 - endPos);
  updateLEDs(endPos == positionOpen ? 1.0f : 0.0f);
}

// ── HTTP server ───────────────────────────────────────────────
String parseParam(String req, String key) {
  int idx = req.indexOf(key + "=");
  if (idx < 0) return "";
  int start = idx + key.length() + 1;
  int end = req.indexOf('&', start);
  if (end < 0) end = req.indexOf(' ', start);
  if (end < 0) end = req.length();
  return req.substring(start, end);
}

void handleClient(WiFiClient &client) {
  String req = "";
  unsigned long t = millis();
  while (client.connected() && millis() - t < 1500) {
    if (client.available()) {
      char c = client.read();
      req += c;
      if (req.endsWith("\r\n\r\n")) break;
    }
  }

  bool isRoot    = req.indexOf("GET / ")    >= 0 || req.indexOf("GET /\r") >= 0;
  bool isStatus  = req.indexOf("GET /status")  >= 0;
  bool isControl = req.indexOf("GET /control") >= 0;
  bool isSpeed   = req.indexOf("GET /speed")   >= 0;
  bool isOptions = req.indexOf("OPTIONS ")      >= 0;

  if (isOptions) {
    client.println("HTTP/1.1 204 No Content");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Access-Control-Allow-Methods: GET, POST, OPTIONS");
    client.println("Access-Control-Allow-Headers: Content-Type, Access-Control-Allow-Private-Network");
    client.println("Access-Control-Allow-Private-Network: true");
    client.println("Connection: close");
    client.println();
    client.stop();
    return;
  }

  // Fast /speed endpoint -- updates speedLevel immediately, minimal response
  if (isSpeed) {
    String vStr = parseParam(req, "v");
    if (vStr.length()) speedLevel = constrain(vStr.toInt(), 1, 10);
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println("Content-Length: 2");
    client.println();
    client.print("ok");
    client.stop();
    return;
  }

  if (isRoot) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    const int CHUNK = 64;
    int len = strlen_P(HTML_PAGE);
    for (int i = 0; i < len; i += CHUNK) {
      char buf[CHUNK + 1];
      int n = min(CHUNK, len - i);
      memcpy_P(buf, HTML_PAGE + i, n);
      buf[n] = '\0';
      client.print(buf);
    }
    client.stop();
    return;
  }

  if (isControl) {
    String mStr = parseParam(req, "mode");
    String sStr = parseParam(req, "speed");
    String pStr = parseParam(req, "pause");
    String bStr = parseParam(req, "bloom");

    if      (mStr == "breathe") currentMode = BREATHE;
    else if (mStr == "pause")   currentMode = PAUSE_MODE;
    else if (mStr == "open")    currentMode = OPEN_MODE;
    else if (mStr == "manual")  currentMode = MANUAL;

    if (sStr.length()) speedLevel = constrain(sStr.toInt(), 1, 10);
    if (pStr.length()) pauseSec   = constrain(pStr.toInt(), 0, 10);
    if (bStr.length()) bloomPct   = constrain(bStr.toInt(), 0, 100);

    stateChanged = true;
    Serial.print("CMD mode="); Serial.print(mStr);
    Serial.print(" speed=");   Serial.print(speedLevel);
    Serial.print(" pause=");   Serial.println(pauseSec);
  }

  if (!isStatus && !isControl) {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
    client.stop();
    return;
  }

  String modeStr = "breathe";
  if      (currentMode == PAUSE_MODE) modeStr = "pause";
  else if (currentMode == OPEN_MODE)  modeStr = "open";
  else if (currentMode == MANUAL)     modeStr = "manual";

  String body = "{\"mode\":\"" + modeStr + "\","
                "\"speed\":"  + String(speedLevel) + ","
                "\"pause\":"  + String(pauseSec)   + ","
                "\"bloom\":"  + String(bloomPct)   + "}";

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Private-Network: true");
  client.println("Connection: close");
  client.println("Content-Length: " + String(body.length()));
  client.println();
  client.print(body);
  client.stop();
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  delay(1500);

  Wire.begin();
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);
  delay(10);

  writeServo(SERVO_1_CH, positionZero);
  writeServo(SERVO_2_CH, 90 - positionZero);

  strip.begin();
  strip.setBrightness(180);
  strip.show();
  setAllLEDs(COLOR_CLOSED_R, COLOR_CLOSED_G, COLOR_CLOSED_B);

  connectWiFi();
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost - reconnecting...");
      connectWiFi();
    }
  }

  pollServer();

  if (stateChanged) {
    stateChanged = false;
    if (currentMode == OPEN_MODE)
      ultraSmoothMoveMirrored(positionZero, positionOpen);
    if (currentMode == BREATHE)
      cycleCount = 0;
    return;
  }

  if (currentMode == BREATHE) {
    cycleCount++;
    Serial.print("Cycle #"); Serial.println(cycleCount);

    for (int s = 1; s <= 3; s++) {
      if (stateChanged || currentMode != BREATHE) return;
      ultraSmoothMoveMirrored(positionZero, positionOpen);
      delay(s == 3 ? 1500 : 1000);
      if (stateChanged || currentMode != BREATHE) return;
      ultraSmoothMoveMirrored(positionOpen, positionZero);
      delay(1000);
      pollServer();
    }

    if (cycleCount >= cyclesBeforePause) {
      fadeTo(COLOR_CLOSED_R / 4, COLOR_CLOSED_G / 4, COLOR_CLOSED_B / 4, 60);
      unsigned long t0 = millis();
      while (millis() - t0 < 3800) {
        pollServer();
        if (stateChanged) return;
        delay(50);
      }
      fadeTo(COLOR_CLOSED_R, COLOR_CLOSED_G, COLOR_CLOSED_B, 60);
      cycleCount = 0;
    } else {
      unsigned long t0 = millis();
      while (millis() - t0 < (unsigned long)(pauseSec * 1000)) {
        pollServer();
        if (stateChanged) return;
        delay(50);
      }
    }

  } else if (currentMode == PAUSE_MODE) {
    pollServer(); delay(100);

  } else if (currentMode == OPEN_MODE) {
    pollServer(); delay(100);

  } else if (currentMode == MANUAL) {
    int targetAngle = map(bloomPct, 0, 100, positionZero, positionOpen);
    writeServo(SERVO_1_CH, targetAngle);
    writeServo(SERVO_2_CH, 90 - targetAngle);
    updateLEDs((float)bloomPct / 100.0f);
    pollServer(); delay(100);
  }
}
