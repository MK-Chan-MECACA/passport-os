#!/usr/bin/env bash
# Build passport-os (Linux, WSL or macOS).
#
# Usage:
#   bash build.sh          build the local official Jollybot edition
#   bash build.sh clean    clear the CMake cache first, then full rebuild (only needed when the IDF version or path changed)
#   bash build.sh release  standalone build of the official Jollybot edition with the website release config, then verify
#   bash build.sh diag     memory diagnostic build (heap task tracking + allocation peak), output to build-diag, not for release
#
# Note: ESP-IDF is installed in WSL at /opt/esp-idf; there is no toolchain on the Windows side, so builds must run in WSL.
# Flashing goes through Windows COM3, which WSL cannot see by default: bind it with usbipd, or use esptool directly on Windows.
set -u

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IDF_DIR="${IDF_DIR:-${IDF_PATH:-/opt/esp-idf}}"

cd "$PROJECT_DIR" || { echo "Project directory not found: $PROJECT_DIR"; exit 1; }

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ESP-IDF not found: $IDF_DIR/export.sh"
    echo "If it is elsewhere, run: IDF_DIR=/path/to/esp-idf bash build.sh"
    exit 1
fi

echo "==> Loading ESP-IDF environment ($IDF_DIR)"
set +e
. "$IDF_DIR/export.sh" >/dev/null 2>&1
rc=$?
set -e
if [ $rc -ne 0 ]; then
    echo "export.sh failed; check the ESP-IDF installation"
    exit 1
fi

if [ "${1:-}" = "release" ]; then
    # Seed from tracked defaults every time. Local sdkconfig may silently retain
    # obsolete TLS/flash options even after sdkconfig.defaults has been updated.
    mkdir -p build-public
    cp -- sdkconfig.defaults build-public/sdkconfig
    idf.py -B build-public -DSDKCONFIG="$PROJECT_DIR/build-public/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=ON \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build \
        merge-bin -o FoloToy-AI-Passport-full.bin
    python tools/verify_firmware.py build-public --public
    exit 0
fi

if [ "${1:-}" = "diag" ]; then
    mkdir -p build-diag
    cat -- sdkconfig.defaults sdkconfig.diag > build-diag/sdkconfig
    idf.py -B build-diag -DSDKCONFIG="$PROJECT_DIR/build-diag/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
    exit 0
fi

if [ "${1:-}" = "clean" ]; then
    echo "==> Cleaning build directory"
    rm -rf build
fi

echo "==> Building (local official Jollybot edition)"
# Never reuse the ignored root sdkconfig: it may preserve obsolete options even
# after defaults change. Keep the expanded generated config inside the build
# directory and seed it from the tracked memory budget on every invocation.
mkdir -p build
cp -- sdkconfig.defaults build/sdkconfig
idf.py -B build -DSDKCONFIG="$PROJECT_DIR/build/sdkconfig" \
    -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
    -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
