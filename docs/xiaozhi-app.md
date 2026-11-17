# Xiaozhi AI conversation app (`main/apps/xiaozhi`): rework notes

These notes record how the conversation feature from FoloToy's `folo-ai-passport-xiaozhi` was merged into Passport OS, and how two problems were tracked down and fixed: "the service does not work (no reply to a conversation)" and "unstable or crashing after a conversation".

Hardware: ESP32-C3 (400 KB SRAM / 8 MB Flash). About **94 KB** of heap is free after the app starts.
Desktop log baseline: `Initial Free Heap: 95 KB`; `94 KB` on entering the Xiaozhi app.

Malaysia edition: the app keeps the official xiaozhi.me backend. Tool names, descriptions, voice intents and on-screen text are in English. Users should set their agent's language and voice to English on xiaozhi.me.

---

## 1. Summary

| # | Symptom | Root cause | Fix |
| --- | --- | --- | --- |
| 1 | Cannot reach the Xiaozhi backend / no reply | The official backend's **OTA configuration** step was skipped entirely. The client hard-coded `wss://api.tenclass.net/xiaozhi/v1/` with a literal `test-token`, so it never received the server's `websocket{url,token,version}` | Added `xiaozhi_ota.cc`: `POST /xiaozhi/ota/` as the official flow does, parse `websocket` / `activation`, and store the result in NVS |
| 2 | No reply | `listen`/`abort` messages **did not carry `session_id`**, so the server could not attach the turn to a session | Parse `hello.session_id` from the server and echo it in every outgoing message |
| 3 | No reply | The handshake lacked the `Protocol-Version` header; `Client-Id` used the MAC address (the official client requires a persistent UUIDv4) | Add `Protocol-Version`; generate a UUIDv4 on first use and store it in NVS |
| 4 | No reply (intermittent) | Client **DTX silence suppression**: when it judged the input silent it sent no packets at all, and the server, receiving no audio, never answered | `enable_dtx = false` (in push-to-talk use DTX only does harm) |
| 5 | Reply never shown on screen | Reply text was bound to the `llm` message, but the official `llm` carries only `emotion`; the text arrives in **`tts/sentence_start`** | Bind text to `tts/sentence_start` (still accepting `llm.text` from some servers) |
| 6 | **Crash (always reproducible)** | "Press OK to interrupt while the AI is speaking": the websocket task ran `StartPlayback()` while the input task ran `StopPlayback()`, both touching the same codec handle and buffers without a lock → use-after-free → panic reboot | Only the control task may drive the audio lifecycle; websocket callbacks only post events and write to the ring buffer |
| 7 | Crash (stack overflow) | Opus keeps its scratch buffers on the stack: **about 19 KB to encode, over 6 KB to decode**. The original 24 KB recording task stack was enough, but once split across several tasks none of them had enough | Recording and playback share **one resident worker thread** (20 KB stack); half duplex makes them mutually exclusive by design |
| 8 | Crash (stack overflow) | OTA's mbedTLS handshake plus NVS/SPI-flash reads form a deep call chain that overflowed the 6 KB control task stack | OTA moved to a temporary `xzg_ota` task (12 KB stack) deleted after use |
| 9 | Hang | `StopRecording()` spun without limit waiting for the task to exit. Once the recording task was stuck in an I2S read with `portMAX_DELAY`, the `portMAX_DELAY` wait inside `esp_websocket_client_stop()` never returned either | Every task wait now has a **bounded timeout**; a timeout only logs and carries on with cleanup |
| 10 | App stuck on "Thinking", OK does nothing | The state machine had no timeout: if the server never sent `tts`, it stayed in Thinking forever | Added thinking and reply timeouts; on timeout it aborts and returns to Idle |

---

## 2. Key measurements (real device, COM3 serial log)

Numbers worth keeping from the fixes, so later changes do not fall back into the same traps:

| Item | Measured | Notes |
| --- | --- | --- |
| Opus **encode** stack | ~19 KB | `clt_mdct_forward_c` / `silk_NSQ_c`; an 8 KB stack overflowed by 6040 bytes |
| Opus **decode** stack | >6 KB | `quant_all_bands`; a 4 KB stack overflowed by 1320 bytes |
| Opus encoder **heap** | ~23 KB | Opened and closed each turn to return it to the heap |
| Server sample rate | **24000 Hz** | The decoder and codec must open at 24 kHz, otherwise pitch and speed are wrong |
| Server echo of `frame_duration` | Follows the client | Whatever the client reports, the server's `hello` returns |
| Free heap while speaking | ~14 KB | Encoder and TLS both live; the tightest moment |
| Free heap while replying | ~20 KB | Encoder released, decoder live |

**Frame length.** The client reports **60 ms** frames and always encodes **16 kHz** mono. 20 ms frames and SILK (VOIP) encoding were tried; the server did not answer at all (reverted). Frame length has almost no effect on encoder stack use (CELT scratch size barely depends on it).

**Known harmless log lines:**

- `tts state=sentence_end (ignored)`: newer servers send this to mark the end of one sentence. It does not affect the audio stream or state machine; ignoring it is correct.
- `Playback ring full (...), frame dropped`: the server sent a burst larger than the buffer headroom. It should not appear on the normal path; if it is frequent, increase `kPlayRingCap` (at the cost of the same amount of heap).
- `WebSocket error, type=0/1` followed by `letting it auto-reconnect`: network jitter. Verified on the device: disconnect → recording cleans up automatically → client reconnects → new handshake → conversation continues, with no reboot and no user action.

---

## 3. Architecture: single-owner control task

```text
button task (3072 stack) ──post──┐
                                 ├─► xzg_ctl (control task, 6 KB stack)
websocket task (5 KB stack) ──┐  │     * only caller of Protocol::Start/Stop/Trigger*
   └ callbacks do two things: │  │     * only caller of AudioService Start*/Stop*
       · write events to queue ┘ │     * only caller of ui_*
       · push Opus to ring buffer┘     * sender of recorded frames (drain_audio_tx)
                                           │
xzg_ota (temporary, 12 KB stack) ── OTA HTTPS handshake, deleted after use
xzg_audio (resident, 20 KB stack) ── recording encode / playback decode (half duplex, one shared stack)
```

Key points:

- **Never do blocking work in a websocket callback** (no decoding, no I2S writes, no waiting on locks). The old code called `PlayAudioFrame()` (decode plus blocking I2S write) directly in the callback, making the network task do the audio work.
- **`Protocol` is a function-local static that is never destroyed**, which removes "callback touches a destroyed object" at the root.
- **`xzg_audio` is created when the app starts.** At that point the heap is fresh and contiguous (largest block 50 KB+), so a 20 KB contiguous stack is always available. Requesting it when the user starts talking fails, because TLS/OTA have fragmented the heap (largest block only 7 KB).
- **Switching away calls `Deinit()`, which returns the resident thread and every buffer to the heap.** After leaving Xiaozhi, free heap returns to 80 KB, so Radio and other apps are not starved.
- **The Opus encoder stays resident (memory fix, 2026-09-14).** The encoder needs a ~25 KB contiguous block (measured: open fails with ret:-7 at largest=22528/23552), while late in a session the largest block is only 4–9 KB, so opening it on demand in a fragmented heap always fails. `Init()` now calls `EnsureEncoder()` once and `StopRecording()` no longer closes it. If the decoder cannot open during playback, the encoder is released temporarily (`enc_dropped_for_playback_`) and restored right after `StopPlayback()/AbortPlayback()`. If decoder shutdown is not confirmed idle, the handle is kept (the old code cleared it, permanently leaking ~13 KB).
- **Matching global memory changes** (without them the resident encoder starves the TLS handshake: `Dynamic Impl: alloc(3778) failed`): Home's `s_portrait_a8` (14.4 KB A8 portrait buffer) and the boot chime's `s_note` (3.2 KB) moved from .bss to allocate-on-use and free-after-use; free heap during a session rose from ~30 KB to ~48 KB. Verified on the device: three conversation rounds with recording and playback working, and after exit free=99.5 KB / largest=51.2 KB with no leak (`_diag/xz_memfix4.log`).

---

## 4. Files

| File | Responsibility |
| --- | --- |
| `app_xiaozhi.cc` | Single-owner control task, state machine, timeouts, button events, sending recorded frames |
| `xiaozhi_protocol.{h,cc}` | WebSocket v3: request headers, `hello` handshake, `session_id`, fragment reassembly, message parsing |
| `xiaozhi_audio.{h,cc}` | ES8311 half-duplex audio: resident worker thread, ring buffer, bounded waits, memory gate |
| `xiaozhi_ota.{h,cc}` | Official backend OTA/activation: obtain `websocket{url,token,version}`, show the activation code |
| `xiaozhi_store.{h,cc}` | NVS configuration (namespace `xiaozhi`, compatible with the official `websocket` namespace) |
| `xiaozhi_ui.{h,cc}` | LVGL screen; every entry point re-checks widget pointers inside the lock |

---

## 5. Configuration

NVS namespace `xiaozhi` (write it with the `nvs_flash` tools or a future serial command):

| Key | Default | Notes |
| --- | --- | --- |
| `ws_url` | empty | For a self-hosted server, e.g. `ws://192.168.x.x:8000/xiaozhi/v1/` |
| `token` | empty | Usually empty for a self-hosted server; written automatically by OTA for the official backend |
| `version` | 3 | Protocol version |
| `ota_url` | `https://api.tenclass.net/xiaozhi/ota/` | Change this for a self-hosted server |
| `client_id` | generated on first use | Persistent UUIDv4. **Do not edit it**: changing it gives the device a new identity |

Priority: **OTA result > `xiaozhi` namespace > official `websocket` namespace > built-in defaults**.
The OTA result is cached for 10 minutes; three consecutive connection failures fetch it again.

### Official backend (xiaozhi.me)

On first connection the device POSTs to the OTA endpoint:

- If it returns `activation{code,message}`, the screen shows "Activate: XXXXXX" with instructions: on xiaozhi.me, sign in, tap Add Device and enter the code. The device checks again every 30 seconds and connects once linked; **no restart is needed**.
- If it returns `websocket{url,token,version}`, the device is already linked and connects to start the conversation.

After linking, set the agent's language and voice to English in the xiaozhi.me console.

### Self-hosted server (xiaozhi-esp32-server)

Point `ota_url` in NVS at the self-hosted server's OTA address, or write `ws_url` and `version` directly.

---

## 6. Buttons

| Button | State | Action |
| --- | --- | --- |
| OK press | Not connected | Retry the connection now |
| OK press | Idle | Start talking |
| OK press | Listening | Stop talking, upload and wait for the reply |
| OK press | Replying | Interrupt (cut the sound first, then tell the server) |
| OK press | Thinking | Ignored (a 50-second timeout returns to Idle) |
| Volume + / Volume − | Any | Volume ±10% |
| Hold OK | Any | Return to the launcher (handled globally) |
