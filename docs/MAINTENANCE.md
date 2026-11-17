# Maintenance

The upstream author's version of this guide (Windows/WSL paths, rc.57–rc.58 handover notes) is kept in Chinese at [archive/zh/MAINTENANCE-upstream.md](archive/zh/MAINTENANCE-upstream.md).

## Small changes

Find the module from `AGENTS.md` and [CURRENT.md](CURRENT.md), change the source and run the matching tests. Run the full host regression before handing over firmware. Update only what changed in CURRENT.md, with its verification result; do not keep parallel copies of history.

## Environment

- The working copy must live at a path **without spaces**. The `esp_audio_codec` component passes an unquoted `-L` path, so the link fails otherwise. A symlink does not help because CMake resolves it. On this machine the repo is `~/passport-os`; `~/Claude Code/Passport OS MY` is only a symlink to it, so build from `~/passport-os`.
- ESP-IDF 5.5.3 (CI uses 5.5.3). `build.sh` loads it from `/opt/esp-idf` unless `IDF_DIR` is set.
- Host tests assume Linux GCC. On macOS run them in a container (`python:3.12-bookworm` plus `nodejs`, `cmake`, `ninja-build`) with ESP-IDF mounted at `/opt/esp-idf`:

```bash
bash tools/test-host.sh
```

Host tests need Python 3, a C/C++ compiler, Node.js and the usual tools the scripts call. The first firmware build fetches `managed_components/` according to `dependencies.lock`.

## Builds

```bash
bash build.sh            # local build in build/
bash build.sh release    # public release configuration in build-public/
```

`build.sh` regenerates the configuration from `sdkconfig.defaults` every time, so a stale `sdkconfig` cannot break the memory budget. Do not remove the memory configuration checks in CMake. `release` is a standalone public-configuration build, not a publication; `diag` is a diagnostic build and must not be shipped.

If the repository is moved, existing build directories keep the old path in `CMakeCache.txt`; run `idf.py fullclean` (or delete `build*/`) before building again.

Firmware hand-over checks (in a shell with ESP-IDF loaded):

```bash
idf.py -B build merge-bin -o FoloToy-AI-Passport-full.bin
python3 tools/verify_firmware.py build
```

`python3 tools/check_repo.py --firmware-fork` runs the repository checks. `bash tools/validate.sh --static` also runs actionlint and the full host tests; the first run needs network access to install missing tools. `bash tools/validate.sh --firmware` builds the public configuration in a temporary directory and copies the full and application images to `build/`.

## Releases

1. Set `PROJECT_VER` in `CMakeLists.txt` and write `docs/release-<version>.md`.
2. Build and verify the public configuration, then optionally run `tools/package_firmware.py` for a local candidate with a source snapshot and manifest.
3. Push a tag such as `v0.2.0-my.1`. The `Build firmware` workflow attaches `Passport-OS-MY-<tag>-full.bin`, `Passport-OS-MY-<tag>-app.bin` and `SHA256.txt` to a GitHub Release on `MK-Chan-MECACA/passport-os`.

Creating a tag or release publishes firmware; do it only when the user asks.

## Updating a device

Confirm the actual serial port and what the user wants changed first. If the user explicitly asks for a fresh install, take a full flash backup, then erase and write the full image. Every other upgrade keeps the device data: take a fresh full backup, confirm the partition table, write only the `0x10000` application partition, read it back and confirm the non-application regions are unchanged. Without permission for a full wipe, never erase the whole chip or write a merged image over an existing device's data. Identify every image by SHA256.
