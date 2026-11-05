# BLE MIDI Adapter

100% ai slop Bluetooth LE midi to usb midi adapter for ESP32. Use at your own risk

ESP32-S3 firmware that connects to a BLE MIDI device as the central and passes
its MIDI to any USB host as a class-compliant USB MIDI device. No PC or drivers
needed once it's flashed.

```
BLE MIDI device ──BLE──▶ ESP32-S3 (NimBLE central) ──USB MIDI device──▶ USB host
```

## Builds

| Env | Board | USB | Screen |
|---|---|---|---|
| `esp32s3` | Plain ESP32-S3 boards | MIDI only | None |
| `esp32s3-debug` | Plain ESP32-S3 boards | MIDI and serial console, logs raw BLE | None |
| `m5cores3` | M5Stack CoreS3 | MIDI and serial console | Touch screen |

## M5Stack CoreS3 screen

- **Power button:** the screen starts off, and a short press turns it on or off.
  MIDI keeps running either way.
- **Swipe left or right:** switch between the two pages.
- **MIDI page:** the latest messages, newest at the bottom.
- **Devices page:** BLE MIDI devices in range. Tap one to connect to it and make
  it the saved device. Tap the current one to disconnect and forget it; the
  adapter then stays unconnected until you pick another. It shows up to five
  devices.

The adapter only scans while connected if the Devices page is open, in short
windows.

## 1. Verify the board first

The "ESP32-S3 Mini" listing is unverified. Plug the board into the PC and run:

```sh
esptool chip_id        # must report ESP32-S3; a C3 goes back
esptool flash_id       # flash size (platformio.ini assumes 4 MB)
```

The USB-C socket has to be wired to GPIO19/20 (native USB). If `lsusb` shows
`303a:1001 Espressif USB JTAG/serial debug unit`, it is.

## 2. Build / test / flash

```sh
pio test -e native          # BLE MIDI parser unit tests (host)
pio run -e esp32s3          # build firmware
pio run -e esp32s3 -t upload
```

The firmware hands native USB to TinyUSB (MIDI only, no CDC). After the first
flash the board shows up as **BLE MIDI Adapter** (USB MIDI), not as a serial port.
To reflash, hold **BOOT** while plugging in to get into the ROM bootloader.
Logs go to UART0 (TX = GPIO43, 115200 baud). Use a USB-serial adapter on that
pin if you need them.

Check on the PC before plugging into another USB host: `aconnect -l` / `amidi -l`
should list `BLE MIDI Adapter`, and `aseqdump` should show what the device plays.

## 3. Behaviour

- **First pairing:** with no stored peer, it connects to the first device
  advertising the BLE MIDI service. To restrict this, build with
  `-DPEER_NAME_PREFIX=\"Name\"`.
- **After that:** the peer's address is saved in NVS, and on power-up it only
  reconnects to that device. Pairing only happens if the device asks for it.
- **Forget peer:** hold BOOT for 3 s while running.
- **On disconnect:** it sends All Notes Off on all 16 channels so nothing hangs
  on the USB host.
- **Latency:** it requests a 7.5–15 ms connection interval. BLE timestamps
  are dropped and messages are forwarded as they arrive.

## Open questions

- USB host port current limits on whatever the adapter plugs into.
- Devices with a changing (resolvable private) address may connect once and
  then not be found again.
- Whether a given host accepts the device as-is. If not, try a different
  VID/PID or product string.
