#!/usr/bin/env python3
"""
macOS BLE relay (approach A) - impersonate a stair light on the Mac itself.

The Mac does two things at once:
  1. connects to the real controller with Bleak (which makes it stop advertising);
  2. advertises the same name/service with CoreBluetooth, so the phone connects
     to the Mac instead of the real device.

Requires:
    pip install bleak pyobjc-framework-CoreBluetooth

Usage:
    python3 ble_proxy_macos.py --name downstair --address <UUID-or-MAC>

`--address` is the real controller as seen by Bleak: a CoreBluetooth UUID on
macOS, or a MAC address on Linux. Run `python3 -m bleak.discover` if unsure.

Quit with Ctrl-C.
"""

import argparse
import asyncio
import threading
import time

import objc
from Foundation import NSData, NSObject
import CoreBluetooth
from bleak import BleakClient
from PyObjCTools import AppHelper

SERVICE_UUID = "0000FFE0-0000-1000-8000-00805F9B34FB"
CHAR_FFE1_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB"
CHAR_FFE2_UUID = "0000FFE2-0000-1000-8000-00805F9B34FB"

CFG = {"name": "downstair", "address": "", "log": "ble-relay.log"}
LOG_LOCK = threading.Lock()

tx_queue = None
main_loop = None
periph_delegate = None
real_connected = threading.Event()


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    with LOG_LOCK:
        print(line, flush=True)
        try:
            with open(CFG["log"], "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass


class PeripheralDelegate(NSObject):
    def init(self):
        self = objc.super(PeripheralDelegate, self).init()
        if self is None:
            return None
        self.pm = None
        self.char_ffe1 = None
        self.char_ffe2 = None
        self.advertised = False
        return self

    # ---- setup ----
    def peripheralManagerDidUpdateState_(self, pm):
        log("CBPeripheralManager state=%s" % pm.state())
        if pm.state() == CoreBluetooth.CBManagerStatePoweredOn:
            self.setup_service(pm)

    def setup_service(self, pm):
        props_ffe1 = (
            CoreBluetooth.CBCharacteristicPropertyRead
            | CoreBluetooth.CBCharacteristicPropertyWrite
            | CoreBluetooth.CBCharacteristicPropertyWriteWithoutResponse
            | CoreBluetooth.CBCharacteristicPropertyNotify
        )
        perms_ffe1 = (
            CoreBluetooth.CBAttributePermissionsReadable
            | CoreBluetooth.CBAttributePermissionsWriteable
        )
        self.char_ffe1 = CoreBluetooth.CBMutableCharacteristic.alloc(
        ).initWithType_properties_value_permissions_(
            CoreBluetooth.CBUUID.UUIDWithString_("FFE1"),
            props_ffe1,
            None,
            perms_ffe1,
        )

        props_ffe2 = (
            CoreBluetooth.CBCharacteristicPropertyWrite
            | CoreBluetooth.CBCharacteristicPropertyWriteWithoutResponse
        )
        self.char_ffe2 = CoreBluetooth.CBMutableCharacteristic.alloc(
        ).initWithType_properties_value_permissions_(
            CoreBluetooth.CBUUID.UUIDWithString_("FFE2"),
            props_ffe2,
            None,
            CoreBluetooth.CBAttributePermissionsWriteable,
        )

        svc = CoreBluetooth.CBMutableService.alloc().initWithType_primary_(
            CoreBluetooth.CBUUID.UUIDWithString_("FFE0"), True
        )
        svc.setCharacteristics_([self.char_ffe1, self.char_ffe2])
        pm.addService_(svc)

    def peripheralManager_didAddService_error_(self, pm, service, error):
        if error:
            log("error adding service: %s" % error)
            return
        log("service FFE0 added (FFE1 + FFE2)")
        AppHelper.callLater(CFG["advertise_delay"], self.start_advertising, pm)

    def start_advertising(self, pm):
        if self.advertised:
            return
        if not real_connected.is_set():
            log("real device not connected yet - delaying advertising")
            AppHelper.callLater(2.0, self.start_advertising, pm)
            return
        adv = {
            CoreBluetooth.CBAdvertisementDataLocalNameKey: CFG["name"],
            CoreBluetooth.CBAdvertisementDataServiceUUIDsKey: [
                CoreBluetooth.CBUUID.UUIDWithString_("FFE0")
            ],
        }
        pm.startAdvertising_(adv)

    def peripheralManagerDidStartAdvertising_error_(self, pm, error):
        if error:
            log("advertising error: %s" % error)
        else:
            self.advertised = True
            log("advertising as '%s' - open the phone app now" % CFG["name"])

    # ---- traffic ----
    def peripheralManager_central_didSubscribeToCharacteristic_(self, pm, central, characteristic):
        log("phone subscribed to %s" % characteristic.UUID())

    def peripheralManager_central_didUnsubscribeFromCharacteristic_(self, pm, central, characteristic):
        log("phone unsubscribed from %s" % characteristic.UUID())

    def peripheralManager_didReceiveReadRequest_(self, pm, request):
        log("phone read %s" % request.characteristic().UUID())
        request.setValue_(NSData.dataWithBytes_length_(b"\x00" * 20, 20))
        pm.respondToRequest_withResult_(request, CoreBluetooth.CBATTErrorSuccess)

    def peripheralManager_didReceiveWriteRequests_(self, pm, requests):
        for req in requests:
            uuid_str = str(req.characteristic().UUID()).lower()
            val = bytes(req.value()) if req.value() else b""
            log(">> phone write %s hex=%s" % (uuid_str, val.hex()))
            if main_loop and tx_queue is not None:
                target = CHAR_FFE1_UUID if "ffe1" in uuid_str else CHAR_FFE2_UUID
                main_loop.call_soon_threadsafe(tx_queue.put_nowait, (target, val))
        if requests:
            pm.respondToRequest_withResult_(requests[0], CoreBluetooth.CBATTErrorSuccess)

    def notify_phone(self, data):
        if self.pm and self.char_ffe1:
            ns_data = NSData.dataWithBytes_length_(data, len(data))
            AppHelper.callAfter(self._update_value, ns_data)

    def _update_value(self, ns_data):
        ok = self.pm.updateValue_forCharacteristic_onSubscribedCentrals_(
            ns_data, self.char_ffe1, None
        )
        try:
            hexstr = bytes(ns_data).hex()
        except Exception:
            hexstr = "<%d bytes>" % len(ns_data)
        log("<< notify phone hex=%s ok=%s" % (hexstr, ok))


async def central_task():
    global tx_queue, main_loop
    main_loop = asyncio.get_running_loop()
    tx_queue = asyncio.Queue()

    log("connecting to real device %s ..." % CFG["address"])
    while True:
        try:
            async with BleakClient(CFG["address"], timeout=20.0) as client:
                real_connected.set()
                log("connected to real device - it should stop advertising")

                def on_notify(_sender, data):
                    b = bytes(data)
                    log("<< real notify hex=%s" % b.hex())
                    if periph_delegate:
                        periph_delegate.notify_phone(b)

                await client.start_notify(CHAR_FFE1_UUID, on_notify)
                log("subscribed to real FFE1")

                while client.is_connected:
                    try:
                        target, payload = await asyncio.wait_for(tx_queue.get(), timeout=1.0)
                        log(">> forward to real %s hex=%s" % (target, payload.hex()))
                        await client.write_gatt_char(target, payload, response=False)
                    except asyncio.TimeoutError:
                        pass
        except Exception as e:
            log("real device error: %s (retrying in 2s)" % e)
        finally:
            real_connected.clear()
            if periph_delegate and periph_delegate.advertised:
                AppHelper.callAfter(periph_delegate.pm.stopAdvertising)
                periph_delegate.advertised = False
                log("advertising stopped")
        await asyncio.sleep(2.0)


def run_asyncio():
    asyncio.run(central_task())


def main():
    global periph_delegate

    ap = argparse.ArgumentParser(description="macOS BLE relay for stair light controllers")
    ap.add_argument("--name", default="downstair", help="advertised BLE name to impersonate")
    ap.add_argument("--address", required=True, help="real device UUID/MAC as seen by Bleak")
    ap.add_argument("--log", default="ble-relay.log")
    ap.add_argument("--advertise-delay", type=float, default=4.0,
                    help="seconds to wait before advertising (default 4)")
    args = ap.parse_args()

    CFG.update(name=args.name, address=args.address, log=args.log,
               advertise_delay=args.advertise_delay)

    open(CFG["log"], "w").close()
    log("starting macOS BLE relay: impersonating '%s', real=%s" % (CFG["name"], CFG["address"]))

    t = threading.Thread(target=run_asyncio, daemon=True)
    t.start()

    periph_delegate = PeripheralDelegate.alloc().init()
    periph_delegate.pm = CoreBluetooth.CBPeripheralManager.alloc().initWithDelegate_queue_(
        periph_delegate, None
    )

    try:
        AppHelper.runConsoleEventLoop()
    except KeyboardInterrupt:
        log("stopped")


if __name__ == "__main__":
    main()
