#!/usr/bin/env python3
"""
Mac side of the ESP32 serial relay (approach B).

Topology:

    phone --BLE--> ESP32 (tools/relay/esp32-relay) --USB serial--> this script
                                                                      |
                                                                      +--BLE--> real light

The ESP32 prints every phone write as `TX_FFE1:<hex>`. This script forwards it
to the real controller with Bleak and writes the controller's notifications
back as `RX:<hex>` lines.

Requires:
    pip install bleak pyserial

Usage:
    python3 ble_serial_relay.py --address <UUID-or-MAC> [--port /dev/cu.usbmodemXXXX]

Quit with Ctrl-C.
"""

import argparse
import asyncio
import glob
import queue
import sys
import threading
import time

import serial
from bleak import BleakClient

CHAR_FFE1_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB"
CHAR_FFE2_UUID = "0000FFE2-0000-1000-8000-00805F9B34FB"

CFG = {"address": "", "port": "", "baud": 115200, "log": "ble-serial-relay.log"}
LOG_LOCK = threading.Lock()

to_real = None      # asyncio.Queue: (char_uuid, bytes) -> real device
to_phone = queue.Queue()   # str hex -> serial thread writes "RX:<hex>"


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    with LOG_LOCK:
        print(line, flush=True)
        try:
            with open(CFG["log"], "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass


def find_port():
    patterns = ["/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"]
    for pat in patterns:
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[0]
    return None


def serial_worker(loop):
    """Owns the serial port: parses TX_* lines, drains to_phone."""
    ser = None
    buf = b""
    while True:
        if ser is None or not ser.is_open:
            port = CFG["port"] or find_port()
            if not port:
                time.sleep(1.0)
                continue
            try:
                ser = serial.Serial(port, CFG["baud"], timeout=0.05)
                log("serial open: %s" % port)
            except Exception as e:
                log("serial open failed: %s" % e)
                time.sleep(1.0)
                continue

        try:
            chunk = ser.read(256)
            while not to_phone.empty():
                hexstr = to_phone.get_nowait()
                ser.write(("RX:" + hexstr + "\n").encode())
                log("<< RX to ESP32: %s" % hexstr)
        except Exception as e:
            log("serial error: %s" % e)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(1.0)
            continue

        if not chunk:
            continue

        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            log("[ESP32] %s" % text)
            if text.startswith("TX_FFE1:"):
                try:
                    data = bytes.fromhex(text.split(":", 1)[1].strip())
                except ValueError:
                    continue
                if to_real is not None:
                    loop.call_soon_threadsafe(to_real.put_nowait, (CHAR_FFE1_UUID, data))
            elif text.startswith("TX_FFE2:"):
                try:
                    data = bytes.fromhex(text.split(":", 1)[1].strip())
                except ValueError:
                    continue
                if to_real is not None:
                    loop.call_soon_threadsafe(to_real.put_nowait, (CHAR_FFE2_UUID, data))


async def ble_worker():
    global to_real
    to_real = asyncio.Queue()

    log("connecting to real device %s ..." % CFG["address"])
    while True:
        try:
            async with BleakClient(CFG["address"], timeout=20.0) as client:
                log("connected to real device")

                def on_notify(_sender, data):
                    b = bytes(data)
                    log("<< real notify hex=%s" % b.hex())
                    to_phone.put(b.hex())

                await client.start_notify(CHAR_FFE1_UUID, on_notify)
                log("subscribed to real FFE1 - open the phone app")

                while client.is_connected:
                    try:
                        target, payload = await asyncio.wait_for(to_real.get(), timeout=1.0)
                        log(">> forward to real %s hex=%s" % (target, payload.hex()))
                        await client.write_gatt_char(target, payload, response=False)
                    except asyncio.TimeoutError:
                        pass
        except Exception as e:
            log("real device error: %s (retrying in 2s)" % e)
        await asyncio.sleep(2.0)


def main():
    ap = argparse.ArgumentParser(description="ESP32 serial relay - Mac side")
    ap.add_argument("--address", required=True, help="real device UUID/MAC as seen by Bleak")
    ap.add_argument("--port", default="", help="serial port (default: auto-detect)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default="ble-serial-relay.log")
    args = ap.parse_args()

    CFG.update(address=args.address, port=args.port, baud=args.baud, log=args.log)
    open(CFG["log"], "w").close()
    log("starting serial relay, real=%s" % CFG["address"])

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    threading.Thread(target=serial_worker, args=(loop,), daemon=True).start()

    try:
        loop.run_until_complete(ble_worker())
    except KeyboardInterrupt:
        log("stopped")


if __name__ == "__main__":
    sys.exit(main())
