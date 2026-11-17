# Passport OS MY 0.2.0-my.1

First Malaysia edition, based on upstream Passport OS 0.2.0-rc.59.

## Changes from upstream

- English-only interface, voice tools and setup pages. Latin-only fonts; the app image is about 500 KB smaller.
- Malaysian city catalogue from GeoNames (530 places in 16 states and Federal Territories). Kuala Lumpur is the default.
- Radio searches Malaysian stations (city, then state, then country) and accepts MP3 and AAC/AAC+. Presets: Traxx FM, BFM 89.9, Nasional FM, Klasik FM, ai FM, 988 FM.
- Tech News from Hacker News replaces AI Hot (Xiaozhi tools `self.news.tech_hot` and `self.news.tech_latest`).
- NetEase Cloud Music removed; asking Xiaozhi for music plays live radio. 12 apps.
- Connectivity fallback probe is `cp.cloudflare.com/generate_204`.
- Xiaozhi keeps the official xiaozhi.me backend. Set the agent's language and voice to English on xiaozhi.me.

## Install

Write `-full.bin` at `0x0` for a fresh install, or `-app.bin` at `0x10000` to upgrade a device with the same partition layout. See [the install guide](install.md).

## Known limits

- Not yet checked on a real device: English layout on the physical screen, AAC playback with Wi-Fi and TLS active, the setup pages on a real phone, and Xiaozhi with an English agent from Malaysia.
- Upgrading from upstream keeps an unused `aihot/actor` NVS key.
- Power and battery life have not been measured.
