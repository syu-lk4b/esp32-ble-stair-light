# ESP32 BLE Stair Light Panel

**A touch panel that takes over cheap BLE stair step lights and puts them on Home Assistant and HomeKit.**

English · [简体中文](README.zh-CN.md)

---

These "stair step lights" are sold under a dozen brand names with a phone app
and an RF remote. The app is slow, the remote gets lost, and nothing integrates
with the rest of the house.

This project replaces the app with a 2.8" touch panel built on an ESP32-S3:

- scans for the two controllers (`upstair` / `downstair`) and connects over BLE;
- six lighting modes, two independent brightness channels;
- syncs clock and the built-in timer;
- mirrors everything to Home Assistant, which exposes it to HomeKit / Siri;
- reconnects automatically when a controller drops off;
- OTA updates over Wi-Fi.

No cloud, no vendor app, no account.

[![Architecture](docs/architecture.png)](docs/architecture.html)

> The image links to the interactive diagram (`docs/architecture.html`).

---

## Contents

- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Manual install](#manual-install)
- [Home Assistant setup](#home-assistant-setup)
- [HomeKit](#homekit)
- [Web API](#web-api)
- [BLE protocol](#ble-protocol)
- [Implementation notes](#implementation-notes)
- [Troubleshooting](#troubleshooting)
- [Compatibility](#compatibility)
- [License](#license)

---

## Hardware

| Part | Requirement |
|---|---|
| Board | **ES3C28P**: ESP32-S3, 16 MB flash, 2.8" 320×240 touch panel |
| Display driver | ST7789 + CST820 touch (configured in `include/lgfx_es3c28p.h`) |
| Power | 5 V USB |
| Stair controllers | The BLE type with an `FFE0` service and an `FFE1` characteristic, advertising as `upstair` / `downstair` |

The firmware is board-specific only in `include/lgfx_es3c28p.h` (LCD/touch
pins) and `platformio.ini` (PSRAM mode). Any ESP32-S3 with a compatible panel
can be used by editing those two files.

---

## Quick start

```bash
git clone https://github.com/syu-lk4b/esp32-ble-stair-light
cd esp32-ble-stair-light
./setup.sh
```

`setup.sh` walks through:

1. dependency checks;
2. Wi-Fi, Home Assistant URL/token, BLE device names, timer, optional static IP;
3. building and flashing the firmware;
4. finding the panel on the LAN and printing its BLE/HA status;
5. installing the Home Assistant helpers;
6. an optional smoke test (switch the downstairs light to STEP mode).

Already flashed?

```bash
./setup.sh --skip-flash
```

---

## Manual install

### 1. Secrets

```bash
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h
```

The important fields:

```c
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-password"
#define HA_BASE_URL "http://homeassistant.local:8123"
#define HA_TOKEN "eyJ..."
#define BLE_NAME_DOWN "downstair"
#define BLE_NAME_UP   "upstair"
```

Create the HA token under **Profile → Security → Long-lived access tokens**.

### 2. Build and flash

```bash
pio run
pio run -t upload
```

### 3. Verify

```bash
curl http://stair-panel.local/status
```

```json
{
  "fw": "1.0.0",
  "ip": "192.168.1.60",
  "ha": true,
  "downstair": { "connected": true, "mode": 3, "bri_step": 79, "bri_flow": 63 },
  "upstair":   { "connected": true, "mode": 1, "bri_step": 79, "bri_flow": 96 }
}
```

If `ha` is `false`, check the URL and token. If a controller is
`"connected": false`, make sure the vendor app is closed and the panel is
within BLE range.

---

## Home Assistant setup

### 1. Install the helpers

```bash
cp homeassistant/stair_light_package.yaml <ha-config>/packages/stair_light.yaml
```

In `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Restart HA. This creates:

| Entity | Type | Purpose |
|---|---|---|
| `input_select.downstair_stair_mode` | select | mode 0–5 |
| `input_select.upstair_stair_mode` | select | mode 0–5 |
| `input_number.downstair_step_brightness` | number | 0–100 |
| `input_number.downstair_flow_brightness` | number | 0–100 |
| `input_number.upstair_step_brightness` | number | 0–100 |
| `input_number.upstair_flow_brightness` | number | 0–100 |
| `input_button.stair_sync_time` | button | push clock + timer |
| `input_button.stair_ble_reconnect` | button | force a BLE reconnect |

The panel also publishes two connectivity sensors through the REST API:

```
binary_sensor.downstair_stair_ble
binary_sensor.upstair_stair_ble
```

They are recreated automatically within 15 seconds of an HA restart.

### 2. Dashboard

`homeassistant/dashboard_card.yaml` contains a ready-made card.

### 3. How the sync works

- The panel polls HA every ~800 ms with `/api/template`.
- Changes made in HA are pushed to the BLE controllers.
- Changes made on the touch panel or by the physical remote are written back
  to HA within a second.
- While you are touching the panel, HA reads are paused for 1.2 s so the two
  sides do not fight over the same value.

### 4. Option names

The option strings are Chinese because they come from the vendor app:

| Value | Option |
|---|---|
| 0 | `0-全亮模式` (all on) |
| 1 | `1-交替模式` (alternate) |
| 2 | `2-流水模式` (flow) |
| 3 | `3-逐步模式` (step) |
| 4 | `4-跟随模式` (follow) |
| 5 | `5-随机模式` (random) |

`MODE_OPTIONS` in `src/main.cpp` and the `input_select` options must match
exactly. If you rename them, update both and the template light file.

---

## HomeKit

HomeKit is **not** implemented on the ESP32. The panel talks to Home Assistant,
and HA's built-in **HomeKit Bridge** publishes the entities to Apple Home.

1. Install the optional template lights:
   `homeassistant/template_lights.example.yaml`
2. Settings → Devices & Services → Add Integration → **HomeKit Bridge**.
3. Include `light.downstair_stair_light` and `light.upstair_stair_light`.
4. Scan the pairing QR code in the Apple Home app.

> The BLE protocol has no explicit power command. The template light treats
> "step brightness > 0" as on; adjust it if your controller behaves differently.

---

## Web API

`http://stair-panel.local/` serves a small control page. Every endpoint accepts
optional HTTP Basic auth (`WEB_USER` / `WEB_PASS`).

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | control page |
| `GET` | `/status` | device + BLE + HA status (JSON) |
| `POST` | `/set?dev=0&mode=3` | set mode (`dev` 0 = downstairs, 1 = upstairs) |
| `POST` | `/set?dev=0&bri_step=80` | set step brightness |
| `POST` | `/set?dev=0&bri_flow=60` | set flow brightness |
| `POST` | `/set?dev=0&sync_time=1` | push clock + timer |
| `POST` | `/set?reconnect=1` | force a BLE reconnect |
| `POST` | `/ota` | upload `firmware.bin` |

OTA:

```bash
pio run
curl -u admin:****** -F "file=@.pio/build/es3c28p/firmware.bin" \
     http://stair-panel.local/ota
```

---

## BLE protocol

See [docs/BLE-PROTOCOL.md](docs/BLE-PROTOCOL.md) for the full reverse-engineered
spec, and [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md) for how it
was captured: the fake-peripheral relay, the first packet, and the command
sweep.

The short version:

```
AA AF CMD LEN payload... CHK        CHK = sum(previous bytes) & 0xFF
```

| CMD | Payload | Meaning |
|---|---|---|
| `B5` | mode 0–5 | lighting mode |
| `B9` | 0–100 | step brightness |
| `B8` | 0–100 | flow brightness |
| `BE` | on HH MM off HH MM | built-in timer |
| `BA` | HH MM SS | time sync + config query |

Reply: `55 50 BA 10 <16 bytes>`, with flow at `[11]`, step at `[12]`, mode at
`[15]`.

---

## Implementation notes

### 1. One panel, two BLE links

The ESP32 is a BLE **central**. `scanAndConnect()` runs an active scan for the
two advertised names and connects each one independently. Both links stay open
at the same time; a disconnect callback clears the state and the loop retries
every 10 seconds.

Commands are written to `FFE1`; replies arrive as notifications on the same
characteristic. A mutex serialises writes so the UI task and the HA task cannot
interleave half a frame.

### 2. HA as the source of truth (with a local override window)

Two loops write to the same six values: the panel and Home Assistant. Without
arbitration they echo each other forever.

The panel solves it with:

- `lastLocalTouchMs`: for 1.2 s after a touch, HA reads are skipped;
- per-field "last seen" caches (`haMode`, `haStep`, `haFlow`) so only actual
  changes are applied;
- `pushToHa` flags so a local change is written back exactly once.

### 3. The timer is the vendor's, not HA's

`BE 04 HH MM HH MM` writes the controller's **built-in** schedule. The panel
pushes it when you press `SYNC TIME`, together with the current clock (`BA`).
This keeps the lights working even if HA is down.

### 4. UI

LovyanGFX draws a 320×240 dashboard: two device tabs, six mode buttons, two
sliders, and sync/reconnect buttons. Touch is polled every 25 ms with a 180 ms
debounce; sliders map x-position to 0–100.

### 5. Network resilience

- Wi-Fi is reconnected every 5 s if the link drops.
- HA requests use a 2.5 s timeout and never block the UI (separate task on
  core 0).
- OTA uses the second 3 MB partition, so a failed update can be retried.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Both lights show `connected: false` | vendor app still connected; wrong BLE names | close the app; check `BLE_NAME_*` |
| Only one connects | out of range / controller asleep | move the panel; press a button on the light |
| `ha: false` | wrong URL or expired token | `curl -H "Authorization: Bearer ..." $HA_BASE_URL/api/` |
| HA shows nothing | package not loaded | check `packages:` and restart HA |
| Mode changes bounce back | option strings don't match | align `MODE_OPTIONS` and `input_select` |
| Display stays black | board/pins mismatch | edit `include/lgfx_es3c28p.h` |
| Touch is offset | rotation/calibration | adjust `lcd.setRotation()` |
| Web UI unreachable | mDNS | use the IP from `/status` |
| OTA fails | partition table | use the bundled `partitions_ota.csv` |

---

## Compatibility

| Item | Status |
|---|---|
| ES3C28P (ESP32-S3 + 2.8" panel) | ✅ tested |
| `upstair` / `downstair` FFE0/FFE1 controllers | ✅ tested |
| Other rebrands of the same controller | ⚠️ likely, verify the GATT UUIDs |
| Controllers with a different app/protocol | ❌ |
| Home Assistant 2024.x+ | ✅ |
| Apple Home via HA HomeKit Bridge | ✅ |

---

## License

Apache License 2.0. See [LICENSE](LICENSE).
