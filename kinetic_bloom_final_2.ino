/*
 * Kinetic Blooming Sculpture -- Conflict-Detecting Research Edition
 * Arduino Uno R4 WiFi + PCA9685 + 2x Servos + WS2812B + KY-037 Mic
 *
 * HOW TO USE:
 *   1. Turn on your phone hotspot (MayPhone)
 *   2. Power the Arduino
 *   3. Open  https://angel-control.netlify.app/  -- auto-connects!
 *   4. For LLM detection: open /listen page on your phone
 *
 * KY-037 Wiring:
 *   KY-037 VCC  -> Arduino 5V
 *   KY-037 GND  -> Arduino GND
 *   KY-037 A0   -> Arduino A0   (analog level)
 *   KY-037 D0   -> not used
 *   (Adjust blue potentiometer for sensitivity)
 *
 * PCA9685 Wiring:
 *   SDA -> A4 | SCL -> A5 | VCC -> 5V | GND -> GND
 *   V+  -> 5V charger | Servo1 -> CH0 | Servo2 -> CH1
 *   LED DIN -> D6 | LED 5V -> 5V | LED GND -> GND
 */

#include <Adafruit_NeoPixel.h>
#include <Adafruit_PWMServoDriver.h>
#include <WiFiS3.h>
#include <Wire.h>

// ── WiFi credentials ──────────────────────────────────────────
const char *WIFI_SSID     = "MayPhone";
const char *WIFI_PASSWORD = "fcfo7zacczmf";

// Fixed static IP -- always at 172.20.10.10 on iPhone hotspot
IPAddress STATIC_IP(172, 20, 10, 10);
IPAddress GATEWAY  (172, 20, 10,  1);
IPAddress SUBNET   (255, 255, 255, 240);

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40, Wire);
WiFiServer server(80);

#define LED_PIN  6
#define NUM_LEDS 8
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

const int SERVO_1_CH   = 0;
const int SERVO_2_CH   = 1;
const int SERVO_MIN_US = 500;
const int SERVO_MAX_US = 2500;
const int positionZero = 90;   // closed position
const int positionOpen = 67;   // open position

const uint8_t COLOR_CLOSED_R = 30,  COLOR_CLOSED_G = 0,   COLOR_CLOSED_B = 60;
const uint8_t COLOR_OPEN_R   = 255, COLOR_OPEN_G   = 120, COLOR_OPEN_B   = 0;
const uint8_t COLOR_CONFLICT_R = 60, COLOR_CONFLICT_G = 0, COLOR_CONFLICT_B = 5;

// ── Microphone (KY-037 on A0) ─────────────────────────────────
#define MIC_PIN             A0
#define VOL_THRESHOLD       700     // 0-1023; turn pot clockwise for more sensitive
#define VOL_CONFLICT_MS     3000    // stay above threshold 3s to auto-trigger
#define CONFLICT_AUTO_REOPEN_MS 20000  // auto-reopen after 20s

// ── State ─────────────────────────────────────────────────────
enum Mode { BREATHE, PAUSE_MODE, OPEN_MODE, MANUAL, CONFLICT_MODE };
Mode currentMode  = BREATHE;
int  currentPos   = positionZero;  // track actual servo 1 position
int  speedLevel   = 5;
int  pauseSec     = 2;
int  bloomPct     = 0;
bool stateChanged = false;
int  cycleCount   = 0;
const int cyclesBeforePause = 3;
unsigned long lastWifiCheck = 0;

// Volume detection state
unsigned long volHighSince    = 0;
bool          volElevated     = false;
unsigned long lastVolumeCheck = 0;
unsigned long conflictStartMs = 0;

// Volume rolling average buffer (30 samples for 3 seconds at 100ms intervals)
int volSamples[30]            = {0};
int volSampleIdx              = 0;
int volSampleCount            = 0;
long volSampleSum             = 0;

// ── Event Log (circular buffer, 30 events) ────────────────────
struct ConflictEvent {
  unsigned long startMs;
  unsigned long durationMs;
  char          src[8];    // "vol", "llm", "manual"
  uint8_t       score;     // LLM score * 100 (0-100)
};
ConflictEvent evtLog[30];
int evtTotal = 0;  // total events ever (capped at 30 for index math)
int evtHead  = 0;  // next write position

void logConflictEvent(const char* src, int score100) {
  int idx = evtHead % 30;
  evtLog[idx].startMs    = millis();
  evtLog[idx].durationMs = 0;
  evtLog[idx].score      = (uint8_t)constrain(score100, 0, 100);
  strncpy(evtLog[idx].src, src, 7);
  evtLog[idx].src[7] = '\0';
  evtHead = (evtHead + 1) % 30;
  if (evtTotal < 30) evtTotal++;
  conflictStartMs = millis();
}

void closeLastEventDuration() {
  if (evtTotal == 0) return;
  int idx = (evtHead - 1 + 30) % 30;
  if (evtLog[idx].durationMs == 0) {
    evtLog[idx].durationMs = millis() - evtLog[idx].startMs;
  }
}

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
.topbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:1.5rem;padding-top:.25rem}
.title{font-size:20px;font-weight:500}.title em{font-style:italic;font-weight:300;color:var(--a)}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--m);background:var(--s);padding:5px 10px;border-radius:99px;border:1px solid var(--b)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--b);flex-shrink:0;transition:background .3s}
.dot.on{background:var(--g)}.dot.err{background:var(--r)}
.conflict-banner{display:none;background:rgba(192,57,43,.18);border:1px solid var(--r);border-radius:12px;padding:.75rem 1rem;margin-bottom:.75rem;font-size:13px;color:var(--r);text-align:center;font-weight:500}
.conflict-banner.show{display:block}
.lbl{font-size:11px;text-transform:uppercase;letter-spacing:.07em;color:var(--m);margin-bottom:.65rem}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:1rem}
.mb{padding:14px 12px;border:1.5px solid var(--b);border-radius:14px;background:var(--s);cursor:pointer;text-align:left;-webkit-tap-highlight-color:transparent;transition:all .15s;width:100%}
.mb:active{opacity:.75}
.mb.on{border-color:var(--a);background:rgba(212,130,10,.12)}
.mb.conflict-on{border-color:var(--r)!important;background:rgba(192,57,43,.12)!important}
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
.btn{display:block;width:100%;padding:15px;font-size:15px;font-weight:500;border:none;border-radius:14px;background:var(--a);color:#fff;cursor:pointer;font-family:inherit;transition:opacity .15s,transform .1s;margin-bottom:.75rem}
.btn:active{opacity:.85;transform:scale(.98)}
.btn:disabled{opacity:.45;cursor:default;transform:none}
.llm-btn{background:#5436DA;margin-bottom:1rem}
.sec{font-size:13px;font-weight:500;color:var(--m);margin:1.25rem 0 .75rem;display:flex;align-items:center;gap:8px}
.sec::after{content:'';flex:1;height:1px;background:var(--b)}
.evt-row{display:flex;align-items:center;gap:8px;padding:.6rem 0;border-bottom:1px solid var(--b);font-size:12px}
.evt-row:last-child{border-bottom:none}
.evt-src{padding:2px 7px;border-radius:99px;font-size:11px;font-weight:500}
.evt-src.llm{background:rgba(84,54,218,.25);color:#8B76E8}
.evt-src.vol{background:rgba(192,57,43,.2);color:var(--r)}
.evt-src.manual{background:rgba(212,130,10,.15);color:var(--a)}
.evt-time{color:var(--m);flex:1}
.evt-dur{color:var(--m)}
.no-events{text-align:center;color:var(--m);font-size:13px;padding:1rem 0}
.toast{position:fixed;bottom:28px;left:50%;transform:translateX(-50%);font-size:13px;font-family:inherit;padding:10px 22px;border-radius:99px;background:var(--t);color:var(--bg);opacity:0;transition:opacity .25s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}.toast.err{background:var(--r);color:#fff}
</style>
</head>
<body>
<div class="topbar">
  <div class="title">Angel <em>sculpture</em></div>
  <div class="pill"><span class="dot" id="dot"></span><span id="st">connecting...</span></div>
</div>
<div class="conflict-banner" id="conflictBanner">&#9888; Conflict detected - flower closed</div>
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
      oninput="V('sv',this.value)" onchange="quickSpeed(this.value)"/>
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
<button class="btn llm-btn" onclick="window.location.href='https://angel-control.netlify.app/listen/'">&#127908; Start LLM Detection</button>
<div class="sec">Conflict Events</div>
<div id="evtList"><div class="no-events">No conflicts yet this session</div></div>
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
  t.textContent=msg;t.className='toast'+(err?' err':'')+' show';
  setTimeout(()=>t.className='toast'+(err?' err':''),2500);
}
function getParams(){
  return new URLSearchParams({mode,speed:document.getElementById('spd').value,pause:document.getElementById('psd').value,bloom:document.getElementById('bl').value});
}
async function quickSpeed(v){
  const ind=document.getElementById('spdSending');
  ind.style.opacity='1';
  try{await fetch('/speed?v='+v,{signal:AbortSignal.timeout(2000)});setStatus(true,'connected');}
  catch(e){setStatus(false,'lost connection');}
  ind.style.opacity='0';
}
async function send(p,silent){
  try{
    const r=await fetch('/control?'+p,{signal:AbortSignal.timeout(5000)});
    if(r.ok){setStatus(true,'connected');if(!silent)showToast('Applied!');}
    else{setStatus(false,'error');if(!silent)showToast('Error',true);}
  }catch(e){setStatus(false,'lost connection');if(!silent)showToast('Cannot reach sculpture',true);}
}
async function switchMode(m,el){
  if(busy)return;
  busy=true;mode=m;
  document.querySelectorAll('.mb').forEach(b=>{b.classList.remove('on','conflict-on');});
  el.classList.add('on');
  document.getElementById('mc').className='card'+(m==='manual'?'':' dim');
  document.getElementById('conflictBanner').classList.remove('show');
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
function renderEvents(events){
  const el=document.getElementById('evtList');
  if(!events||events.length===0){el.innerHTML='<div class="no-events">No conflicts yet</div>';return;}
  const now=Date.now();
  el.innerHTML=events.slice().reverse().map(e=>{
    const srcCls=e.src==='llm'?'llm':e.src==='vol'?'vol':'manual';
    const srcLabel=e.src==='llm'?'LLM':e.src==='vol'?'VOL':'MAN';
    const dur=e.dur>0?(e.dur/1000).toFixed(1)+'s':'ongoing';
    const score=e.src==='llm'&&e.score>0?` ${e.score}%`:'';
    return `<div class="evt-row">
      <span class="evt-src ${srcCls}">${srcLabel}${score}</span>
      <span class="evt-time">${new Date(now-(window._startMs||now)+e.t).toLocaleTimeString()}</span>
      <span class="evt-dur">${dur}</span>
    </div>`;
  }).join('');
}
async function sync(){
  try{
    const r=await fetch('/status',{signal:AbortSignal.timeout(4000)});
    if(r.ok){
      const d=await r.json();
      setStatus(true,'connected');
      mode=d.mode;
      document.querySelectorAll('.mb').forEach(b=>{
        b.classList.remove('on','conflict-on');
        if(b.dataset.m===d.mode) b.classList.add(d.mode==='conflict'?'conflict-on':'on');
      });
      document.getElementById('mc').className='card'+(d.mode==='manual'?'':' dim');
      document.getElementById('conflictBanner').classList.toggle('show',d.mode==='conflict');
      document.getElementById('spd').value=d.speed;V('sv',d.speed);
      document.getElementById('psd').value=d.pause;V('pv',d.pause+'s');
      document.getElementById('bl').value=d.bloom;V('bv',d.bloom+'%');
    }
  }catch(e){setStatus(false,'lost connection');}
  try{
    const r=await fetch('/events',{signal:AbortSignal.timeout(3000)});
    if(r.ok){const d=await r.json();if(!window._startMs&&d.length)window._startMs=d[0].t;renderEvents(d);}
  }catch(e){}
}
sync();setInterval(sync,8000);
</script>
</body>
</html>)rawhtml";

// ── WiFi ──────────────────────────────────────────────────────
void connectWiFi() {
  Serial.println("\nConnecting to hotspot...");
  WiFi.disconnect();
  delay(500);
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
  Serial.println("   CONNECTED  -  172.20.10.10");
  Serial.println("   Open netlify app on phone!");
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
  const int STEPS = 30;
  for (int i = 1; i <= STEPS; i++) {
    float t = (float)i / STEPS;
    setAllLEDs(sr + (uint8_t)((tr - sr) * t),
               sg + (uint8_t)((tg - sg) * t),
               sb + (uint8_t)((tb - sb) * t));
    delay(durationMs / STEPS);
  }
}

// ── Volume monitoring ─────────────────────────────────────────
void checkVolume() {
  if (millis() - lastVolumeCheck < 100) return;
  lastVolumeCheck = millis();

  int v = analogRead(MIC_PIN);

  // Update rolling 3-second average (30 samples)
  volSampleSum -= volSamples[volSampleIdx];
  volSamples[volSampleIdx] = v;
  volSampleSum += v;
  volSampleIdx = (volSampleIdx + 1) % 30;
  if (volSampleCount < 30) volSampleCount++;
  int rollingAvg = volSampleSum / volSampleCount;

  // Print values to Serial Monitor
  Serial.print("MIC: ");
  Serial.print(v);
  Serial.print(" | Avg(3s): ");
  Serial.println(rollingAvg);

  bool loud = (v > VOL_THRESHOLD);

  if (loud) {
    if (!volElevated) { volElevated = true; volHighSince = millis(); }
    if (currentMode != CONFLICT_MODE && millis() - volHighSince > VOL_CONFLICT_MS) {
      // Volume-based auto-conflict
      logConflictEvent("vol", 0);
      currentMode  = CONFLICT_MODE;
      stateChanged = true;
      Serial.println("CONFLICT: volume threshold exceeded");
    }
  } else {
    volElevated = false;
  }

  // Auto-reopen after timeout
  if (currentMode == CONFLICT_MODE &&
      millis() - conflictStartMs > CONFLICT_AUTO_REOPEN_MS) {
    closeLastEventDuration();
    currentMode  = BREATHE;
    stateChanged = true;
    Serial.println("CONFLICT: auto-reopen after timeout");
  }
}

// ── Poll server (safe to call anywhere) ──────────────────────
void handleClient(WiFiClient &client); // forward declaration

void pollServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client = server.available();
  if (client) handleClient(client);
}

// ── Movement ──────────────────────────────────────────────────
void ultraSmoothMoveMirrored(int startPos, int endPos) {
  float distance = abs(endPos - startPos);
  int direction  = (endPos > startPos) ? 1 : -1;
  int steps      = (int)(distance / 0.05f);
  if (steps < 1) steps = 1;

  int lastPos1 = startPos, lastPos2 = 90 - startPos;
  uint8_t lastR = 0, lastG = 0, lastB = 0;

  for (int i = 0; i <= steps; i++) {
    if (stateChanged) return;
    if (i % 10 == 0) pollServer();

    float p  = (float)i / steps;
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
  currentPos = endPos;
  updateLEDs(endPos == positionOpen ? 1.0f : 0.0f);
}

// ── HTTP helpers ──────────────────────────────────────────────
String parseParam(String req, String key) {
  int idx = req.indexOf(key + "=");
  if (idx < 0) return "";
  int start = idx + key.length() + 1;
  int end   = req.indexOf('&', start);
  if (end < 0) end = req.indexOf(' ', start);
  if (end < 0) end = req.length();
  return req.substring(start, end);
}

String urlDecode(String s) {
  s.replace("%3A", ":"); s.replace("%2F", "/");
  s.replace("%3F", "?"); s.replace("%3D", "=");
  s.replace("%26", "&"); s.replace("%2C", ",");
  s.replace("+",   " ");
  return s;
}

// ── HTTP server ───────────────────────────────────────────────
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

  bool isRoot     = req.indexOf("GET / ")       >= 0 || req.indexOf("GET /\r") >= 0;
  bool isStatus   = req.indexOf("GET /status")  >= 0;
  bool isControl  = req.indexOf("GET /control") >= 0;
  bool isSpeed    = req.indexOf("GET /speed")   >= 0;
  bool isConflict = req.indexOf("GET /conflict")>= 0;
  bool isEvents   = req.indexOf("GET /events")  >= 0;
  bool isOptions  = req.indexOf("OPTIONS ")     >= 0;

  // CORS preflight
  if (isOptions) {
    client.println("HTTP/1.1 204 No Content");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Access-Control-Allow-Methods: GET, OPTIONS");
    client.println("Connection: close");
    client.println();
    client.stop();
    return;
  }

  // Fast speed endpoint
  if (isSpeed) {
    String v = parseParam(req, "v");
    if (v.length()) speedLevel = constrain(v.toInt(), 1, 10);
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

  // Conflict trigger endpoint (called by LLM listen page or volume auto-detect)
  if (isConflict) {
    String src      = parseParam(req, "src");
    String scoreStr = parseParam(req, "score");
    String redirect = urlDecode(parseParam(req, "redirect"));

    if (src.length() == 0) src = "manual";
    int score = scoreStr.length() ? scoreStr.toInt() : 0;

    // Close previous event duration if still open
    closeLastEventDuration();
    // Log new event
    logConflictEvent(src.c_str(), score);

    // Trigger close
    currentMode  = CONFLICT_MODE;
    stateChanged = true;

    Serial.print("CONFLICT: src="); Serial.print(src);
    Serial.print(" score="); Serial.println(score);

    if (redirect.length() > 0) {
      client.println("HTTP/1.1 302 Found");
      client.println("Location: " + redirect);
      client.println("Connection: close");
      client.println();
    } else {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Access-Control-Allow-Origin: *");
      client.println("Connection: close");
      client.println("Content-Length: 10");
      client.println();
      client.print("{\"ok\":true}");
    }
    client.stop();
    return;
  }

  // Events log endpoint
  if (isEvents) {
    String body = "[";
    int n = evtTotal;
    for (int i = 0; i < n; i++) {
      // Oldest first: start from (evtHead - n + i + 30) % 30
      int idx = (evtHead - n + i + 30) % 30;
      if (i > 0) body += ",";
      body += "{\"t\":"    + String(evtLog[idx].startMs);
      body += ",\"src\":\"" + String(evtLog[idx].src) + "\"";
      body += ",\"score\":" + String(evtLog[idx].score);
      body += ",\"dur\":"   + String(evtLog[idx].durationMs) + "}";
    }
    body += "]";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println("Content-Length: " + String(body.length()));
    client.println();
    client.print(body);
    client.stop();
    return;
  }

  // Main page
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

  // Control endpoint
  if (isControl) {
    String mStr = parseParam(req, "mode");
    String sStr = parseParam(req, "speed");
    String pStr = parseParam(req, "pause");
    String bStr = parseParam(req, "bloom");

    if      (mStr == "breathe") { closeLastEventDuration(); currentMode = BREATHE; }
    else if (mStr == "pause")   currentMode = PAUSE_MODE;
    else if (mStr == "open")    currentMode = OPEN_MODE;
    else if (mStr == "manual")  currentMode = MANUAL;

    if (sStr.length()) speedLevel = constrain(sStr.toInt(), 1, 10);
    if (pStr.length()) pauseSec   = constrain(pStr.toInt(), 0, 10);
    if (bStr.length()) bloomPct   = constrain(bStr.toInt(), 0, 100);

    stateChanged = true;
  }

  if (!isStatus && !isControl) {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
    client.stop();
    return;
  }

  // Status JSON response
  String modeStr = "breathe";
  if      (currentMode == PAUSE_MODE)    modeStr = "pause";
  else if (currentMode == OPEN_MODE)     modeStr = "open";
  else if (currentMode == MANUAL)        modeStr = "manual";
  else if (currentMode == CONFLICT_MODE) modeStr = "conflict";

  String body = "{\"mode\":\"" + modeStr + "\","
                "\"speed\":"  + String(speedLevel) + ","
                "\"pause\":"  + String(pauseSec)   + ","
                "\"bloom\":"  + String(bloomPct)   + ","
                "\"conflicts\":" + String(evtTotal) + "}";

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
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
  pinMode(MIC_PIN, INPUT);

  Wire.begin();
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(50);
  delay(10);

  writeServo(SERVO_1_CH, positionZero);
  writeServo(SERVO_2_CH, 90 - positionZero);
  currentPos = positionZero;

  strip.begin();
  strip.setBrightness(180);
  strip.show();
  setAllLEDs(COLOR_CLOSED_R, COLOR_CLOSED_G, COLOR_CLOSED_B);

  connectWiFi();
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  // WiFi watchdog
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost - reconnecting...");
      connectWiFi();
    }
  }

  checkVolume();
  pollServer();

  if (stateChanged) {
    stateChanged = false;

    if (currentMode == OPEN_MODE) {
      ultraSmoothMoveMirrored(currentPos, positionOpen);

    } else if (currentMode == CONFLICT_MODE) {
      // Fast close - speed 10 for conflict impact
      int saved = speedLevel;
      speedLevel = 10;
      ultraSmoothMoveMirrored(currentPos, positionZero);
      speedLevel = saved;
      fadeTo(COLOR_CONFLICT_R, COLOR_CONFLICT_G, COLOR_CONFLICT_B, 400);

    } else if (currentMode == BREATHE) {
      cycleCount = 0;
    }
    return;
  }

  // ── Mode behaviors ─────────────────────────────────────────
  if (currentMode == BREATHE) {
    cycleCount++;
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
      fadeTo(COLOR_CLOSED_R/4, COLOR_CLOSED_G/4, COLOR_CLOSED_B/4, 60);
      unsigned long t0 = millis();
      while (millis() - t0 < 3800) {
        pollServer(); checkVolume();
        if (stateChanged) return;
        delay(50);
      }
      fadeTo(COLOR_CLOSED_R, COLOR_CLOSED_G, COLOR_CLOSED_B, 60);
      cycleCount = 0;
    } else {
      unsigned long t0 = millis();
      while (millis() - t0 < (unsigned long)(pauseSec * 1000)) {
        pollServer(); checkVolume();
        if (stateChanged) return;
        delay(50);
      }
    }

  } else if (currentMode == PAUSE_MODE || currentMode == OPEN_MODE) {
    checkVolume(); pollServer(); delay(100);

  } else if (currentMode == CONFLICT_MODE) {
    // Stay closed - LEDs pulse dark red
    checkVolume(); pollServer(); delay(100);

  } else if (currentMode == MANUAL) {
    int targetAngle = map(bloomPct, 0, 100, positionZero, positionOpen);
    writeServo(SERVO_1_CH, targetAngle);
    writeServo(SERVO_2_CH, 90 - targetAngle);
    currentPos = targetAngle;
    updateLEDs((float)bloomPct / 100.0f);
    checkVolume(); pollServer(); delay(100);
  }
}
