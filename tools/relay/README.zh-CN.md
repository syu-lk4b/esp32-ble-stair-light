# 蓝牙中继工具

这里是从零逆向厂商协议时用的工具，完整过程见
[docs/REVERSE-ENGINEERING.zh-CN.md](../../docs/REVERSE-ENGINEERING.zh-CN.md)。

原理：让手机 App 连到一台"假设备"，把它的每一个字节记录下来并转发给真机。

> 请只对自己拥有的设备使用。

## 两种实现

| | A — macOS 代理 | B — ESP32 串口中继 |
|---|---|---|
| 假外设跑在 | Mac（CoreBluetooth） | ESP32 |
| 真机链路跑在 | Mac（Bleak） | Mac（Bleak） |
| 稳定性 | 一般 | 稳定 |
| 额外硬件 | 不需要 | 任意 ESP32 |
| 推荐 | 否 | **是** |

两种方案都依赖同一个前提：真机必须停止广播。连上它之后，多数 BLE 模块会自动停止广播。

## 依赖

```bash
python3 -m pip install -r requirements.txt
```

`pyobjc-framework-CoreBluetooth` 只在 macOS 上需要，且只有方案 A 用到。

## 方案 B — ESP32 串口中继（推荐）

### 1. 烧录中继固件

```bash
cd esp32-relay
pio run -t upload
```

默认广播名是 `downstair`。要冒充另一台：

```bash
pio run -t upload -DRELAY_NAME=\"upstair\"
```

### 2. 找到真机地址

```bash
python3 -m bleak.discover
```

macOS 上是一个 CoreBluetooth UUID（不是 MAC）；Linux 上是 MAC。

### 3. 启动中继

```bash
python3 ble_serial_relay.py --address <real-uuid>
```

脚本会先连上真机（真机随即停止广播），然后等待 ESP32 的数据。

### 4. 打开手机 App

App 会发现并连接到假设备。日志里能看到每一帧：

```
[20:42:46] [ESP32] TX_FFE1:aaafba03142a2e82
[20:42:46] >> forward to real 0000ffe1-... hex=aaafba03142a2e82
[20:42:47] << real notify hex=5550ba100001010d050101585e0101001200172d93
[20:42:47] << RX to ESP32: 5550ba100001010d050101585e0101001200172d93
```

## 方案 A — macOS 代理

```bash
python3 ble_proxy_macos.py --name downstair --address <real-uuid>
```

等到日志出现：

```
advertising as 'downstair' - open the phone app now
```

再打开 App。

这个方案保留下来主要是为了完整性：在同一个进程里同时跑 CoreBluetooth 外设和
Bleak central，偶尔会抢射频；再加上 iOS 对 GATT 缓存很激进。方案 B 就是为了
绕开这些坑。

## 日志格式

| 前缀 | 含义 |
|---|---|
| `TX_FFE1:<hex>` | 手机往 FFE1 写了这些字节 |
| `TX_FFE2:<hex>` | 手机往 FFE2（AT 通道）写了这些字节 |
| `RX:<hex>` | 用这些字节 notify 手机 |
| `EVENT:PHONE_CONNECTED` | 手机连上了中继 |
| `EVENT:ADVERTISING` | 中继已可被发现 |

## 重放一帧

抓到帧之后，不需要中继也能直接发：

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

## 注意事项

- 启动前先关掉厂商 App：控制器同一时间只接受一个 central。
- 必须先连上真机（让它安静下来），再开始广播代理。
- iOS 会缓存 GATT；改过代理特征布局后，开关一次蓝牙。
- 校验和错误的帧会被静默丢弃，没有任何报错。
- 有些命令会回一个短 ACK，有些不会，不要死等回应。

## License

Apache-2.0，与仓库其余部分一致。
