/*
 * BLE relay firmware (peripheral side)
 * ------------------------------------
 * Advertises the same name/service as a stair light controller, prints every
 * write the phone makes to USB serial, and notifies the phone when a line
 * arrives from the Mac.
 *
 * Line protocol (115200 baud):
 *   ESP32 -> Mac : TX_FFE1:<hex>      phone wrote to FFE1
 *                  TX_FFE2:<hex>      phone wrote to FFE2
 *                  EVENT:PHONE_CONNECTED / EVENT:PHONE_DISCONNECTED
 *                  EVENT:READY
 *   Mac -> ESP32 : RX:<hex>           notify the phone on FFE1
 *                  PING                replies PONG
 *
 * Build:
 *   pio run -t upload                 (default name "downstair")
 *   pio run -t upload -DRELAY_NAME=\"upstair\"
 *
 * License: Apache-2.0
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

#ifndef RELAY_NAME
#define RELAY_NAME "downstair"
#endif

static NimBLECharacteristic* chrFfe1 = nullptr;
static NimBLECharacteristic* chrFfe2 = nullptr;

static void printHex(const char* tag, const uint8_t* data, size_t len) {
  Serial.print(tag);
  for (size_t i = 0; i < len; i++) {
    char b[3];
    snprintf(b, sizeof(b), "%02x", data[i]);
    Serial.print(b);
  }
  Serial.println();
}

class CharCB : public NimBLECharacteristicCallbacks {
 public:
  explicit CharCB(const char* tag) : tag_(tag) {}
  void onWrite(NimBLECharacteristic* c) override {
    NimBLEAttValue v = c->getValue();
    printHex(tag_, v.data(), v.size());
  }
 private:
  const char* tag_;
};

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    Serial.println("EVENT:PHONE_CONNECTED");
  }
  void onDisconnect(NimBLEServer*) override {
    Serial.println("EVENT:PHONE_DISCONNECTED");
    NimBLEDevice::startAdvertising();
  }
};

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse "RX:0011aaff" into bytes. Returns the number of bytes written.
static size_t parseHex(const char* s, uint8_t* out, size_t maxLen) {
  size_t n = 0;
  while (s[0] && s[1] && n < maxLen) {
    int hi = hexVal(s[0]);
    int lo = hexVal(s[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = (uint8_t)((hi << 4) | lo);
    s += 2;
  }
  return n;
}

static void handleLine(char* line) {
  if (strncmp(line, "RX:", 3) == 0) {
    uint8_t buf[256];
    size_t n = parseHex(line + 3, buf, sizeof(buf));
    if (n == 0) {
      Serial.println("EVENT:RX_EMPTY");
      return;
    }
    if (!chrFfe1) return;
    chrFfe1->setValue(buf, n);
    chrFfe1->notify();
    printHex("EVENT:RX_OK:", buf, n);
    return;
  }
  if (strcmp(line, "PING") == 0) {
    Serial.println("PONG");
    return;
  }
  if (line[0]) {
    Serial.print("EVENT:UNKNOWN:");
    Serial.println(line);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.print("EVENT:READY name=");
  Serial.println(RELAY_NAME);

  NimBLEDevice::init(RELAY_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(true);

  NimBLEService* svc = server->createService("FFE0");

  chrFfe1 = svc->createCharacteristic(
      "FFE1",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
          NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  chrFfe1->setCallbacks(new CharCB("TX_FFE1:"));

  chrFfe2 = svc->createCharacteristic(
      "FFE2", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrFfe2->setCallbacks(new CharCB("TX_FFE2:"));

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID("FFE0");
  adv->setName(RELAY_NAME);
  adv->setScanResponse(true);
  adv->start();

  Serial.println("EVENT:ADVERTISING");
}

void loop() {
  static char line[768];
  static size_t pos = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      line[pos] = 0;
      handleLine(line);
      pos = 0;
    } else if (c != '\r' && pos < sizeof(line) - 1) {
      line[pos++] = c;
    }
  }
  delay(1);
}
