# Reverse-engineered BLE protocol

This is the vendor protocol used by the cheap "stair step light" controllers
that ship with a phone app and an RF remote. It was recovered by sniffing the
app's GATT traffic. It is **not** documented by the vendor and may differ
between batches.

## GATT

| | |
|---|---|
| Service | `0000FFE0-0000-1000-8000-00805F9B34FB` |
| Characteristic | `0000FFE1-0000-1000-8000-00805F9B34FB` |
| Properties | write, notify |
| Pairing | none (no bonding, no encryption) |

The controllers advertise as `downstair` and `upstair` by default. The panel
acts as a BLE **central** and connects to both at the same time.

A second characteristic (`FFE2`) shows up on some units and looks like an AT
command channel. It is not needed for normal control.

## Frame format

Every command written to `FFE1`:

```
AA AF CMD LEN P0 P1 ... CHK
```

| Byte | Meaning |
|---|---|
| `AA AF` | magic |
| `CMD` | command id |
| `LEN` | payload length |
| `P0..` | payload |
| `CHK` | sum of every preceding byte, modulo 256 |

```c
uint8_t chk = 0;
for (int i = 0; i < 4 + len; i++) chk += buf[i];
buf[4 + len] = chk;
```

## Commands

| CMD | Payload | Meaning |
|---|---|---|
| `B5` | 1 byte, `0..5` | mode |
| `B9` | 1 byte, `0..100` | step brightness |
| `B8` | 1 byte, `0..100` | flow brightness |
| `BE` | 4 bytes: on HH, on MM, off HH, off MM | built-in timer schedule |
| `BA` | 3 bytes: HH, MM, SS | time sync; also triggers a config reply |

### Modes

| Value | Vendor label | Behaviour |
|---|---|---|
| 0 | 全亮模式 | all steps on |
| 1 | 交替模式 | alternating steps |
| 2 | 流水模式 | chasing / flowing |
| 3 | 逐步模式 | one step at a time |
| 4 | 跟随模式 | follow the walker |
| 5 | 随机模式 | random |

## Reply

After a `BA` query the controller notifies:

```
55 50 BA 10 <16 bytes>
```

Observed field layout (empirical, stable on the tested units):

| Offset | Meaning |
|---|---|
| `[11]` | flow brightness (`0..100`) |
| `[12]` | step brightness (`0..100`) |
| `[15]` | mode (`0..5`) |

The panel sends `BA` right after connecting, so the UI and Home Assistant
immediately reflect the controller's real state instead of guessing.

## Example: set mode 2 on the downstairs controller

```
AA AF B5 01 02 B7
```

`AA + AF + B5 + 01 + 02 = 0x02B7` → `CHK = 0xB7`.

## Caveats

- Some units accept commands only while the vendor app is disconnected.
- The controllers are not designed for multiple centrals; the panel keeps one
  connection per device and reconnects when the link drops.
- Range is ordinary BLE (roughly 10 m through walls). The panel should be
  mounted within range of both controllers.
