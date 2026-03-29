// ============================================================
//  ESP32 PWM Controller  —  3 Independent Channels
//  GPIO 25 (CH0) | GPIO 26 (CH1) | GPIO 27 (CH2)
//
//  Each channel has its OWN LEDC timer → fully independent freq
//  Frequency  : ~1 Hz  →  40 000 000 Hz  (hardware limits)
//  Duty Cycle : 0 – 100 %  (per channel)
//  Resolution : auto-calculated per channel for best accuracy
//
//  WiFi AP  SSID : ESP32_PWM   Pass : 12345qwert
//  Web UI   http://192.168.4.1
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include "driver/ledc.h"   // ESP-IDF LEDC — full timer control

// ---- WiFi ----
const char* AP_SSID = "ESP32_PWM";
const char* AP_PASS = "12345qwert";

// ---- Pin assignments (change as needed, any output-capable GPIO) ----
const uint8_t PWM_PINS[3] = {25, 26, 27};

// ---- Each channel gets its own dedicated LEDC timer ----
//  Channels 0/1 share timer, 2/3 share timer, 4/5 share timer.
//  Using channels 0, 2, 4 guarantees each sits on its own timer.
const ledc_timer_t   LEDC_TIMERS[3]   = {LEDC_TIMER_0,   LEDC_TIMER_1,   LEDC_TIMER_2};
const ledc_channel_t LEDC_CHANNELS[3] = {LEDC_CHANNEL_0, LEDC_CHANNEL_2, LEDC_CHANNEL_4};

// ---- Runtime state ----
uint32_t pwmFreq[3] = {1000, 1000, 1000};  // Hz
uint8_t  pwmDuty[3] = {50,   50,   50};    // percent
bool     pwmEn[3]   = {true, true, true};

WebServer server(80);

// ============================================================
//  Auto-pick best resolution for a given frequency.
//  bits = floor(log2(80_000_000 / freq)), clamped 1–20
//  Lower freq → more bits → smoother duty steps
//  Higher freq → fewer bits (hardware constraint)
// ============================================================
uint8_t bestResolution(uint32_t freq) {
  if (freq == 0) return 10;
  uint8_t bits = 0;
  uint32_t val = 80000000UL / freq;
  while (val > 1) { val >>= 1; bits++; }
  if (bits < 1)  bits = 1;
  if (bits > 20) bits = 20;
  return bits;
}

// ============================================================
//  Apply a single channel via ESP-IDF LEDC driver directly.
//  Bypasses Arduino's channel-sharing assumption entirely.
// ============================================================
void applyChannel(int ch) {
  uint8_t  res     = bestResolution(pwmFreq[ch]);
  uint32_t maxDuty = (1UL << res) - 1;
  uint32_t counts  = pwmEn[ch]
                     ? (uint32_t)((uint64_t)pwmDuty[ch] * maxDuty / 100UL)
                     : 0;

  // 1. Configure the timer for this channel
  ledc_timer_config_t tcfg;
  memset(&tcfg, 0, sizeof(tcfg));
  tcfg.speed_mode      = LEDC_LOW_SPEED_MODE;
  tcfg.duty_resolution = (ledc_timer_bit_t)res;
  tcfg.timer_num       = LEDC_TIMERS[ch];
  tcfg.freq_hz         = pwmFreq[ch];
  tcfg.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&tcfg);

  // 2. Configure the output channel
  ledc_channel_config_t ccfg;
  memset(&ccfg, 0, sizeof(ccfg));
  ccfg.gpio_num   = PWM_PINS[ch];
  ccfg.speed_mode = LEDC_LOW_SPEED_MODE;
  ccfg.channel    = LEDC_CHANNELS[ch];
  ccfg.intr_type  = LEDC_INTR_DISABLE;
  ccfg.timer_sel  = LEDC_TIMERS[ch];
  ccfg.duty       = counts;
  ccfg.hpoint     = 0;
  ledc_channel_config(&ccfg);

  // 3. Update duty
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNELS[ch], counts);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNELS[ch]);
}

void applyAllChannels() {
  for (int i = 0; i < 3; i++) applyChannel(i);
}

// ============================================================
//  HTTP: GET /set?f0=1000&d0=50&e0=1&f1=440&d1=75&e1=1&...
// ============================================================
void handleSet() {
  const char* fKeys[3] = {"f0","f1","f2"};
  const char* dKeys[3] = {"d0","d1","d2"};
  const char* eKeys[3] = {"e0","e1","e2"};

  for (int i = 0; i < 3; i++) {
    if (server.hasArg(fKeys[i])) {
      uint32_t f = (uint32_t)server.arg(fKeys[i]).toInt();
      if (f >= 1 && f <= 40000000UL) pwmFreq[i] = f;
    }
    if (server.hasArg(dKeys[i])) {
      int d = server.arg(dKeys[i]).toInt();
      if (d >= 0 && d <= 100) pwmDuty[i] = (uint8_t)d;
    }
    if (server.hasArg(eKeys[i])) {
      pwmEn[i] = server.arg(eKeys[i]).toInt() != 0;
    }
    applyChannel(i);
  }

  // Return current state as JSON
  String json = "{";
  for (int i = 0; i < 3; i++) {
    if (i) json += ",";
    json += "\"f" + String(i) + "\":" + String(pwmFreq[i]);
    json += ",\"d" + String(i) + "\":" + String(pwmDuty[i]);
    json += ",\"e" + String(i) + "\":" + String(pwmEn[i] ? 1 : 0);
    json += ",\"r" + String(i) + "\":" + String(bestResolution(pwmFreq[i]));
  }
  json += "}";
  server.send(200, "application/json", json);
}

// ============================================================
//  HTML Web UI
// ============================================================
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 PWM</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@500;900&display=swap');
:root{
  --bg:#080c14;--surface:#0f1724;--border:#1a3050;
  --c0:#00d4ff;--c1:#00ff9f;--c2:#ff6b35;
  --text:#b0c8e0;--muted:#3a5570;--r:10px;
}
*{box-sizing:border-box;margin:0;padding:0}
body{
  background:var(--bg);color:var(--text);
  font-family:'Share Tech Mono',monospace;
  min-height:100vh;padding:20px 14px 60px;
  display:flex;flex-direction:column;align-items:center;
  background-image:
    radial-gradient(ellipse 100% 50% at 50% -5%,rgba(0,180,255,.1) 0%,transparent 70%),
    repeating-linear-gradient(0deg,transparent,transparent 39px,rgba(0,180,255,.03) 40px),
    repeating-linear-gradient(90deg,transparent,transparent 39px,rgba(0,180,255,.03) 40px);
}
h1{
  font-family:'Orbitron',sans-serif;font-weight:900;
  font-size:clamp(1.3rem,4vw,2rem);letter-spacing:.15em;
  color:var(--c0);text-shadow:0 0 30px rgba(0,212,255,.4);
  margin-bottom:4px;text-align:center;
}
.sub{font-size:.72rem;color:var(--muted);letter-spacing:.1em;text-align:center;margin-bottom:6px}
.badge{
  display:inline-flex;align-items:center;gap:6px;
  border:1px solid var(--c1);background:rgba(0,255,159,.06);
  color:var(--c1);border-radius:20px;padding:3px 14px;
  font-size:.68rem;letter-spacing:.1em;margin-bottom:28px;
}
.dot{width:7px;height:7px;border-radius:50%;background:var(--c1);
     box-shadow:0 0 8px var(--c1);animation:pulse 1.5s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
.channels{
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(290px,1fr));
  gap:18px;width:100%;max-width:980px;
}
.card{
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--r);padding:22px 20px 20px;
  position:relative;overflow:hidden;
  transition:border-color .25s,box-shadow .25s;
}
.card[data-ch="0"]{--cc:var(--c0)}
.card[data-ch="1"]{--cc:var(--c1)}
.card[data-ch="2"]{--cc:var(--c2)}
.card::before{
  content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,transparent,var(--cc),transparent);
}
.card:hover{border-color:var(--cc);box-shadow:0 0 24px rgba(0,0,0,.4)}
.ch-head{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:18px}
.ch-title{font-family:'Orbitron',sans-serif;font-size:.9rem;font-weight:900;letter-spacing:.12em;color:var(--cc)}
.ch-meta{font-size:.63rem;color:var(--muted);margin-top:3px;letter-spacing:.07em}
.tog{position:relative;width:42px;height:22px;flex-shrink:0;margin-top:2px}
.tog input{opacity:0;width:0;height:0}
.tog-bar{position:absolute;inset:0;border-radius:22px;background:var(--border);cursor:pointer;transition:.2s}
.tog-bar::before{
  content:'';position:absolute;width:16px;height:16px;
  left:3px;top:3px;background:var(--muted);border-radius:50%;transition:.2s;
}
.tog input:checked+.tog-bar{background:rgba(0,255,159,.2);border:1px solid var(--c1)}
.tog input:checked+.tog-bar::before{transform:translateX(20px);background:var(--c1)}
.lbl{
  display:flex;justify-content:space-between;align-items:baseline;
  font-size:.68rem;color:var(--muted);letter-spacing:.07em;
  margin-bottom:7px;margin-top:16px;
}
.lbl span{font-size:.9rem;font-family:'Orbitron',sans-serif;color:var(--cc)}
input[type=range]{
  -webkit-appearance:none;width:100%;height:5px;border-radius:3px;
  outline:none;cursor:pointer;
  background:linear-gradient(90deg,var(--cc) var(--pct,50%),var(--border) var(--pct,50%));
}
input[type=range]::-webkit-slider-thumb{
  -webkit-appearance:none;width:17px;height:17px;border-radius:50%;
  background:var(--cc);box-shadow:0 0 10px var(--cc);border:2px solid var(--bg);
}
.freq-row{display:flex;gap:8px;align-items:center;margin-top:10px}
.freq-row input[type=number]{
  flex:1;background:rgba(0,0,0,.4);border:1px solid var(--border);
  border-radius:6px;color:var(--cc);font-family:'Share Tech Mono',monospace;
  font-size:.95rem;padding:7px 10px;outline:none;transition:border-color .2s;
  -moz-appearance:textfield;
}
.freq-row input[type=number]::-webkit-inner-spin-button{opacity:.3}
.freq-row input[type=number]:focus{border-color:var(--cc)}
.freq-unit{font-size:.68rem;color:var(--muted);white-space:nowrap}
.res-info{font-size:.62rem;color:var(--muted);letter-spacing:.05em;margin-top:4px;text-align:right}
.wave{width:100%;height:48px;display:block;margin-top:14px;border-radius:6px}
.apply-btn{
  width:100%;max-width:980px;padding:15px;margin-top:22px;
  font-family:'Orbitron',sans-serif;font-size:.95rem;font-weight:900;
  letter-spacing:.2em;color:var(--bg);
  background:linear-gradient(135deg,#00d4ff,#0088aa);
  border:none;border-radius:var(--r);cursor:pointer;
  box-shadow:0 0 28px rgba(0,212,255,.25);transition:opacity .2s,box-shadow .2s;
}
.apply-btn:hover{opacity:.9;box-shadow:0 0 40px rgba(0,212,255,.4)}
.apply-btn:active{opacity:.7}
#status{
  width:100%;max-width:980px;text-align:center;
  font-size:.72rem;letter-spacing:.08em;min-height:1.1em;
  margin-top:10px;color:var(--c1);opacity:0;transition:opacity .3s;
}
#status.show{opacity:1}
footer{margin-top:40px;font-size:.62rem;color:var(--muted);letter-spacing:.1em;text-align:center}
</style>
</head>
<body>
<h1>&#9650; PWM CONTROLLER</h1>
<p class="sub">ESP32 &mdash; 3 Fully Independent Channels</p>
<span class="badge"><span class="dot"></span>AP MODE &nbsp;&mdash;&nbsp; 192.168.4.1</span>

<div class="channels" id="channels"></div>

<button class="apply-btn" onclick="applyAll()">&#9654;&#9654; APPLY ALL CHANNELS</button>
<div id="status"></div>

<footer>GPIO 25 / 26 / 27 &nbsp;&bull;&nbsp; LEDC TIMER 0 / 1 / 2 &nbsp;&bull;&nbsp; AUTO RESOLUTION</footer>

<script>
const COLORS = ['#00d4ff','#00ff9f','#ff6b35'];
const GPIOS  = [25, 26, 27];
const TIMERS = [0, 1, 2];

let freqs  = [1000, 1000, 1000];
let duties = [50, 50, 50];
let enables= [true, true, true];

function bestRes(f){
  if(!f) return 10;
  let bits=0, val=Math.floor(80000000/f);
  while(val>1){val>>=1;bits++;}
  return Math.max(1,Math.min(20,bits));
}

function formatFreq(f){
  if(f>=1000000) return (f/1000000).toPrecision(4).replace(/\.?0+$/,'')+' MHz';
  if(f>=1000)    return (f/1000).toPrecision(4).replace(/\.?0+$/,'')+' kHz';
  return f+' Hz';
}

// Log scale: slider 0..1000 → freq 1..40000000
function sliderToFreq(v){ return Math.max(1,Math.round(Math.pow(10, v/1000*Math.log10(40000000)))); }
function freqToSlider(f){ return Math.round(Math.log10(Math.max(1,f))/Math.log10(40000000)*1000); }
function freqPct(f){      return (freqToSlider(f)/10).toFixed(2); }

function buildUI(){
  let html='';
  for(let i=0;i<3;i++){
    html+=`
    <div class="card" data-ch="${i}">
      <div class="ch-head">
        <div>
          <div class="ch-title">CHANNEL ${i}</div>
          <div class="ch-meta">GPIO ${GPIOS[i]} &nbsp;&bull;&nbsp; LEDC TIMER ${TIMERS[i]}</div>
        </div>
        <label class="tog">
          <input type="checkbox" id="en${i}" ${enables[i]?'checked':''} onchange="setEnable(${i},this.checked)">
          <span class="tog-bar"></span>
        </label>
      </div>

      <div class="lbl">FREQUENCY <span id="fv${i}">${formatFreq(freqs[i])}</span></div>
      <input type="range" id="fsl${i}" min="0" max="1000" value="${freqToSlider(freqs[i])}"
             oninput="onFreqSlider(${i},this.value)"
             style="--pct:${freqPct(freqs[i])}%">
      <div class="freq-row">
        <input type="number" id="fnum${i}" min="1" max="40000000" value="${freqs[i]}"
               oninput="onFreqNum(${i},this.value)">
        <span class="freq-unit">Hz &nbsp;(1 Hz &ndash; 40 MHz)</span>
      </div>
      <div class="res-info" id="rinfo${i}"></div>

      <div class="lbl" style="margin-top:18px">DUTY CYCLE <span id="dv${i}">${duties[i]} %</span></div>
      <input type="range" id="dsl${i}" min="0" max="100" value="${duties[i]}"
             oninput="onDuty(${i},this.value)"
             style="--pct:${duties[i]}%">

      <canvas class="wave" id="wave${i}" height="48"></canvas>
    </div>`;
  }
  document.getElementById('channels').innerHTML=html;
  for(let i=0;i<3;i++){updateResInfo(i);drawWave(i);}
}

function updateResInfo(i){
  const r=bestRes(freqs[i]);
  const steps=(1<<r)-1;
  const period=(1/freqs[i]*1000);
  const pStr = period>=1 ? period.toPrecision(3)+' ms' : (period*1000).toPrecision(3)+' µs';
  document.getElementById('rinfo'+i).textContent=
    `auto: ${r}-bit resolution · ${steps} duty steps · T = ${pStr}`;
}

function onFreqSlider(i,v){
  freqs[i]=sliderToFreq(parseInt(v));
  document.getElementById('fnum'+i).value=freqs[i];
  document.getElementById('fv'+i).textContent=formatFreq(freqs[i]);
  document.getElementById('fsl'+i).style.setProperty('--pct',freqPct(freqs[i])+'%');
  updateResInfo(i); drawWave(i);
}

function onFreqNum(i,v){
  let f=parseInt(v)||1;
  f=Math.max(1,Math.min(40000000,f));
  freqs[i]=f;
  document.getElementById('fv'+i).textContent=formatFreq(f);
  const sl=document.getElementById('fsl'+i);
  sl.value=freqToSlider(f);
  sl.style.setProperty('--pct',freqPct(f)+'%');
  updateResInfo(i); drawWave(i);
}

function onDuty(i,v){
  duties[i]=parseInt(v);
  document.getElementById('dv'+i).textContent=duties[i]+' %';
  document.getElementById('dsl'+i).style.setProperty('--pct',duties[i]+'%');
  drawWave(i);
}

function setEnable(i,v){
  enables[i]=v;
  drawWave(i);
}

function drawWave(i){
  const cv=document.getElementById('wave'+i);
  if(!cv) return;
  const dpr=window.devicePixelRatio||1;
  cv.width=cv.offsetWidth*dpr||300;
  cv.height=48*dpr;
  const ctx=cv.getContext('2d');
  const W=cv.width, H=cv.height;
  const col=COLORS[i];
  ctx.clearRect(0,0,W,H);
  ctx.fillStyle='rgba(0,0,0,.35)'; ctx.fillRect(0,0,W,H);
  if(!enables[i]){
    ctx.strokeStyle=col+'55';ctx.lineWidth=1.5*dpr;
    ctx.setLineDash([6*dpr,4*dpr]);
    ctx.beginPath();ctx.moveTo(0,H*.75);ctx.lineTo(W,H*.75);ctx.stroke();
    ctx.setLineDash([]);
    return;
  }
  const cycles=4, hi=H*.1, lo=H*.85;
  ctx.strokeStyle=col; ctx.lineWidth=2*dpr;
  ctx.shadowColor=col; ctx.shadowBlur=7;
  ctx.beginPath();
  for(let c=0;c<cycles;c++){
    const x0=c/cycles*W;
    const xm=x0+duties[i]/100*(W/cycles);
    const x1=(c+1)/cycles*W;
    if(c===0) ctx.moveTo(x0,lo);
    ctx.lineTo(x0,lo); ctx.lineTo(x0,hi);
    ctx.lineTo(xm,hi); ctx.lineTo(xm,lo);
    ctx.lineTo(x1,lo);
  }
  ctx.stroke(); ctx.shadowBlur=0;
}

function applyAll(){
  const p=new URLSearchParams();
  for(let i=0;i<3;i++){
    p.set('f'+i,freqs[i]);
    p.set('d'+i,duties[i]);
    p.set('e'+i,enables[i]?1:0);
  }
  fetch('/set?'+p.toString())
    .then(r=>r.json())
    .then(data=>{
      const msg=[0,1,2].map(i=>`CH${i}: ${formatFreq(data['f'+i])} @ ${data['d'+i]}% [${data['r'+i]}bit]`).join(' &nbsp;|&nbsp; ');
      showStatus('&#10003; '+msg);
    })
    .catch(()=>showStatus('&#10007; Connection error','#ff6b35'));
}

function showStatus(msg,col){
  const el=document.getElementById('status');
  el.innerHTML=msg; el.style.color=col||'var(--c1)';
  el.classList.add('show');
  setTimeout(()=>el.classList.remove('show'),4000);
}

buildUI();
window.addEventListener('resize',()=>{for(let i=0;i<3;i++)drawWave(i);});
</script>
</body>
</html>
)rawhtml";

void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 PWM Controller — 3 Independent Channels ===");

  applyAllChannels();

  for (int i = 0; i < 3; i++) {
    Serial.printf("CH%d → GPIO%02d  TIMER%d  %u Hz  %u%%  %u-bit\n",
                  i, PWM_PINS[i], i, pwmFreq[i], pwmDuty[i],
                  bestResolution(pwmFreq[i]));
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP ready — SSID: %s  IP: %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.println("Open: http://192.168.4.1");

  server.on("/",    HTTP_GET, handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
}
