# Reverse-engineering the BLE stair light

This document records **how** the protocol in [BLE-PROTOCOL.md](BLE-PROTOCOL.md)
was obtained, not just the result. It covers the relay setup, the first capture,
the command sweep, and the dead ends.

> Everything here was done on hardware we own. Impersonating a BLE device that
> belongs to someone else is not okay.

## TL;DR

1. Scan with Bleak, dump the GATT table: service `FFE0`, characteristic `FFE1`,
   no pairing.
2. The vendor app only talks to a device that advertises the right name. To see
   its traffic, become that device.
3. **Trick:** connect to the real controller first — most BLE modules stop
   advertising while connected, so the phone now sees only our proxy.
4. Relay phone writes to the real device and real notifications back to the
   phone. Log every frame.
5. The app sends `BA` (time sync) immediately on connect; the reply leaks the
   current mode and both brightness values.
6. Replay the captured frames from the ESP32 and confirm the light obeys.

## 1. Discovery

A plain Bleak scan found two devices advertising the names `upstair` and
`downstair` at roughly −72 dBm. The advertisement contained manufacturer data
with the module's MAC (vendor ID `0x4458`, a DX-BT24 transparent-serial module).

Connecting without pairing revealed the same GATT table on both units:

| | |
|---|---|
| Service | `0000FFE0-...` |
| `FFE1` | read, notify, write, write-without-response |
| `FFE2` | write, write-without-response (AT commands) |

`FFE1` is a serial-over-BLE pipe to the light's MCU. Everything the app does
goes through it as raw bytes.

## 2. The core trick: make the real device disappear

You cannot simply "listen" to BLE. A central connects to a peripheral; if the
phone is connected to the light, a second central cannot join.

The way in is the advertising behaviour: **when a BLE peripheral is connected,
it usually stops advertising.** So:

```
1. Our machine connects to the real "downstair"  -> real device stops advertising
2. Our machine advertises as "downstair"          -> phone sees only us
3. Phone connects to us                           -> we are in the middle
```

From that point every write is ours to read, and every notification is ours to
forward.

## 3. Relay A — macOS as a fake peripheral

Topology:

```
 iPhone app  ──BLE──▶  Mac (CBPeripheralManager, advertises "upstair")
                          │
                          │ Bleak (central)
                          ▼
                       real "upstair" light
```

The Mac runs two BLE roles at once:

- `CBPeripheralManager` advertises service `FFE0` with characteristics `FFE1`
  and `FFE2`, and receives the phone's writes;
- `BleakClient` holds a connection to the real light and forwards frames.

Sketch:

```python
# phone -> real device
def peripheralManager_didReceiveWriteRequests_(self, pm, requests):
    for req in requests:
        data = bytes(req.value())
        target = CHAR_FFE1 if req.characteristic().UUID() == CHAR_FFE1 \
                 else CHAR_FFE2
        tx_queue.put_nowait((target, data))

# real device -> phone
def on_real_notify(sender, data):
    periph_delegate.notify_phone(bytes(data))
```

It worked, but it was fragile:

- macOS occasionally refused to start advertising, or dropped the advertisement
  after a reconnect;
- the phone sometimes reconnected to the real device before the proxy was up;
- iOS caches GATT services, so stale handles had to be invalidated by
  toggling Bluetooth;
- running a peripheral and a central in the same process made timing-sensitive
  failures hard to reproduce.

## 4. Relay B — ESP32 serial relay (the one that stuck)

The stable version splits the two roles: the ESP32 owns the peripheral side,
the Mac owns the central side, and they talk over USB serial.

```
 iPhone app ──BLE──▶ ESP32 (advertises "downstair")
                        │  USB serial, line protocol
                        ▼
                     Mac (Bleak central) ──BLE──▶ real "downstair"
```

Line protocol (one frame per line, 115200 baud):

| Direction | Line | Meaning |
|---|---|---|
| ESP32 → Mac | `TX_FFE1:aaafba03142a2e82` | the phone wrote this to FFE1 |
| ESP32 → Mac | `TX_FFE2:...` | same for FFE2 / AT channel |
| Mac → ESP32 | `RX:5550ba1000...` | notify the phone with these bytes |
| ESP32 → Mac | `EVENT:PHONE_CONNECTED` | bookkeeping |

The ESP32 sketch is deliberately dumb: advertise `FFE0`/`FFE1`/`FFE2` with the
vendored local name, print writes as hex, and notify the phone when a line
arrives. All forwarding logic and logging lives on the Mac, where it is easy to
edit and replay.

Why this won:

- advertising is deterministic on the ESP32;
- the Mac only maintains one BLE link, so no role conflicts;
- the serial log is a clean, timestamped capture;
- replay is trivial: write the same hex to the real device with Bleak.

## 5. First capture: what the app does on connect

The app sends a time sync immediately after connecting:

```
[20:42:46] [ESP32] TX_FFE1:aaafba03142a2e82
[20:42:46] >>> FORWARDING TO REAL DOWNSTAIR: hex=aaafba03142a2e82
[20:42:47] <<< REAL DOWNSTAIR NOTIFY: 5550ba100001010d050101585e0101001200172d93
```

Decoding the command:

```
AA AF  BA  03  14 2A 2E  82
│  │   │   │   └─┬─┘ └┬┘  └ checksum
│  │   │   │     │    └ 0x2E = 46 seconds
│  │   │   │     └ 0x2A = 42 minutes
│  │   │   └ 0x14 = 20 hours
│  │   └ BA = time sync
│  └ AF = magic
└ AA = magic
```

The reply is the jackpot:

```
55 50 BA 10 | 00 01 01 0D 05 01 01 58 5E 01 01 00 12 00 17 2D | 93
             └────────────────── 16 bytes ──────────────────┘
```

| Byte | Value | Meaning |
|---|---|---|
| `[11]` | `0x58` = 88 | flow brightness |
| `[12]` | `0x5E` = 94 | step brightness |
| `[15]` | `0x00` | mode 0 (all on) |
| `[16..19]` | `12 00 17 2D` | timer 18:00 – 23:45 |

That single exchange gave us the state model: mode + two brightness channels +
the built-in timer, all readable without guessing.

The next frames were equally useful:

```
AA AF B9 01 4F 62    -> step brightness 0x4F = 79
AA AF B8 01 3F 51    -> flow brightness 0x3F = 63
```

Each was acknowledged with `55 50 B9 01 00 5F` / `55 50 B8 01 00 5E`, which
confirmed the checksum and the one-byte payload layout.

## 6. Filling in the command table

With a working relay, the rest was systematic:

1. **Read the app's UI actions.** Change one thing at a time (mode, each
   slider, timer) and diff the captured frames.
2. **Replay from the ESP32.** Send the captured bytes from our own code and
   confirm the physical light reacts. A frame that works from the app but not
   from us means we mangled the checksum or the length.
3. **Sweep unknown opcodes.** `brute_power.py` walked `B0`–`BF` with short
   payloads while watching the light. The log shows additional commands the app
   uses (`B2`, `B4`, `B6`, `BB`, `BD`) that are not needed for the panel.
4. **Look for a power command.** We did not find a separate on/off opcode in
   that range; mode `0` (all on) is the closest thing. The panel therefore
   treats brightness 0 as "off" when exposing a light entity to HomeKit.

## 7. Verification

The final check is the one that matters: the ESP32 panel, with the phone app
closed and the relay disconnected, sends the frames itself.

```
AA AF B5 01 02 B7     -> mode 2 (flow)
AA AF B9 01 50 64     -> step brightness 80
AA AF BE 04 12 00 17 2D 71 -> timer 18:00–23:45
```

If the light obeys, the protocol is understood well enough to own the device.

## 8. Gotchas

- **The real device must stop advertising.** Connect to it before advertising
  the proxy, otherwise the phone may pick the real one.
- **One central at a time.** The controllers accept a single connection; the
  vendor app must be closed while the panel is connected.
- **iOS caches GATT.** After changing the proxy's characteristic layout,
  toggle Bluetooth on the phone before testing again.
- **The checksum is a simple sum.** A single wrong byte makes the controller
  ignore the frame silently — no error, no notification.
- **Notifications are the only feedback.** Some commands reply with a short
  ACK (`55 50 <cmd> 01 00 <chk>`), some do not. Do not block waiting for one.
- **Timing matters on connect.** The app sends `BA` within a second; sending it
  too early (before the notification subscription is ready) loses the reply.

## 9. Ethics

This was done on hardware we bought and own, to replace a vendor app that is no
longer maintained. The same technique works on many BLE devices — please only
use it on your own.
