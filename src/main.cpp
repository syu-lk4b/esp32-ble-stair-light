/*
 * ESP32 BLE Stair Light Panel
 * ---------------------------
 * A touch panel that takes over cheap BLE "stair step light" controllers
 * (the kind sold with a phone app and an RF remote) and puts them on
 * Home Assistant / HomeKit.
 *
 * The panel acts as a BLE *central*: it scans for the two advertised names
 * ("upstair" / "downstair"), connects, and speaks the vendor's serial frame
 * format.  Home Assistant is kept in sync over its REST API, and HomeKit is
 * exposed by HA's HomeKit Bridge.
 *
 * Board: ES3C28P (ESP32-S3, 2.8" 320x240 touch panel)
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <time.h>
#include <NimBLEDevice.h>
#include "lgfx_es3c28p.h"
#include "secrets.h"

#define FW_VERSION "1.0.0"

// ---------- Vendor BLE protocol ----------
// Frame: AA AF CMD LEN <payload> CHK      CHK = sum(all previous bytes) & 0xFF
// Reply: 55 50 ...
#define CMD_MODE      0xB5   // payload: 1 byte, 0..5
#define CMD_FLOW_BRI  0xB8   // payload: 1 byte, 0..100
#define CMD_STEP_BRI  0xB9   // payload: 1 byte, 0..100
#define CMD_TIMER     0xBE   // payload: 4 bytes, on HH MM off HH MM
#define CMD_TIME      0xBA   // payload: 3 bytes, HH MM SS (also queries config)

static const char* MODE_OPTIONS[6] = {
  "0-全亮模式", "1-交替模式", "2-流水模式", "3-逐步模式", "4-跟随模式", "5-随机模式"
};

// ---------- BLE state ----------
struct StairDev {
  const char* name;
  const char* label;
  NimBLEClient* client = nullptr;
  NimBLERemoteCharacteristic* chr = nullptr;
  volatile bool connected = false;
  volatile uint8_t mode = 3;
  volatile uint8_t briStep = 79;
  volatile uint8_t briFlow = 63;
  volatile bool pushToHa = false;
};

static StairDev devs[2];
static int activeIdx = 0;
static volatile bool uiDirty = false;
static volatile bool syncBanner = false;
static volatile bool forceReconnect = false;
static volatile bool haOk = false;
static volatile bool otaRejected = false;
static volatile uint32_t lastLocalTouchMs = 0;
static SemaphoreHandle_t bleMutex = nullptr;

static void initDevs() {
  devs[0].name = BLE_NAME_DOWN;
  devs[0].label = "DN";
  devs[0].briStep = 79;
  devs[0].briFlow = 63;
  devs[1].name = BLE_NAME_UP;
  devs[1].label = "UP";
  devs[1].briStep = 79;
  devs[1].briFlow = 96;
}

static void sendPktLocked(StairDev &d, uint8_t cmd, const uint8_t* payload, uint8_t len) {
  if (!d.connected || !d.chr) return;
  uint8_t buf[32];
  buf[0] = 0xAA; buf[1] = 0xAF; buf[2] = cmd; buf[3] = len;
  memcpy(buf + 4, payload, len);
  uint8_t chk = 0;
  for (int i = 0; i < 4 + len; i++) chk += buf[i];
  buf[4 + len] = chk;
  d.chr->writeValue(std::string((char*)buf, 5 + len), false);
}

static void sendPkt(StairDev &d, uint8_t cmd, const uint8_t* payload, uint8_t len) {
  if (!bleMutex) return;
  if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    sendPktLocked(d, cmd, payload, len);
    xSemaphoreGive(bleMutex);
  }
}

static void sendTimeAndQueryLocked(StairDev &d) {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  uint8_t q[3] = {
    (uint8_t)(tm.tm_year > 120 ? tm.tm_hour : 20),
    (uint8_t)(tm.tm_year > 120 ? tm.tm_min  : 0),
    (uint8_t)(tm.tm_year > 120 ? tm.tm_sec  : 0)
  };
  sendPktLocked(d, CMD_TIME, q, 3);
}

static void syncTimerToDevice(int idx) {
  if (!bleMutex) return;
  if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    uint8_t t[4] = {TIMER_ON_HOUR, TIMER_ON_MIN, TIMER_OFF_HOUR, TIMER_OFF_MIN};
    sendPktLocked(devs[idx], CMD_TIMER, t, 4);
    delay(40);
    sendTimeAndQueryLocked(devs[idx]);
    xSemaphoreGive(bleMutex);
  }
}

// Reply: 55 50 BA 10 <16 bytes>; observed layout: [11]=flow, [12]=step, [15]=mode
static void parseConfigReply(int idx, uint8_t* data, size_t len) {
  if (len < 16 || data[0] != 0x55 || data[1] != 0x50 || data[2] != 0xBA || data[3] != 0x10) return;
  if (data[11] <= 100) devs[idx].briFlow = data[11];
  if (data[12] <= 100) devs[idx].briStep = data[12];
  if (data[15] <= 5)   devs[idx].mode    = data[15];
  devs[idx].pushToHa = true;
  uiDirty = true;
}

static void onNotify0(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { parseConfigReply(0, data, len); }
static void onNotify1(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { parseConfigReply(1, data, len); }

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {}
  void onDisconnect(NimBLEClient* c) override {
    for (auto &d : devs) {
      if (d.client == c) {
        d.connected = false;
        d.chr = nullptr;
        d.client = nullptr;
        uiDirty = true;
      }
    }
  }
};

static NimBLEScan* scanner = nullptr;

static void scanAndConnect() {
  if (!bleMutex) return;
  if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  bool need[2] = {!devs[0].connected, !devs[1].connected};
  if (!need[0] && !need[1]) { xSemaphoreGive(bleMutex); return; }

  if (!scanner) { xSemaphoreGive(bleMutex); return; }
  if (scanner->isScanning()) scanner->stop();
  scanner->clearResults();
  NimBLEScanResults results = scanner->start(3, false);
  int cnt = results.getCount();
  for (int i = 0; i < cnt; i++) {
    NimBLEAdvertisedDevice dev = results.getDevice(i);
    std::string name = dev.getName();
    for (int di = 0; di < 2; di++) {
      if (need[di] && name == devs[di].name && !devs[di].connected) {
        NimBLEClient* c = NimBLEDevice::createClient();
        c->setClientCallbacks(new ClientCB(), false);
        c->setConnectionParams(12, 12, 0, 60);
        if (c->connect(&dev)) {
          auto svc = c->getService(NimBLEUUID((uint16_t)0xFFE0));
          if (svc) {
            auto chr = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFE1));
            if (chr) {
              if (chr->canNotify()) chr->subscribe(true, di == 0 ? onNotify0 : onNotify1);
              devs[di].client = c;
              devs[di].chr = chr;
              devs[di].connected = true;
              devs[di].pushToHa = true;
              uiDirty = true;
              sendTimeAndQueryLocked(devs[di]);
            } else {
              c->disconnect();
              NimBLEDevice::deleteClient(c);
            }
          } else {
            c->disconnect();
            NimBLEDevice::deleteClient(c);
          }
        } else {
          NimBLEDevice::deleteClient(c);
        }
      }
    }
  }
  scanner->clearResults();
  xSemaphoreGive(bleMutex);
}

// ---------- Web server ----------
static WebServer server(80);

static bool webAuthOk() {
  if (strlen(WEB_PASS) == 0) return true;
  return server.authenticate(WEB_USER, WEB_PASS);
}

static bool requireAuth() {
  if (webAuthOk()) return true;
  server.requestAuthentication();
  return false;
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BLE Stair Light Panel</title>
<style>
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:16px;max-width:520px}
  h2{margin:0 0 6px;font-size:18px;color:#38bdf8}
  .sub{font-size:12px;color:#94a3b8;margin-bottom:14px}
  .card{background:#1e293b;border-radius:12px;padding:16px;margin-bottom:14px}
  .tabs{display:flex;gap:8px;margin-bottom:12px}
  .tab{flex:1;text-align:center;padding:9px;border-radius:8px;background:#334155;cursor:pointer;font-size:14px}
  .tab.on{background:#0284c7}
  .modes{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
  .mode{padding:10px 4px;text-align:center;border-radius:8px;background:#334155;cursor:pointer;font-size:12px}
  .mode.on{background:#10b981}
  .row{margin:14px 0 4px;font-size:13px;color:#cbd5e1}
  input[type=range]{width:100%}
  .btn{background:#0284c7;color:#fff;border:none;border-radius:8px;padding:10px 14px;font-size:14px;cursor:pointer;margin-right:8px}
  .btn.gray{background:#475569}
  .stat{font-size:12px;color:#94a3b8;line-height:1.7;margin-top:10px}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
</style>
</head>
<body>
<h2>🪜 BLE Stair Light Panel</h2>
<div class="sub" id="ip">connecting…</div>

<div class="card">
  <div class="tabs">
    <div class="tab" id="tab0" onclick="pick(0)">Downstairs / 楼下</div>
    <div class="tab" id="tab1" onclick="pick(1)">Upstairs / 楼上</div>
  </div>
  <div class="modes" id="modes"></div>

  <div class="row">Step brightness / 台阶亮度: <b id="stepVal">–</b></div>
  <input type="range" id="step" min="0" max="100" oninput="bri('step',this.value)" onchange="bri('step',this.value)">
  <div class="row">Flow brightness / 流水亮度: <b id="flowVal">–</b></div>
  <input type="range" id="flow" min="0" max="100" oninput="bri('flow',this.value)" onchange="bri('flow',this.value)">

  <div style="margin-top:16px">
    <button class="btn" onclick="act('sync_time=1')">Sync time / 校时</button>
    <button class="btn gray" onclick="act('reconnect=1')">Reconnect / 重连</button>
  </div>
  <div class="stat" id="stat"></div>
</div>

<script>
let active = 0, data = null;
const MODES = ["All on / 全亮","Alternate / 交替","Flow / 流水","Step / 逐步","Follow / 跟随","Random / 随机"];

function pick(i){ active = i; render(); }
function render(){
  if(!data) return;
  document.getElementById('ip').innerText = 'Device ' + data.ip + ' · HA ' + (data.ha ? 'connected' : 'offline');
  document.getElementById('tab0').className = 'tab' + (active===0?' on':'');
  document.getElementById('tab1').className = 'tab' + (active===1?' on':'');
  const d = active ? data.upstair : data.downstair;
  const box = document.getElementById('modes');
  box.innerHTML = '';
  MODES.forEach((m,i)=>{
    const el = document.createElement('div');
    el.className = 'mode' + (d.mode===i?' on':'');
    el.innerText = m;
    el.onclick = ()=> set('mode='+i);
    box.appendChild(el);
  });
  document.getElementById('step').value = d.bri_step;
  document.getElementById('flow').value = d.bri_flow;
  document.getElementById('stepVal').innerText = d.bri_step + '%';
  document.getElementById('flowVal').innerText = d.bri_flow + '%';
  document.getElementById('stat').innerHTML =
    '<span class="dot" style="background:'+(d.connected?'#10b981':'#ef4444')+'"></span>'+
    (active?'Upstairs':'Downstairs')+' BLE '+(d.connected?'connected':'disconnected');
}
async function load(){
  try { data = await (await fetch('/status')).json(); render(); } catch(e) {}
}
async function set(q){
  await fetch('/set?dev='+active+'&'+q, {method:'POST'});
  load();
}
function bri(which, v){
  document.getElementById(which+'Val').innerText = v + '%';
  document.getElementById(which).value = v;
}
function act(q){ set(q); }
setInterval(load, 2000);
load();
</script>
</body>
</html>
)rawliteral";

static void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  if (!requireAuth()) return;
  char json[512];
  snprintf(json, sizeof(json),
    "{\"fw\":\"%s\",\"ip\":\"%s\",\"ha\":%s,"
    "\"downstair\":{\"connected\":%s,\"mode\":%d,\"bri_step\":%d,\"bri_flow\":%d},"
    "\"upstair\":{\"connected\":%s,\"mode\":%d,\"bri_step\":%d,\"bri_flow\":%d}}",
    FW_VERSION, WiFi.localIP().toString().c_str(), haOk ? "true" : "false",
    devs[0].connected ? "true" : "false", devs[0].mode, devs[0].briStep, devs[0].briFlow,
    devs[1].connected ? "true" : "false", devs[1].mode, devs[1].briStep, devs[1].briFlow);
  server.send(200, "application/json", json);
}

static void handleSet() {
  if (!requireAuth()) return;
  int di = 0;
  if (server.hasArg("dev")) {
    String d = server.arg("dev");
    if (d == "1" || d == "upstair" || d == "up") di = 1;
  }
  if (server.hasArg("mode")) {
    uint8_t v = constrain(server.arg("mode").toInt(), 0, 5);
    devs[di].mode = v;
    sendPkt(devs[di], CMD_MODE, &v, 1);
    devs[di].pushToHa = true;
    uiDirty = true;
  }
  if (server.hasArg("bri_step")) {
    uint8_t v = constrain(server.arg("bri_step").toInt(), 0, 100);
    devs[di].briStep = v;
    sendPkt(devs[di], CMD_STEP_BRI, &v, 1);
    devs[di].pushToHa = true;
    uiDirty = true;
  }
  if (server.hasArg("bri_flow")) {
    uint8_t v = constrain(server.arg("bri_flow").toInt(), 0, 100);
    devs[di].briFlow = v;
    sendPkt(devs[di], CMD_FLOW_BRI, &v, 1);
    devs[di].pushToHa = true;
    uiDirty = true;
  }
  if (server.hasArg("sync_time")) { syncTimerToDevice(di); syncBanner = true; uiDirty = true; }
  if (server.hasArg("reconnect")) forceReconnect = true;
  handleStatus();
}

// ---------- Home Assistant sync (Core 0) ----------
static int haPost(const char* path, const String &body, String* outResp = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.begin(String(HA_BASE_URL) + path);
  http.setTimeout(2500);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code > 0 && outResp) *outResp = http.getString();
  http.end();
  haOk = (code >= 200 && code < 300);
  return code;
}

static void pushDeviceToHa(int di, int &lastMode, int &lastStep, int &lastFlow) {
  const char* prefix = (di == 0) ? "downstair" : "upstair";
  if (devs[di].mode <= 5 && devs[di].mode != lastMode) {
    String b = String("{\"entity_id\":\"input_select.") + prefix + "_stair_mode\",\"option\":\"" +
               MODE_OPTIONS[devs[di].mode] + "\"}";
    if (haPost("/api/services/input_select/select_option", b) == 200) lastMode = devs[di].mode;
  }
  if (devs[di].briStep != lastStep) {
    String b = String("{\"entity_id\":\"input_number.") + prefix + "_step_brightness\",\"value\":" +
               String(devs[di].briStep) + "}";
    if (haPost("/api/services/input_number/set_value", b) == 200) lastStep = devs[di].briStep;
  }
  if (devs[di].briFlow != lastFlow) {
    String b = String("{\"entity_id\":\"input_number.") + prefix + "_flow_brightness\",\"value\":" +
               String(devs[di].briFlow) + "}";
    if (haPost("/api/services/input_number/set_value", b) == 200) lastFlow = devs[di].briFlow;
  }
}

static void pushSensorsToHa(bool force) {
  static int lastConn[2] = {-1, -1};
  for (int di = 0; di < 2; di++) {
    int c = devs[di].connected ? 1 : 0;
    if (force || c != lastConn[di]) {
      const char* prefix = (di == 0) ? "downstair" : "upstair";
      const char* cn = (di == 0) ? "楼下楼梯灯蓝牙连接" : "楼上楼梯灯蓝牙连接";
      String body = String("{\"state\":\"") + (c ? "on" : "off") +
                    "\",\"attributes\":{\"friendly_name\":\"" + cn +
                    "\",\"device_class\":\"connectivity\",\"ip\":\"" +
                    WiFi.localIP().toString() + "\"}}";
      if (haPost((String("/api/states/binary_sensor.") + prefix + "_stair_ble").c_str(), body) >= 200) {
        lastConn[di] = c;
      }
    }
  }
}

static void haTask(void*) {
  int haMode[2] = {-1, -1}, haStep[2] = {-1, -1}, haFlow[2] = {-1, -1};
  String lastSyncBtn = "", lastReconnBtn = "";
  uint32_t lastSensorPushMs = 0;

  const String tplBody =
    "{\"template\":\"{{ states('input_select.downstair_stair_mode')[0] }}|"
    "{{ states('input_number.downstair_step_brightness')|int(79) }}|"
    "{{ states('input_number.downstair_flow_brightness')|int(63) }}|"
    "{{ states('input_select.upstair_stair_mode')[0] }}|"
    "{{ states('input_number.upstair_step_brightness')|int(79) }}|"
    "{{ states('input_number.upstair_flow_brightness')|int(96) }}|"
    "{{ states('input_button.stair_sync_time') }}|"
    "{{ states('input_button.stair_ble_reconnect') }}\"}";

  while (true) {
    if (forceReconnect) {
      forceReconnect = false;
      if (bleMutex && xSemaphoreTake(bleMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (devs[activeIdx].client && devs[activeIdx].client->isConnected()) {
          devs[activeIdx].client->disconnect();
        }
        devs[activeIdx].connected = false;
        xSemaphoreGive(bleMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      scanAndConnect();
      uiDirty = true;
    }

    if (WiFi.status() == WL_CONNECTED) {
      for (int di = 0; di < 2; di++) {
        if (devs[di].pushToHa) {
          devs[di].pushToHa = false;
          pushDeviceToHa(di, haMode[di], haStep[di], haFlow[di]);
        }
      }
      bool forceSensor = (millis() - lastSensorPushMs > 15000);
      if (forceSensor) lastSensorPushMs = millis();
      pushSensorsToHa(forceSensor);

      if (millis() - lastLocalTouchMs > 1200) {
        String resp;
        int code = haPost("/api/template", tplBody, &resp);
        if (code == 200 && resp.length() >= 11) {
          String parts[8];
          int idx = 0, start = 0;
          for (int i = 0; i <= (int)resp.length() && idx < 8; i++) {
            if (i == (int)resp.length() || resp[i] == '|') {
              parts[idx++] = resp.substring(start, i);
              start = i + 1;
            }
          }
          if (idx == 8) {
            for (int di = 0; di < 2; di++) {
              int base = di * 3;
              int m = parts[base].charAt(0) - '0';
              int s = parts[base + 1].toInt();
              int f = parts[base + 2].toInt();
              if (m >= 0 && m <= 5) {
                if (haMode[di] == -1) { haMode[di] = m; devs[di].mode = m; }
                else if (m != haMode[di]) {
                  haMode[di] = m; devs[di].mode = m;
                  uint8_t p = m; sendPkt(devs[di], CMD_MODE, &p, 1); uiDirty = true;
                }
              }
              if (s >= 0 && s <= 100) {
                if (haStep[di] == -1) { haStep[di] = s; devs[di].briStep = s; }
                else if (s != haStep[di]) {
                  haStep[di] = s; devs[di].briStep = s;
                  uint8_t p = s; sendPkt(devs[di], CMD_STEP_BRI, &p, 1); uiDirty = true;
                }
              }
              if (f >= 0 && f <= 100) {
                if (haFlow[di] == -1) { haFlow[di] = f; devs[di].briFlow = f; }
                else if (f != haFlow[di]) {
                  haFlow[di] = f; devs[di].briFlow = f;
                  uint8_t p = f; sendPkt(devs[di], CMD_FLOW_BRI, &p, 1); uiDirty = true;
                }
              }
            }
            if (lastSyncBtn == "") lastSyncBtn = parts[6];
            else if (parts[6] != lastSyncBtn) {
              lastSyncBtn = parts[6];
              syncTimerToDevice(0);
              syncTimerToDevice(1);
              syncBanner = true;
              uiDirty = true;
            }
            if (lastReconnBtn == "") lastReconnBtn = parts[7];
            else if (parts[7] != lastReconnBtn) {
              lastReconnBtn = parts[7];
              forceReconnect = true;
            }
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(800));
  }
}

// ---------- UI (Core 1) ----------
static LGFX lcd;
static int W = 320, H = 240;
struct Btn { int x, y, w, h; const char* label; uint8_t val; };
static Btn modeBtns[6] = {
  {4,  42, 100, 30, "ALL ON", 0}, {108, 42, 100, 30, "ALT",    1}, {212, 42, 100, 30, "FLOW",   2},
  {4,  76, 100, 30, "STEP",   3}, {108, 76, 100, 30, "CORNER", 4}, {212, 76, 100, 30, "RANDOM", 5},
};
static bool isIn(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void drawTopBar() {
  lcd.fillRect(0, 0, W, 28, lcd.color565(20, 20, 30));
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(6, 5);
  lcd.print("STAIR LIGHT");
  for (int i = 0; i < 2; i++) {
    int bx = 170 + i * 74, by = 4, bw = 68, bh = 20;
    bool sel = (i == activeIdx);
    lcd.fillRoundRect(bx, by, bw, bh, 6, sel ? lcd.color565(0, 180, 120) : lcd.color565(50, 50, 60));
    lcd.setTextColor(TFT_WHITE);
    lcd.setCursor(bx + 10, by + 6);
    lcd.print(devs[i].label);
    lcd.fillCircle(bx + bw - 10, by + 10, 4,
                   devs[i].connected ? lcd.color565(0, 255, 0) : lcd.color565(255, 0, 0));
  }
  lcd.setCursor(6, 17);
  lcd.setTextColor(lcd.color565(140, 200, 160));
  if (WiFi.status() == WL_CONNECTED) lcd.print(WiFi.localIP().toString());
  else lcd.print("WiFi connecting...");
}

static void drawModes() {
  lcd.setTextSize(1);
  lcd.setTextColor(lcd.color565(160, 160, 160));
  lcd.setCursor(4, 30);
  lcd.print("MODE");
  for (auto &b : modeBtns) {
    bool sel = (devs[activeIdx].mode == b.val);
    lcd.fillRoundRect(b.x, b.y, b.w, b.h, 6,
                      sel ? lcd.color565(0, 200, 100) : lcd.color565(40, 40, 50));
    lcd.setTextColor(TFT_WHITE);
    lcd.setCursor(b.x + 22, b.y + 10);
    lcd.print(b.label);
  }
}

static void drawSliders() {
  auto &d = devs[activeIdx];
  const int sx = 4, sw = 250, sy1 = 118, sy2 = 152, sh = 12;
  lcd.fillRect(0, 102, W, 68, lcd.color565(12, 12, 18));
  lcd.setTextSize(1);
  lcd.setTextColor(lcd.color565(160, 160, 160));
  lcd.setCursor(4, 104);
  lcd.print("STEP BRI");
  lcd.fillRoundRect(sx, sy1, sw, sh, 5, lcd.color565(30, 30, 40));
  int fx1 = map(d.briStep, 0, 100, 0, sw);
  lcd.fillRoundRect(sx, sy1, fx1, sh, 5, lcd.color565(0, 200, 255));
  lcd.fillCircle(sx + fx1, sy1 + 6, 9, TFT_WHITE);
  lcd.setTextColor(TFT_CYAN);
  lcd.setCursor(sx + sw + 10, sy1 - 1);
  lcd.printf("%d%%", d.briStep);

  lcd.setTextColor(lcd.color565(160, 160, 160));
  lcd.setCursor(4, 138);
  lcd.print("FLOW BRI");
  lcd.fillRoundRect(sx, sy2, sw, sh, 5, lcd.color565(30, 30, 40));
  int fx2 = map(d.briFlow, 0, 100, 0, sw);
  lcd.fillRoundRect(sx, sy2, fx2, sh, 5, lcd.color565(255, 180, 0));
  lcd.fillCircle(sx + fx2, sy2 + 6, 9, TFT_WHITE);
  lcd.setTextColor(lcd.color565(255, 200, 0));
  lcd.setCursor(sx + sw + 10, sy2 - 1);
  lcd.printf("%d%%", d.briFlow);
}

static void drawFooter() {
  lcd.fillRect(0, 170, W, 70, lcd.color565(12, 12, 18));
  lcd.setTextSize(1);
  lcd.setTextColor(lcd.color565(160, 220, 255));
  lcd.setCursor(4, 172);
  lcd.printf("HA:%s  DN:%s  UP:%s", haOk ? "OK" : "--",
             devs[0].connected ? "OK" : "--", devs[1].connected ? "OK" : "--");

  if (syncBanner) {
    lcd.fillRoundRect(4, 188, 152, 30, 6, TFT_GREEN);
    lcd.setTextColor(TFT_BLACK);
    lcd.setCursor(48, 198);
    lcd.print("SYNCED!");
  } else {
    lcd.fillRoundRect(4, 188, 152, 30, 6, lcd.color565(60, 80, 160));
    lcd.setTextColor(TFT_WHITE);
    lcd.setCursor(36, 198);
    lcd.print("SYNC TIME");
  }
  lcd.fillRoundRect(164, 188, 152, 30, 6, lcd.color565(80, 60, 60));
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(196, 198);
  lcd.print("RECONNECT");
}

static void redrawContent() { drawTopBar(); drawModes(); drawSliders(); drawFooter(); }
static void redrawAll() { lcd.fillScreen(lcd.color565(12, 12, 18)); redrawContent(); }

static uint32_t lastTouchMs = 0;
static void handleTouch() {
  uint16_t x, y;
  if (!lcd.getTouch(&x, &y)) return;
  if (millis() - lastTouchMs < 180) return;
  lastTouchMs = millis();
  lastLocalTouchMs = millis();

  for (int i = 0; i < 2; i++) {
    int bx = 170 + i * 74, by = 4, bw = 68, bh = 20;
    if (isIn(x, y, bx, by, bw, bh)) { activeIdx = i; redrawAll(); return; }
  }
  for (auto &b : modeBtns) {
    if (isIn(x, y, b.x, b.y, b.w, b.h)) {
      devs[activeIdx].mode = b.val;
      uint8_t p = b.val;
      sendPkt(devs[activeIdx], CMD_MODE, &p, 1);
      devs[activeIdx].pushToHa = true;
      drawModes();
      return;
    }
  }
  if (y >= 110 && y <= 132) {
    int v = constrain(map((int)x, 4, 254, 0, 100), 0, 100);
    devs[activeIdx].briStep = v;
    uint8_t p = v;
    sendPkt(devs[activeIdx], CMD_STEP_BRI, &p, 1);
    devs[activeIdx].pushToHa = true;
    drawSliders();
    return;
  }
  if (y >= 144 && y <= 166) {
    int v = constrain(map((int)x, 4, 254, 0, 100), 0, 100);
    devs[activeIdx].briFlow = v;
    uint8_t p = v;
    sendPkt(devs[activeIdx], CMD_FLOW_BRI, &p, 1);
    devs[activeIdx].pushToHa = true;
    drawSliders();
    return;
  }
  if (isIn(x, y, 4, 188, 152, 30)) {
    syncTimerToDevice(activeIdx);
    syncBanner = true;
    drawFooter();
    delay(350);
    syncBanner = false;
    drawFooter();
    return;
  }
  if (isIn(x, y, 164, 188, 152, 30)) { forceReconnect = true; return; }
}

// ---------- setup / loop ----------
static void applyStaticIp() {
  IPAddress ip, gw, mask;
  if (!ip.fromString(STATIC_IP) || !gw.fromString(STATIC_GW) || !mask.fromString(STATIC_MASK)) return;
  if ((uint32_t)ip == 0) return;
  WiFi.config(ip, gw, mask);
}

static void maintainWiFi() {
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void setup() {
  pinMode(pins::AMP_EN, OUTPUT);
  digitalWrite(pins::AMP_EN, HIGH);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[BOOT] BLE Stair Light Panel %s\n", FW_VERSION);

  bleMutex = xSemaphoreCreateMutex();
  initDevs();

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);
  W = lcd.width();
  H = lcd.height();
  redrawAll();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOST);
  applyStaticIp();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] http://%s/\n", WiFi.localIP().toString().c_str());
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/ota", HTTP_POST, []() {
    if (otaRejected) {
      otaRejected = false;
      server.requestAuthentication();
      return;
    }
    bool ok = !Update.hasError();
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    delay(300);
    if (ok) ESP.restart();
  }, []() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
      if (!webAuthOk()) { otaRejected = true; return; }
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!otaRejected) Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
      if (!otaRejected) Update.end(true);
    }
  });
  server.begin();

  NimBLEDevice::init("StairPanel");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  scanner = NimBLEDevice::getScan();
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(60);

  scanAndConnect();
  redrawContent();

  xTaskCreatePinnedToCore(haTask, "haTask", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  server.handleClient();

  static uint32_t lastTouchPoll = 0;
  if (millis() - lastTouchPoll >= 25) {
    lastTouchPoll = millis();
    handleTouch();
  }

  if (uiDirty) {
    uiDirty = false;
    redrawContent();
    if (syncBanner) { delay(350); syncBanner = false; drawFooter(); }
  }

  if (forceReconnect) {
    forceReconnect = false;
    if (bleMutex && xSemaphoreTake(bleMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      if (devs[activeIdx].client && devs[activeIdx].client->isConnected()) {
        devs[activeIdx].client->disconnect();
      }
      devs[activeIdx].connected = false;
      xSemaphoreGive(bleMutex);
    }
    drawTopBar();
    delay(250);
    scanAndConnect();
    redrawAll();
  }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (!devs[0].connected || !devs[1].connected) scanAndConnect();
    drawTopBar();
    drawFooter();
  }

  maintainWiFi();
  delay(2);
}
