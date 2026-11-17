// main/app_types.h - shared pure-C types: state machine / events / actions / UI snapshot.
// Includes no ESP-IDF headers, so it compiles directly on the host for tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- Key actions (injected downstream, executed by the PC client) ----------------
typedef enum {
    APP_KEY_ENTER = 0,   // Enter in the input box (submit)
    APP_KEY_CLEAR,       // clear all text in the input box
} app_key_action_t;

// ---------------- States ----------------
// Note: named app_stage_t rather than app_state_t - the latter is the full state struct in app_state.h.
typedef enum {
    APP_ST_HOME = 0,        // idle home page
    APP_ST_READY,           // workflow ready (PTT available)
    APP_ST_LISTENING,       // recording (PTT held)
    APP_ST_TRANSCRIBING,    // legacy name: voice done, waiting for DOWN=Enter to send
    APP_ST_AGENT_RUNNING,   // Agent running
    APP_ST_APPROVAL,        // waiting for physical approval
    APP_ST_COUNT,
} app_stage_t;

// ---------------- Keys ----------------
// Physical key semantics (2026-08-29): UP (volume up) = press to record, release to send (push to talk);
// DOWN (volume down) = click for Enter / long-press 0.5s to clear the input box; OK = click to navigate·approve /
// long-press 0.5s to lock/unlock. Each key only has "click + long-press", no double-click (see app_state.c).
typedef enum {
    APP_BTN_UP = 0,
    APP_BTN_DOWN,
    APP_BTN_OK,
} app_btn_t;

// ---------------- Prompt tones ----------------
typedef enum {
    APP_TONE_START = 0,     // 440Hz/80ms  recording ready
    APP_TONE_SEND,          // 880Hz/60ms  voice sent
    APP_TONE_APPROVAL,      // 587+880/150ms approval alert
    APP_TONE_SUCCESS,       // 660→880/200ms done
    APP_TONE_REJECT,        // 220Hz/120ms reject
    APP_TONE_ERROR,         // 180Hz/100ms offline/invalid action
    APP_TONE_COUNT,
} app_tone_t;

// ---------------- Events ----------------
typedef enum {
    APP_EV_KEY_PRESS = 0,
    APP_EV_KEY_RELEASE,
    APP_EV_KEY_CLICK,
    APP_EV_KEY_DOUBLE,
    APP_EV_KEY_LONG,        // the instant a long press reaches the threshold (UP key 0.5s -> start talking)
    APP_EV_KEY_LONG_UP,     // release after the long-press timeout (long-press state no longer reports RELEASE; UP key -> stop talking)
    APP_EV_AGENT_STATUS,
    APP_EV_APPROVAL_REQUEST,
    APP_EV_TRANSCRIPT,
    APP_EV_AUDIO_DROP_START,
    APP_EV_AUDIO_DROP_END,
    // Renamed 2026-09-13 (D2): the channel moved from BLE to Wi-Fi UDP, but the events were still named BLE,
    // making readers think Bluetooth was still in use. Renamed to LINK_* meaning "link", independent of the channel.
    APP_EV_LINK_UP,         // link up: peer is ready (sufficient condition for PTT)
    APP_EV_LINK_DOWN,       // link down (disconnected or peer lost)
    APP_EV_LINK_DROP,       // event line dropped because of disconnect/not ready (UI toast feedback)
    APP_EV_AUDIO_ERROR,     // capture hardware error
    APP_EV_INIT_FAILED,     // Local transport/pipeline startup failed; re-enter to retry
    APP_EV_TONE_DONE,       // START tone finished playing (signal for async stream start, see design)
    APP_EV_TICK,            // 100ms heartbeat, drives timeouts/screen-off
    // ---- USB wired channel (USB-Serial-JTAG, see design.md) ----
    APP_EV_USB_CONNECTED,   // USB session up (PC handshake ping received; USB-side sufficient condition for link_up)
    APP_EV_USB_DISCONNECTED, // USB session down (unplugged: is_connected flipped)
    APP_EV_TIME_SET,         // time-set downlink (epoch seconds UTC; shared by both channels: CTRL time.set line + SYS time set)
    APP_EV_PC_STATUS,        // Hotkey forwarder capability heartbeat (not ASR status)
} app_event_type_t;

// ---------------- Link channel (dual always-on channel architecture, 2026-08-28) ----------------
// The old exclusive mode (BLE=0/USB=2, NVS-persisted + switch on reboot) is fully retired: BLE advertising is always on,
// the USB data channel works on plug-in (usb_link on standby, auto-attaches when a host appears). Session routing follows
// link_channel: the most recently connected/used channel in the state machine; audio and voice.* uplink follow it.
typedef enum {
    APP_CHAN_BLE = 0,       // BLE (wireless): up once the EVENT characteristic is subscribed
    APP_CHAN_USB = 2,       // USB (wired): up after the PC ping handshake
    APP_CHAN_NONE = 0xFF,   // both channels down (link_up=false)
} app_chan_t;

// Agent status (conversion to/from protocol strings in app_protocol.c)
typedef enum {
    APP_AGENT_READY = 0,
    APP_AGENT_THINKING,
    APP_AGENT_RUNNING,
    APP_AGENT_ERROR,
    APP_AGENT_DONE,
    APP_AGENT_COUNT,
} app_agent_state_t;

// Injection mode (kept for protocol compatibility: the Mac side injects, the device no longer consumes it; UI shows the transcript)
typedef enum {
    APP_INJECT_TYPE = 0,    // old: HID types character by character (retired, field kept only)
    APP_INJECT_PASTE,       // old: paste with Cmd+V (retired, field kept only)
} app_inject_mode_t;

// Approval risk level
typedef enum {
    APP_RISK_LOW = 0,
    APP_RISK_MEDIUM,
    APP_RISK_HIGH,
    APP_RISK_COUNT,
} app_risk_t;

// Approval action
typedef enum {
    APP_ACTION_APPROVE = 0,
    APP_ACTION_REJECT,
    APP_ACTION_DETAILS,
} app_approval_decision_t;

// Text length limits (sent by the Mac per item, device only displays)
#define APP_TRANSCRIPT_MAX    128
#define APP_TASK_ID_MAX       32
#define APP_TITLE_MAX         64
#define APP_TARGET_MAX        64
#define APP_DIFF_MAX          64
// Display channel: must be ≥ APP_TRANSCRIPT_MAX so transcript lines split by relay at 128B land on screen intact
#define APP_AGENT_MSG_MAX     APP_TRANSCRIPT_MAX
#define APP_TOAST_MAX         64

// Event (fixed-size struct, copied into the queue, union kept small)
typedef struct {
    app_event_type_t type;
    union {
        // mv = bsp_button_read_mv() (mV) at callback time: a real press reads 3-5, a fake press (RF
        // corrosion) reads 2890 = released level - app_state uses this to tell real from fake LONG (see the READY state)
        struct { uint8_t btn; uint16_t mv; } key;       // KEY_*
        struct {
            uint8_t state;                              // app_agent_state_t
            char message[APP_AGENT_MSG_MAX];
        } agent_status;                                 // AGENT_STATUS
        struct {
            char task_id[APP_TASK_ID_MAX];
            char title[APP_TITLE_MAX];
            char target[APP_TARGET_MAX];
            char diff_summary[APP_DIFF_MAX];
            uint8_t risk;                               // app_risk_t
        } approval;                                     // APPROVAL_REQUEST
        struct {
            char text[APP_TRANSCRIPT_MAX];
            uint8_t inject_mode;                        // app_inject_mode_t (kept for protocol compatibility)
            bool final;                                 // false = preview (not final); true = final
        } transcript;                                   // TRANSCRIPT
        struct { int64_t epoch; } time_set;             // TIME_SET (UTC seconds; int64 aligns to 8, union still ≤228B)
        struct { bool audio_ready; bool hotkey_ready; } pc_status;
    } u;
} app_event_t;

// ---------------- Actions ----------------
typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_UI_REFRESH,
    APP_ACT_UI_SCREEN_OFF,
    APP_ACT_UI_SCREEN_ON,
    APP_ACT_UI_PANEL_OFF,   // panel SLPIN power-off (60s idle level)
    APP_ACT_UI_PANEL_ON,    // panel SLPOUT power-on (wake)
    APP_ACT_SEND_VOICE_START,
    APP_ACT_SEND_VOICE_END,
    APP_ACT_SEND_KEY_ACTION,   // upstream key action (enter/clear, injected by the PC client)
    APP_ACT_SEND_AGENT_ACTION,
    APP_ACT_STREAM_START,
    APP_ACT_STREAM_STOP,
    APP_ACT_STREAM_CANCEL,   // cancel/disconnect: stop capture + clear ring + discard in-flight frames (differs from STOP: no drain of sends)
    APP_ACT_PLAY_TONE,
    APP_ACT_TIME_SET,        // time_sync_set_epoch (apply time set)
} app_action_type_t;

// Maximum actions produced per event. emit() silently drops when full, so this must be ≥ the longest
// reduction path, otherwise trailing actions are lost - 4 -> 6 (2026-08-29): pressing volume up to record from the
// screen-off/powered-off state is the longest path; after the wake triple (UI_PANEL_ON + UI_SCREEN_ON + UI_REFRESH)
// come PLAY_TONE + SEND_VOICE_START = 5 actions, and the 5th was exactly the one dropped - the result was
// the beep played and capture started, but the Mac never got voice.start (the whole utterance never reached ASR).
// Equally long are "screen off + volume-down long-press clear" (wake triple + SEND_KEY_ACTION + PLAY_TONE)
// and offline PTT (wake triple + PLAY_TONE(ERROR) + UI_REFRESH). 6 leaves one spare.
#define APP_ACT_MAX 6

typedef struct {
    app_action_type_t type;
    union {
        uint8_t tone;                                   // PLAY_TONE
        struct { uint8_t action; } key_action;          // SEND_KEY_ACTION(app_key_action_t)
        struct {
            char task_id[APP_TASK_ID_MAX];
            uint8_t decision;                           // app_approval_decision_t
        } agent_action;                                 // SEND_AGENT_ACTION
        struct { int64_t epoch; } time_set;             // TIME_SET
    } u;
} app_action_t;

// ---------------- UI snapshot ----------------
typedef struct {
    app_stage_t    state;
    bool           link_up;       // false -> OFFLINE (link_name DISCONNECTED) banner + PTT disabled; true = channel up
    bool           pc_ready;      // Fresh PC audio + hotkey readiness, separate from UDP discovery
    bool           init_failed;
    bool           screen_on;
    bool           panel_on;      // panel power (false after SLPIN power-off)
    bool           net_busy;      // audio dropping frames -> BUSY banner
    bool           ble_connected; // whether the BLE connection is established (icon grey while not subscribed)
    char           link_name[8];  // current link channel name: "BLE"/"USB" (the disconnect banner renders from this)
    bool           battery_available;
    uint8_t        battery_soc;   // 0..100
    bool           wifi_connected;
    int8_t         wifi_rssi;     // Actual station signal, independent of PC discovery.
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final; // false -> the current agent_message is a transcript preview (not final, UI adds a cursor feel)
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;   // app_risk_t
    uint32_t       elapsed_ms;      // how long the current state has lasted (filled in by the main loop at snapshot time)
    char           toast[APP_TOAST_MAX];
} app_ui_snapshot_t;

// ---------------- Timeout constants ----------------
#define APP_IDLE_BACKLIGHT_OFF_MS   20000u   // no key press -> backlight off (screen off, rendering skipped)
#define APP_IDLE_PANEL_OFF_MS       60000u   // no key press -> panel SLPIN power-off (μA level)
#define APP_TRANSCRIBE_TIMEOUT  30000u   // transcription wait timeout
#define APP_AGENT_RUN_TIMEOUT   90000u   // Agent run timeout
#define APP_TICK_MS             100u     // app task heartbeat
// S3: maximum wait for TONE_DONE after the START tone (fallback stream start). 500 -> 200 (2026-08-29):
// the beep itself is only 80ms; the 500ms margin was for "the sound task getting starved", but it is also the worst-case
// PTT latency - real hardware once caught 622ms from press to recording (= this value + APP_TICK_MS granularity), with the user already
// talking while the device had not started recording. 200 still leaves 2.5× margin; the cost is that if the beep really is delayed it overlaps
// capture (the first frames record the 440Hz), only on the degraded path, which is acceptable. The fallback firing leaves a trace on the serial log
// (recognize STREAM_START in the main.c heartbeat), so next time it is slow, read the log instead of guessing.
#define APP_TONE_PENDING_TIMEOUT_MS 200u
// Maximum single PTT utterance guard: raised to 10 minutes (600 s) to allow long continuous natural speech input,
// while still preventing a physically stuck key from draining the battery.
#define APP_PTT_MAX_TALK_MS     600000u

#ifdef __cplusplus
}
#endif
