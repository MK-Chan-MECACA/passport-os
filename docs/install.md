# Passport OS MY install guide

This guide is for people installing a prebuilt image. You do not need AI tools: download the firmware, flash it to matching hardware, then finish setup from your phone. See the [project home page](../README.md) for the app list and development notes.

## Supported device

- FoloToy AI Passport
- ESP32-C3 with 8 MB Flash
- A hardware revision that matches the release's `partitions.csv`, screen, audio codec and button circuit

Devices that look similar but differ in flash size, screen, pins, audio chip or battery circuit cannot be flashed directly.

## Download

Download all files from the same [GitHub Release](https://github.com/MK-Chan-MECACA/passport-os-my/releases):

- `Passport-OS-MY-vX.Y.Z-full.bin`: first installation or a complete reset, written at `0x0`.
- `Passport-OS-MY-vX.Y.Z-app.bin`: normal upgrade of a device that already runs Passport OS with the same partition table, written at `0x10000`.
- `SHA256.txt`: checks that the downloads are complete and unmodified.

Do not mix full images, application images and partition tables from different versions. A full image overwrites the device configuration. To keep Wi-Fi, portraits, cards or game records, use the application image after confirming the partition table matches.

## Flash on Windows

1. Install Python 3.11 or newer.
2. Open PowerShell and install esptool:

```powershell
py -m pip install esptool
```

3. Connect the device over USB and find its serial port in Device Manager, for example `COM3`.
4. Optional: check the downloaded full image against `SHA256.txt`:

```powershell
Get-FileHash .\Passport-OS-MY-vX.Y.Z-full.bin -Algorithm SHA256
```

5. For a first installation or to clear all data:

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 erase-flash
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  --flash-mode dio --flash-size 8MB --flash-freq 80m `
  0x0 .\Passport-OS-MY-vX.Y.Z-full.bin
```

6. For a normal upgrade, write only the application partition:

```powershell
py -m esptool --chip esp32c3 --port COM3 --baud 460800 write-flash `
  0x10000 .\Passport-OS-MY-vX.Y.Z-app.bin
```

## Flash on macOS or Linux

Install esptool with `python3 -m pip install esptool`. The serial port is usually `/dev/cu.usbmodem*` on macOS or `/dev/ttyACM0` on Linux. Check the download with `shasum -a 256 -c SHA256.txt` (macOS) or `sha256sum -c SHA256.txt` (Linux), then run the same commands with the port changed:

```bash
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 --baud 460800 erase-flash
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 --baud 460800 write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 Passport-OS-MY-vX.Y.Z-full.bin
```

Do not unplug USB while flashing. The device resets itself when writing finishes. If the serial port shows nothing for a while, unplug and reconnect USB and watch the device screen.

## First start

On first start the device opens the phone setup screen and a password-free hotspot such as `Passport-WiFi-D7A9`:

1. Connect your phone to the hotspot.
2. Open `http://192.168.4.1` in the browser.
3. Save your home Wi-Fi, city and profile.
4. Add a portrait or cards if you want them; anything you do not need can be skipped.
5. After saving, return to Home. The device connects through your own Wi-Fi.

## Features that use outside services

- Home weather and air quality need a network connection and city coordinates (Open-Meteo).
- Radio uses public Malaysian station streams from the Radio Browser directory. Stations can stop working when broadcasters change their streams.
- Xiaozhi needs a network connection and a device binding on [xiaozhi.me](https://xiaozhi.me). The first time you open Xiaozhi it shows a binding code; add the device on xiaozhi.me and it connects within 30 seconds without a restart. **Set the agent's language and voice to English on xiaozhi.me.** The servers are in China, so replies take longer from Malaysia.
- Tech News (asked through Xiaozhi) reads Hacker News.
- AI Voice needs its Windows companion bridge running on a computer.
- Muse needs phone pairing. When using a proxy, enter the proxy host's LAN IP and HTTP port; the port depends on your proxy tool.
- Why Barking, Tetris, Sound Jump, Stopwatch, Pomodoro, Reminders and Lists work offline.

## Build from source

You need ESP-IDF 5.5.3, Python 3, Git, and Linux, WSL or another environment that runs ESP-IDF (macOS works too):

```bash
git clone https://github.com/MK-Chan-MECACA/passport-os-my.git
cd passport-os-my
bash build.sh release
```

`bash build.sh release` loads ESP-IDF from `/opt/esp-idf`. If ESP-IDF is installed elsewhere, use `IDF_DIR=/path/to/esp-idf bash build.sh release`.

The full image is written to `build-public/FoloToy-AI-Passport-full.bin` and the application image to `build-public/passport-os.bin`. ESP-IDF components are fetched according to `dependencies.lock`. Do not commit your local `managed_components/` or generated `sdkconfig`.

With ESP-IDF 5.5.3 activated, verify:

```bash
bash tools/test-host.sh
./tools/validate.sh --static
./tools/validate.sh --firmware
```

## Common problems

**Cannot join the hotspot.** Make sure the device is showing the phone setup screen; unplug and reconnect USB if needed. The hotspot is open and has no password.

**Old profile still shows after flashing.** Only the application partition was written, or the erase did not cover everything. For a fresh start, run `erase-flash` and then write the full image.

**Xiaozhi or Muse will not connect.** First check that home Wi-Fi works, then check the Xiaozhi binding, the service account, the phone pairing and the proxy host and port.

**Xiaozhi answers in Chinese.** Change the agent's language and voice to English on xiaozhi.me.

**You want to keep your own changes.** Edit the source, run the host tests and firmware checks, then build your own application or full image.

## Licence and contributing

Passport OS uses the MIT licence. Muse's Noise protocol code and adapter include Apache-2.0 upstream content; the `LICENSE`, `NOTICE.md` and third-party notes in each component directory apply. Never add Wi-Fi passwords, API tokens, device logs, personal portraits or local configuration exports to a contribution.

Issues, hardware compatibility reports and improvements are welcome. Include the firmware version, hardware revision, how you flashed it and whether device data was kept.
