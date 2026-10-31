# BLE relay toolkit

These are the tools used to capture the vendor protocol described in
[docs/REVERSE-ENGINEERING.md](../../docs/REVERSE-ENGINEERING.md).

They let a phone app talk to a fake device while every frame is logged and
forwarded to the real controller.

> Use them only on hardware you own.

## Two implementations

| | A — macOS proxy | B — ESP32 serial relay |
|---|---|---|
| Fake peripheral runs on | the Mac (CoreBluetooth) | an ESP32 |
| Real link runs on | the Mac (Bleak) | the Mac (Bleak) |
| Reliability | flaky | solid |
| Extra hardware | none | any ESP32 |
| Recommended | no | **yes** |

Both need the real controller to stop advertising. That happens automatically
when you connect to it: most BLE modules stop advertising while connected.

## Requirements

```bash
python3 -m pip install -r requirements.txt
```

`pyobjc-framework-CoreBluetooth` is macOS-only and only needed for option A.

## Option B — ESP32 serial relay (recommended)

### 1. Flash the relay firmware

```bash
cd esp32-relay
pio run -t upload
```

The default advertised name is `downstair`. For the other controller:

```bash
pio run -t upload -DRELAY_NAME=\"upstair\"
```

### 2. Find the real device address

```bash
python3 -m bleak.discover
```

On macOS this is a CoreBluetooth UUID (not a MAC); on Linux it is a MAC.

### 3. Start the relay

```bash
python3 ble_serial_relay.py --address <real-uuid>
```

The script connects to the real controller (which then stops advertising) and
waits for the ESP32.

### 4. Open the phone app

The app should discover the fake device and connect. The log shows every frame:

```
[20:42:46] [ESP32] TX_FFE1:aaafba03142a2e82
[20:42:46] >> forward to real 0000ffe1-... hex=aaafba03142a2e82
[20:42:47] << real notify hex=5550ba100001010d050101585e0101001200172d93
[20:42:47] << RX to ESP32: 5550ba100001010d050101585e0101001200172d93
```

## Option A — macOS proxy

```bash
python3 ble_proxy_macos.py --name downstair --address <real-uuid>
```

Wait for:

```
advertising as 'downstair' - open the phone app now
```

then open the app.

This mode is kept for completeness: CoreBluetooth advertising and a Bleak
central connection in the same process occasionally fight over the radio, and
iOS caches GATT aggressively. Option B exists because of those failures.

## Log format

| Prefix | Meaning |
|---|---|
| `TX_FFE1:<hex>` | the phone wrote this to FFE1 |
| `TX_FFE2:<hex>` | the phone wrote this to FFE2 (AT channel) |
| `RX:<hex>` | notify the phone with these bytes |
| `EVENT:PHONE_CONNECTED` | the phone connected to the relay |
| `EVENT:ADVERTISING` | the relay is discoverable |

## Replaying a frame

Once captured, a frame can be sent directly without the relay:

```python
import asyncio
from bleak import BleakClient

ADDR = "<real-uuid>"
CHAR = "0000FFE1-0000-1000-8000-00805F9B34FB"

async def main():
    async with BleakClient(ADDR) as c:
        await c.write_gatt_char(CHAR, bytes.fromhex("aaafb50102b7"), response=False)

asyncio.run(main())
```

## Caveats

- Close the vendor app before starting: the controller accepts one central.
- The real device must be connected (and therefore silent) before advertising.
- iOS caches GATT; toggle Bluetooth after changing the proxy layout.
- Frames with a bad checksum are ignored silently — no error is returned.
- Some commands reply with a short ACK, some do not. Never block on a reply.

## License

Apache-2.0, same as the rest of the repository.
