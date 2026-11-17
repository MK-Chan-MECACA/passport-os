# Passport OS MY — Malaysia edition plan

Fork of `manchunx7-bit/passport-os` (0.2.0-rc.59), localized for English-speaking
users in Malaysia. Decisions below were agreed on 2026-11-17 and are the scope of
this edition. Anything not listed here stays as upstream.

## Decisions

| # | Area | Decision |
|---|---|---|
| 1 | Language | English-only UI. Drop CJK glyphs; regenerate fonts as Latin-only. |
| 2 | City catalog | GeoNames `cities5000`, filtered to country `MY`. Parent = state / Federal Territory. Default city Kuala Lumpur. Keep CC-BY 4.0 attribution in `third_party/`. Phone page keeps the custom name + coordinates fallback. |
| 3 | Radio | Radio Browser with `countrycode=MY`, no state filter. Accept MP3 and AAC/AAC+. Replace the six qingting.fm presets with Malaysian streams, each URL live-checked before shipping. |
| 3a | HLS | Not enabled. Revisit only after AAC is proven stable on real hardware. |
| 4 | NetEase Music | Removed completely (app, tests, launcher/registry/MCP hooks). "Play music" voice requests route to Radio. 12 apps. |
| 5 | AI Hot → Tech News | Replace `aihot.news` with the HN Algolia API (one HTTPS request per fetch). `Hot` = `search?tags=front_page`; `Latest` = `search_by_date?query=AI&tags=story&numericFilters=points>50`; `Daily` dropped. Titles + points only. |
| 6 | Xiaozhi | Keep the official xiaozhi.me backend. Translate MCP tool names/descriptions, voice intents and on-screen activation text to English. README tells users to set the agent language and voice to English on xiaozhi.me. Keep the `ota_url` override. Name stays "Xiaozhi". |
| 7 | Setup and network | Translate `phone_setup.html`, `wifi_configuration.html`, `web/phone-location.html`. Fallback connectivity probe `qualcomm.cn` → `cp.cloudflare.com/generate_204`. Keep IP-geo services and the `Passport-WiFi-xxxx` SSID. |
| 8 | Branding | Name "Passport OS MY", version `0.2.0-my.1`. Releases via GitHub Releases on `MK-Chan-MECACA/passport-os` (`-full.bin` + `-app.bin`). Keep the FoloToy MIT notice; add a Malaysia-edition credit line. |
| 9 | Docs | Translate living docs: `README.md` (English becomes primary; merge `docs/README.en.md`), `AGENTS.md`, `docs/CURRENT.md`, `docs/MAINTENANCE.md`, `docs/xiaozhi-app.md`, install guide → `docs/install.md`. Move historical reviews, plans, audit, release notes and `voice-udp-channel.md` to `docs/archive/zh/` unchanged. Delete `README.zh_CN.md`. |
| 10 | Code comments | Translate all hand-written comments in a separate final commit with no code changes. Firmware build output must be identical before and after. Generated font files excluded. |

Unchanged on purpose: timezone default (UTC+8 = MYT) and weather (Open-Meteo, global).

## Order of work

One commit per step on branch `my-edition`. Run the relevant host tests after each step.

1. Remove NetEase.
2. Remove the `aihot.news` client (Tech News added in step 5).
3. Malaysian city catalog (GeoNames generator, `cities.json.gz`, radio state map).
4. Radio: `countrycode=MY`, MP3 + AAC/AAC+, Malaysian presets.
5. Tech News on HN Algolia.
6. English UI strings and Latin-only fonts.
7. Setup pages and connectivity fallback.
8. Branding, version, docs.
9. Comment translation.

## Risks and acceptance

- **AAC with no PSRAM.** The decoder needs about 23 KiB; upstream logs show out-of-memory retries. This needs a real-device test with Wi-Fi + TLS active. A host test is not acceptance.
- **Xiaozhi latency.** The backend is in China; expect higher voice round-trip time from Malaysia.
- **Build path.** ESP-IDF rejects project paths containing spaces. Build from a path without spaces (see `docs/MAINTENANCE.md` once translated).
- Serial-simulated buttons are not physical-button or real-microphone acceptance.
