#!/usr/bin/env bash
# Regenerate the Latin-only UI fonts from the Source Han Sans copy shipped with LVGL.
# Run from anywhere after `idf.py reconfigure` has populated managed_components/.
set -euo pipefail
cd "$(dirname "$0")/.."
OTF=managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
# ASCII, Latin-1 (accents, ° · ± µ), Greek mu, dashes, curly quotes, bullet,
# ellipsis, arrows and euro. Dynamic text (stations, headlines, replies) can use any of these.
RANGES=(--range 0x20-0x7E --range 0xA0-0xFF --range 0x3BC --range 0x2013-0x2014
        --range 0x2018-0x2019 --range 0x201C-0x201D --range 0x2022 --range 0x2026
        --range 0x2190-0x2193 --range 0x20AC)
gen() {  # name size
    npx --yes lv_font_conv@1.5.3 --font "$OTF" "${RANGES[@]}" --size "$2" --bpp 4 \
        --format lvgl --no-compress --no-prefilter --lv-include lvgl.h \
        --lv-font-name "$1" -o "main/fonts/$1.c"
    # Keep the header free of the build machine's paths.
    sed -i.bak "s#--font [^ ]*#--font $OTF#" "main/fonts/$1.c" && rm "main/fonts/$1.c.bak"
}
gen buddy_font_16 16
gen ui_font_20 20
