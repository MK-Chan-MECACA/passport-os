# Offline city directory (Malaysia)

Source: [GeoNames](https://www.geonames.org/) `cities5000.zip` and
`admin1CodesASCII.txt` from <https://download.geonames.org/export/dump/>,
retrieved 2026-11-17. Data © GeoNames, licensed under
[Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/).

Changes: `cities5000_MY.txt` keeps only rows whose country code is `MY`
(`awk -F'\t' '$9=="MY"' cities5000.txt`); `admin1_MY.txt` keeps only the
`MY.*` lines. Rows are otherwise unchanged.

This is a weather-location directory (places with population ≥ 5,000), not an
administrative-division register. Each place's parent is its state or federal
territory. The phone page offers custom names and decimal coordinates for
places not listed. It does not claim street-level positioning accuracy.

Regenerate with `python3 tools/generate_city_catalog.py`; use `--check` to verify
the checked-in JSON, deterministic gzip, city glyph list and radio place table.
The device serves the gzip bytes from flash. Only the phone decompresses it.
