// "Why Barking": a quirky voice-controlled app.
//
// Memory strategy (hard requirements for release, still hold after the optimization):
//   1. No large buffers - no canvas/framebuffer; the dog and pipes are a resident pool of small objects
//      (~40 lv_obj), zero heap allocation during play;
//   2. No separate render task - the game loop is a 33ms lv_timer running in the LVGL task;
//      only two extra tasks: mic capture (3KB stack) and sound effects (2KB stack, can be switched off with one flag);
//   3. stop() reclaims everything: delete the timer first, then tear down the screen; tasks are joined/deleted before returning, heap back to its pre-entry level.
//
// ---------------------------------------------------------------------------
// 2026-09-13 optimization overview (details in OPTIMIZATION.md in this directory):
//   Perf  P1 Bubble text no longer calls lv_label_set_text every frame - in LVGL 9 that API has no "same-text
//           short-circuit"; every call is free+malloc+full relayout+invalidate, and it used to run 30 times/s,
//           fragmenting the heap and costing relayouts - the top source of dropped frames and heap churn. Now dirty-checked.
//        P2 Ears/mouth/volume bar/pipe visibility are all cached; LVGL is untouched when values don't change.
//        P3 Fixed s_frame only incrementing in READY (which is never entered), which made the canary run every frame.
//        P4 Mic: skip EMA during warm-up, seed from the first block (~64ms faster response); debug log can be disabled.
//   Arch  S1 Split into four parts: build / HUD / effects / loop; px_block/px_label share one naming scheme;
//        S2 Difficulty and hitbox constants centralized in the "tuning section" of game_core.h;
//        S3 Sound effects split out into their own game_sfx module.
//   Feel  F1 Death fall + sliding result panel, OK fast-forwards (no more instant restart);
//        F2 Fixed the fatal bug "re-enter right after leaving → mic task left over → no mic for the whole round";
//        F3 Start the mic first on entry so warm-up overlaps screen building.
//   UI    U1 Live volume bar on the left (input visibility, the most critical thing in a voice-controlled game);
//        U2 Score flash; U3 hint shortened to a single line; U4 new 20-point rank.
// Gameplay balance values (SPEED/GAP/thrust mapping/hitbox) all kept at their original values, unchanged.
// ---------------------------------------------------------------------------
#include "apps/game/app_game.h"

#include "apps/game/game_core.h"
#include "apps/game/game_mic.h"
#include "apps/game/game_sfx.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "fonts/app_fonts.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "display/lv_display_private.h" /* canary: detects inv_p out-of-bounds corruption */

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "nvs.h"
}

namespace {

constexpr char kTag[] = "app_game";

// ---------------------------------------------------------------------------
// Tuning section (edit here to try the feel; values in parentheses are the rollback values)
// ---------------------------------------------------------------------------

// Loudness → thrust mapping: measured quiet-room EMA ≈ 60, normal speech a few hundred to over a thousand, blowing/shouting >2500.
// Below FLOOR gives no thrust (natural fall), FULL gives full thrust; the end-of-round log includes the round's peak for re-tuning.
// Noisy environment → raise FLOOR; can't get it to climb → lower FULL. (original 150 / 2500)
constexpr uint16_t kVolFloor = 150;
constexpr uint16_t kVolFull = 2500;

constexpr uint32_t kFrameMs = 33;

// Master sound switch. Set to 0 to go back to the "silent version" entirely (game_sfx creates no task).
constexpr bool kSfxEnabled = true;

// Dog mouth: silent 2px → full volume 8px (same as original).
constexpr int kMouthMinH = 2;
constexpr int kMouthMaxH = 8;
constexpr int kMouthW = 7;
constexpr int kTongueX = 14;
constexpr int kTongueYBase = 8;

// Bubble thresholds (thrust 0..255): >60 shows "Woof!", ≥180 upgrades to "Woof woof!" (same as original).
constexpr int kBarkSoft = 60;
constexpr int kBarkLoud = 180;

// Score flash duration (frames). 6 ≈ 200ms.
constexpr int kScoreFlashFrames = 6;

// Death sequence: fall ~300ms (9 frames × 7px), then the panel slides in ~230ms (7 frames).
// Rollback: set kDeathFallFrames to 0 for the original "show results immediately" behavior.
constexpr int kDeathFallFrames = 9;
constexpr int kDeathFallPx = 7;
constexpr int kPanelAnimFrames = 7;
constexpr int kPanelShownY = 86;
constexpr int kPanelHiddenY = 340;

// Live volume bar on the left: lets the player see "how much the mic hears", the most important input visibility in a voice-controlled game.
constexpr int kVuX = 4;
constexpr int kVuY = 34;
constexpr int kVuW = 6;
constexpr int kVuH = 244;    // 34..278, doesn't overlap the grass (GC_GROUND_Y=284)
constexpr int kVuStep = 6;   // height quantization step, reduces pointless redraws

// Hint text (single line, no wrapping - the original 13 Chinese characters were 221px wide, over 216, and wrapped onto two lines over the grass)
constexpr char kHintPlay[] = "Bark to rise · OK: Jump";
constexpr char kHintDead[] = "OK: Again · Hold OK: Back";
constexpr char kHintReady[] = "OK: Start · Hold OK: Back";

// ---------------------------------------------------------------------------
// Colors (reusing the system pixel-art palette)
// ---------------------------------------------------------------------------
#define kInk (UI_INK)
#define kPaper (UI_PAPER)
constexpr uint32_t kDogBody = 0xE6A13B;  // Shiba golden coat
constexpr uint32_t kDogEar = 0xAA6518;   // brown ears
constexpr uint32_t kDogWhite = 0xFFF3D6; // light cheeks
constexpr uint32_t kMouthBg = 0x551111;  // dark red mouth
constexpr uint32_t kTongue = 0xFF6B8B;   // pink tongue
#define kGrass (UI_ACCENT)    // pipe body
#define kGrassD (UI_BORDER)   // pipe lip/outline

// ---------------------------------------------------------------------------
// Text cache - the core of this optimization
// ---------------------------------------------------------------------------

// lv_label_set_text() has no internal "same as current text, return early" short-circuit: every call does
// lv_free old string → lv_malloc new string → full relayout → invalidate. The "Woof!" bubble used to call it every frame
// unconditionally (30 times/s), fragmenting the heap and retriggering layout. So compare before calling.
struct TextCache {
    char text[48];
    void reset() { text[0] = '\0'; }
};

bool set_text_cached(lv_obj_t *label, TextCache &cache, const char *text) {
    if (!label || !text) return false;
    if (std::strcmp(cache.text, text) == 0) return false;  // unchanged: don't touch LVGL at all
    std::snprintf(cache.text, sizeof(cache.text), "%s", text);
    lv_label_set_text(label, cache.text);
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic<bool> s_active{false};
lv_obj_t *s_screen = nullptr;
lv_timer_t *s_timer = nullptr;

lv_obj_t *s_dog = nullptr;               // container 22x18
lv_obj_t *s_dog_ear = nullptr;           // swinging back ear
lv_obj_t *s_dog_mouth = nullptr;         // dynamically stretched mouth
lv_obj_t *s_dog_tongue = nullptr;        // tongue sticking out
lv_obj_t *s_bark_lbl = nullptr;          // live "Woof!" bubble
lv_obj_t *s_score_lbl = nullptr;
ui_pixel_battery_t s_battery{};
int64_t s_battery_us = 0;
lv_obj_t *s_hint_lbl = nullptr;
lv_obj_t *s_vu_fill = nullptr;           // volume bar fill (the track is its sibling, no handle needed)
lv_obj_t *s_dead_panel = nullptr;
lv_obj_t *s_dead_score = nullptr;
lv_obj_t *s_dead_best = nullptr;
lv_obj_t *s_dead_rating = nullptr;       // silly rank rating

// Render cache: don't write to LVGL when values haven't changed. lv_obj_set_x/y already short-circuit on equal values, but
// set_size / set_style / set_text don't, so one guard layer here is simplest.
int s_last_dog_y = -1;
int s_last_mouth_h = -1;
int s_last_ear_y = -1;
int s_last_vu_h = -1;
uint32_t s_last_vu_color = 0;
int s_dog_y = 0;                          // dog y used by the latest render (start of the death fall)

// Pipe pool: each pipe = 4 objects (upper body/upper lip/lower lip/lower body), recycled
lv_obj_t *s_pipe_obj[GC_PIPES][4];
int s_pipe_last_h[GC_PIPES][2];      // cached upper/lower body heights, -1 forces the first refresh
bool s_pipe_shown[GC_PIPES];         // whether it was on-screen last frame (avoids repeatedly moving off-screen pipes)

// Effects / scoring / stats
int s_score_cache = -1;
int s_score_flash = 0;
int s_dying = 0;        // >0: fall animation playing; 0: can restart
int s_panel_anim = 0;   // >0: result panel sliding in
int s_best = 0;
uint16_t s_peak_level = 0;
uint32_t s_frame = 0;

TextCache s_bark_cache;
TextCache s_hint_cache;

// Dropped-frame stats (file scope so start_run can reset them, avoiding carry-over between rounds)
int64_t s_last_tick_us = 0;
int32_t s_max_tick_ms = 0;
int s_slow_frames = 0;
int s_stat_frames = 0;

// Breathing bob table (READY state): ±6px, period about 1s
const int8_t kBob[16] = {0, 2, 4, 5, 6, 5, 4, 2, 0, -2, -4, -5, -6, -5, -4, -2};

// ---------------------------------------------------------------------------
// Building blocks - common px_* prefix, matching the system pixel-art style
// ---------------------------------------------------------------------------
lv_obj_t *px_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                   int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    // Explicitly opaque: skips alpha blending on every draw; solid blocks are the cheapest primitive on this device.
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t *px_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
void build_dog(lv_obj_t *parent) {
    s_dog = lv_obj_create(parent);
    lv_obj_remove_flag(s_dog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_dog, GC_BIRD_X, 140);
    lv_obj_set_size(s_dog, GC_BIRD_W, GC_BIRD_H);
    lv_obj_set_style_bg_opa(s_dog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dog, 0, 0);
    lv_obj_set_style_pad_all(s_dog, 0, 0);

    // 1. Head/body (golden, 4px radius)
    px_block(s_dog, 2, 2, 17, 13, kDogBody, 4);
    // 2. Left ear (back ear, wiggles while flying)
    s_dog_ear = px_block(s_dog, 0, 1, 5, 7, kDogEar, 2);
    // 3. Right ear (front ear)
    px_block(s_dog, 6, 0, 5, 6, kDogEar, 2);
    // 4. Light cheek/muzzle area
    px_block(s_dog, 10, 6, 10, 8, kDogWhite, 2);
    // 5. Black nose
    px_block(s_dog, 18, 5, 4, 3, kInk, 1);
    // 6. Goofy eyes (big white + black pupil)
    px_block(s_dog, 9, 3, 5, 5, 0xFFFFFF, 2);
    px_block(s_dog, 11, 4, 2, 2, kInk, 1);
    // 7. Dynamic mouth (dark red, initial height 2)
    s_dog_mouth = px_block(s_dog, 13, 9, kMouthW, kMouthMinH, kMouthBg, 1);
    // 8. Tongue (pink, starts inside the mouth)
    s_dog_tongue = px_block(s_dog, kTongueX, kTongueYBase + kMouthMinH, 5, 2, kTongue, 1);

    // 9. Bark bubble label (shown above the head)
    s_bark_lbl = px_label(parent, "", &buddy_font_16,
        ui_theme_get() == UI_THEME_PLAYFUL ? UI_WARNING : 0xFFE066, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_bark_lbl, GC_BIRD_X + GC_BIRD_W + 2, 134);
}

void build_pipes(lv_obj_t *parent) {
    for (int i = 0; i < GC_PIPES; ++i) {
        s_pipe_obj[i][0] = px_block(parent, -40, 0, GC_PIPE_W, 10, kGrass, 0);
        s_pipe_obj[i][1] = px_block(parent, -40, 0, GC_PIPE_W + GC_PIPE_LIP_OVERHANG * 2,
                                    GC_PIPE_LIP, kGrassD, 0);
        s_pipe_obj[i][2] = px_block(parent, -40, 0, GC_PIPE_W + GC_PIPE_LIP_OVERHANG * 2,
                                    GC_PIPE_LIP, kGrassD, 0);
        s_pipe_obj[i][3] = px_block(parent, -40, 0, GC_PIPE_W, 10, kGrass, 0);
        s_pipe_last_h[i][0] = -1;
        s_pipe_last_h[i][1] = -1;
        s_pipe_shown[i] = false;
    }
}

// Volume bar: track + fill. The fill starts transparent; update_vu() lights it by thrust.
void build_vu(lv_obj_t *parent) {
    px_block(parent, kVuX, kVuY, kVuW, kVuH, UI_SURFACE_ALT, 0);
    s_vu_fill = px_block(parent, kVuX, kVuY, kVuW, kVuH, UI_YELLOW, 0);
    lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
}

void build_dead_panel(lv_obj_t *parent) {
    // Paper base + ink outline, width 190, height 122 (geometry identical to before the optimization, only the slide-in animation added)
    lv_obj_t *panel = px_block(parent, 12, kPanelHiddenY, 216, 140, kPaper, 0);
    s_dead_panel = panel;
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_side(panel, ui_theme_is_classic()
        ? LV_BORDER_SIDE_FULL
        : static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_BORDER), 0);

    lv_obj_t *title = px_label(panel, "Play again?", &ui_font_20, UI_TEXT,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, 210);
    lv_obj_set_pos(title, 3, 10);

    s_dead_score = px_label(panel, "0", &ui_digits_32, UI_SKY_DARK,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_score, 210);
    lv_obj_set_pos(s_dead_score, 3, 34);

    s_dead_best = px_label(panel, "Best 0", &buddy_font_16, UI_SUB,
                           LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_best, 210);
    lv_obj_set_pos(s_dead_best, 3, 82);

    s_dead_rating = px_label(panel, "Too quiet. Try again", &buddy_font_16, UI_TEXT,
                             LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_rating, 210);
    lv_obj_set_pos(s_dead_rating, 3, 112);
}

// ---------------------------------------------------------------------------
// HUD - everything driven by "dirty checks"
// ---------------------------------------------------------------------------
void set_dog_y(int y) {
    if (y == s_last_dog_y) return;
    s_last_dog_y = y;
    s_dog_y = y;
    lv_obj_set_y(s_dog, y);
}

// Bubble: lv_label_set_text only when the text really changes; when non-empty it follows the dog (and avoids the top bar).
void update_bark(const char *text, int dog_y) {
    set_text_cached(s_bark_lbl, s_bark_cache, text);
    if (text[0] == '\0') return;
    int y = dog_y - 4;
    if (y < GC_TOP + 2) y = GC_TOP + 2;
    lv_obj_set_y(s_bark_lbl, y);
}

// Volume bar: compare after quantizing the height; fully transparent when silent (no HIDDEN - see the sync_pipes comment).
void update_vu(int thrust) {
    int h = (thrust * kVuH) / 255;
    h = (h / kVuStep) * kVuStep;
    if (h < kVuStep) h = 0;
    if (h == s_last_vu_h) return;

    if (h == 0) {
        lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
    } else {
        if (s_last_vu_h <= 0) lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_COVER, 0);
        lv_obj_set_size(s_vu_fill, kVuW, h);
        lv_obj_set_y(s_vu_fill, kVuY + (kVuH - h));   // grows upward from the bottom
        const uint32_t c = (thrust < 85) ? UI_GRASS
                                         : (thrust < kBarkLoud ? UI_YELLOW : UI_RED);
        if (c != s_last_vu_color) {
            lv_obj_set_style_bg_color(s_vu_fill, lv_color_hex(c), 0);
            s_last_vu_color = c;
        }
    }
    s_last_vu_h = h;
}

void set_hint(const char *text) { set_text_cached(s_hint_lbl, s_hint_cache, text); }

void sync_pipes() {
    for (int i = 0; i < GC_PIPES; ++i) {
        const gc_pipe_t *p = gc_pipe(i);
        const int px = (int)(p->x >> 4);
        const bool visible = px > -(GC_PIPE_W + 10) && px < 240;
        lv_obj_t **o = s_pipe_obj[i];
        if (!visible) {
            /* No HIDDEN flag: the LVGL 9.3 unhide path has triggered an inv_p infinite loop on this device
               (see the tick canary); same "move off-screen" approach as the result panel.
               With the s_pipe_shown cache, nothing is written to LVGL at all while off-screen. */
            if (s_pipe_shown[i]) {
                for (int k = 0; k < 4; ++k) lv_obj_set_pos(o[k], -40, 0);
                s_pipe_shown[i] = false;
            }
            continue;
        }
        s_pipe_shown[i] = true;
        const int gap_top = (int)(p->gap_y >> 4);
        const int gap_bot = gap_top + (int)p->gap_h;
        const int lip = GC_PIPE_LIP_OVERHANG;

        lv_obj_set_pos(o[0], px, GC_TOP);
        lv_obj_set_pos(o[1], px - lip, gap_top - GC_PIPE_LIP);
        lv_obj_set_pos(o[2], px - lip, gap_bot);
        lv_obj_set_pos(o[3], px, gap_bot + GC_PIPE_LIP);

        const int top_h = gap_top - GC_TOP;
        if (s_pipe_last_h[i][0] != top_h) {          // height only changes on recycle, set on demand
            lv_obj_set_size(o[0], GC_PIPE_W, top_h);
            s_pipe_last_h[i][0] = top_h;
        }
        const int bot_h = GC_GROUND_Y - gap_bot - GC_PIPE_LIP;
        if (s_pipe_last_h[i][1] != bot_h) {
            lv_obj_set_size(o[3], GC_PIPE_W, bot_h);
            s_pipe_last_h[i][1] = bot_h;
        }
    }
}

// ---------------------------------------------------------------------------
// High score (NVS)
// ---------------------------------------------------------------------------
void load_best(int *best) {
    *best = 0;
    nvs_handle_t h;
    if (nvs_open("flappy", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "best", &v) == ESP_OK) *best = v;
        nvs_close(h);
    }
}

void save_best(int best) {
    nvs_handle_t h;
    if (nvs_open("flappy", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "best", (uint8_t)best) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Start / end of a round
// ---------------------------------------------------------------------------
void start_run() {
    /* Entering the app takes off immediately / one press restarts after death: no more idle READY page,
       otherwise on entry you see "bobbing in place + empty screen", which looks like it's not running (user feedback). */
    s_score_cache = -1;
    s_score_flash = 0;
    s_dying = 0;
    s_panel_anim = 0;
    s_peak_level = 0;
    s_last_tick_us = 0;
    s_max_tick_ms = 0;
    s_slow_frames = 0;
    s_stat_frames = 0;

    lv_label_set_text(s_score_lbl, "0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_YELLOW), 0);
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, kPanelHiddenY);

    // Invalidate all render caches so the new round's first frame resets dog/mouth/ears/volume bar to their initial state
    s_last_dog_y = -1;
    s_last_mouth_h = -1;
    s_last_ear_y = -1;
    s_last_vu_h = -1;
    s_last_vu_color = 0;
    s_bark_cache.reset();
    if (s_bark_lbl) lv_label_set_text(s_bark_lbl, "");
    if (s_vu_fill) lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
    for (int i = 0; i < GC_PIPES; ++i) {
        s_pipe_last_h[i][0] = -1;
        s_pipe_last_h[i][1] = -1;
        s_pipe_shown[i] = true;   // sync_pipes corrects this to real visibility on the next frame
    }

    gc_start((uint32_t)esp_timer_get_time() & 0x7fffffffu);
    set_hint(kHintPlay);
    if (kSfxEnabled) game_sfx_play(GS_START);
}

void on_dead() {
    const int score = gc_score();
    const bool new_best = score > s_best;
    if (new_best) {
        s_best = score;
        save_best(s_best);
    }
    char line[24];
    std::snprintf(line, sizeof(line), "%d", score);
    lv_label_set_text(s_dead_score, line);
    std::snprintf(line, sizeof(line), "Best %d", s_best);
    lv_label_set_text(s_dead_best, line);

    // Silly rank rating (keeps the original four tiers, adds a 20-point easter-egg tier for a sense of achievement)
    const char *rating = "Too quiet. Try again";
    if (score >= 20) rating = "Every dog nearby joins in";
    else if (score >= 10) rating = "A legendary hound!";
    else if (score >= 5) rating = "Security guard alerted";
    else if (score >= 2) rating = "Lone dog, full of spirit";
    if (s_dead_rating) lv_label_set_text(s_dead_rating, rating);

    // Death sequence: mouth opens "awoo" while falling; the result panel slides in after the fall (content already filled)
    s_dying = kDeathFallFrames;
    s_panel_anim = 0;
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, kPanelHiddenY);
    lv_obj_set_size(s_dog_mouth, kMouthW, kMouthMaxH);
    lv_obj_set_pos(s_dog_tongue, kTongueX, kTongueYBase + kMouthMaxH);
    s_last_mouth_h = kMouthMaxH;
    lv_obj_set_y(s_dog_ear, 3);      // ears fully droop
    s_last_ear_y = 3;
    update_bark("Awoo!!", s_dog_y);
    set_hint(kHintDead); /* tell the player clearly how to continue, so they don't think it froze */
    if (kSfxEnabled) game_sfx_play(new_best ? GS_BEST : GS_DEAD);

    static const char *const kReason[] = {"none", "ground", "pipe-top", "pipe-bottom"};
    const gc_death_info_t d = gc_death_info();
    ESP_LOGI(kTag, "game over: score=%d best=%d peak=%u death=%s dog_y=%ld pipe_x=%ld "
                   "gap_top=%ld gap_h=%ld",
             score, s_best, s_peak_level, kReason[d.reason], (long)d.bird_y,
             (long)d.pipe_x, (long)d.gap_top, (long)d.gap_h);
}

// ---------------------------------------------------------------------------
// Three-part per-frame loop
// ---------------------------------------------------------------------------
void ready_tick() {
    // Hover/bob; also log a loudness sample every second to help calibrate voice thresholds
    const int bob = kBob[(s_frame >> 1) & 15];
    const int dog_y = ((GC_TOP + GC_GROUND_Y) / 2 - GC_BIRD_H / 2) + bob - 3;
    set_dog_y(dog_y);
    update_bark("", dog_y);
    if ((s_frame % 30) == 0) {
        ESP_LOGI(kTag, "mic level=%u", game_mic_level());
    }
}

void playing_tick() {
    app_registry_keep_awake(); /* voice-only control, so suppress screen sleep while playing */
    const uint16_t level = game_mic_level();
    if (level > s_peak_level) s_peak_level = level;
    int thrust = ((int)level - kVolFloor) * 255 / (kVolFull - kVolFloor);
    if (thrust < 0) thrust = 0;
    if (thrust > 255) thrust = 255;
    gc_set_thrust((uint8_t)thrust);
    gc_step();

    const int dog_y = (int)(gc_bird_y_q4() >> 4);
    set_dog_y(dog_y);

    // Mouth opens dynamically with sound (P2: don't write LVGL if the height is unchanged)
    const int mouth_h = kMouthMinH + thrust * (kMouthMaxH - kMouthMinH) / 255;
    if (mouth_h != s_last_mouth_h) {
        lv_obj_set_size(s_dog_mouth, kMouthW, mouth_h);
        lv_obj_set_pos(s_dog_tongue, kTongueX, kTongueYBase + mouth_h);
        s_last_mouth_h = mouth_h;
    }

    // Ear wiggle: ears perk up when rising, droop when falling (only written on the frame it flips)
    const int ear_y = (gc_bird_vy_q4() < 0) ? 0 : 2;
    if (ear_y != s_last_ear_y) {
        lv_obj_set_y(s_dog_ear, ear_y);
        s_last_ear_y = ear_y;
    }

    // Bark bubble (P1: only change text when crossing a level, no more per-frame malloc+relayout)
    const char *bark = "";
    if (thrust >= kBarkLoud) bark = "Woof woof!";
    else if (thrust > kBarkSoft) bark = "Woof!";
    update_bark(bark, dog_y);

    update_vu(thrust);
    sync_pipes();

    if (gc_score() != s_score_cache) {
        s_score_cache = gc_score();
        char text[12];
        std::snprintf(text, sizeof(text), "%d", s_score_cache);
        lv_label_set_text(s_score_lbl, text);
        // Score flash ~200ms: makes "passed a pipe" visible even in peripheral vision
        lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_TEXT), 0);
        s_score_flash = kScoreFlashFrames;
        if (kSfxEnabled) game_sfx_play(GS_SCORE);
    }
    if (s_score_flash > 0 && --s_score_flash == 0) {
        lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_YELLOW), 0);
    }

    if (gc_phase() == GC_DEAD) on_dead();
}

void dead_tick() {
    if (s_panel_anim > 0) {                        // result panel slides in
        --s_panel_anim;
        const int done = kPanelAnimFrames - s_panel_anim;
        const int y = kPanelHiddenY +
                      (kPanelShownY - kPanelHiddenY) * done / kPanelAnimFrames;
        lv_obj_set_y(s_dead_panel, y);
        return;
    }
    if (s_dying > 0) {                             // dog keeps falling (visual only, logic already stopped)
        --s_dying;
        int y = s_dog_y + kDeathFallPx;
        const int floor_y = GC_GROUND_Y - GC_BIRD_H;
        if (y > floor_y) y = floor_y;
        set_dog_y(y);
        update_bark("Awoo!!", y);
        if (s_dying == 0) s_panel_anim = kPanelAnimFrames;
    }
}

void tick(lv_timer_t *) {
    if (!s_active.load()) return;
    const bool visible = app_registry_screen_is_on();
    const bool playing = gc_phase() == GC_PLAYING;
    if (s_timer) lv_timer_set_period(s_timer, visible || playing ? kFrameMs : 1000);
    if (!visible && !playing) { s_last_tick_us = 0; return; }

    const int64_t now = esp_timer_get_time();
    if (now - s_battery_us >= 5000000) { ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = now; }
    if (s_last_tick_us) {
        const int64_t dt_ms = (now - s_last_tick_us) / 1000;
        if (dt_ms > 45) ++s_slow_frames;
    }
    s_last_tick_us = now;

    // P3: s_frame used to increment only in the READY branch, but the game takes off on entry and READY is never entered →
    // s_frame stayed 0 → the canary check below ran every frame. Now incremented every frame.
    ++s_frame;

    switch (gc_phase()) {
    case GC_READY:   ready_tick();   break;
    case GC_PLAYING: playing_tick(); break;
    case GC_DEAD:    dead_tick();    break;
    }

    const int64_t spent_ms = (esp_timer_get_time() - now) / 1000;
    if (spent_ms > s_max_tick_ms) s_max_tick_ms = (int32_t)spent_ms;
    if (++s_stat_frames >= 30) { /* ~1s summary: only log when frames drop abnormally, quiet otherwise */
        if (s_slow_frames >= 5) {
            ESP_LOGW(kTag, "frame drops: %d/%d max_tick=%ldms", s_slow_frames,
                     s_stat_frames, (long)s_max_tick_ms);
        }
        s_slow_frames = 0;
        s_stat_frames = 0;
        s_max_tick_ms = 0;
    }

    /* Canary: in LVGL 9.3 lv_inv_area, uint16 i is compared against uint32 inv_p; once inv_p is
       corrupted by an OOB write to >65535, i wraps → infinite loop (caught in the act by WDT on real hardware). Only warns on anomaly. */
    if ((s_frame & 31) == 0) {
        lv_display_t *disp = lv_display_get_default();
        if (disp && disp->inv_p > LV_INV_BUF_SIZE) {
            ESP_LOGE(kTag, "CANARY: inv_p corrupted=%u", (unsigned)disp->inv_p);
        }
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void build_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    ui_pixel_background(s_screen);

    lv_obj_t *bar = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = px_label(bar, "Why Barking", &buddy_font_16, UI_TEXT,
                               LV_TEXT_ALIGN_LEFT);
    ui_pixel_status_text_align(title, 12, 96, LV_TEXT_ALIGN_LEFT);
    s_score_lbl = px_label(bar, "0", &lv_font_montserrat_14, UI_YELLOW,
                           LV_TEXT_ALIGN_RIGHT);
    ui_pixel_status_text_align(s_score_lbl, 110, UI_STATUS_CONTENT_RIGHT - 110,
                               LV_TEXT_ALIGN_RIGHT);
    s_battery = ui_pixel_battery_create(bar, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = esp_timer_get_time();

    build_pipes(s_screen);
    build_vu(s_screen);
    build_dog(s_screen);

    // Hint: paper capsule (square corners + ink outline, same as the design system).
    // It used to be plain white text directly over the grass; white/grass-green contrast was only about 2.2:1, hard to read;
    // with a paper base + UI_SUB it's about 6:1, and matches the hint capsules in other apps.
    s_hint_lbl = px_label(s_screen, "", &buddy_font_16, UI_SUB, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_hint_lbl, 216);
    lv_obj_set_style_bg_color(s_hint_lbl, lv_color_hex(kPaper), 0);
    lv_obj_set_style_bg_opa(s_hint_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_hint_lbl, 4, 0);
    lv_obj_set_style_radius(s_hint_lbl, 0, 0);
    lv_obj_set_style_border_width(s_hint_lbl, 0, 0);
    lv_obj_set_style_border_color(s_hint_lbl, lv_color_hex(UI_BORDER), 0);
    lv_obj_align(s_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    build_dead_panel(s_screen);

    load_best(&s_best);
    s_frame = 0;
    s_peak_level = 0;
    s_hint_cache.reset();
    start_run(); /* takes off on entry, so the page immediately shows "flying forward" motion */
}

}  // namespace

extern "C" {

static void game_init(void) {}

static void game_start(void) {
    if (s_active.load()) return;
    s_active.store(true);
    /* F3: start mic capture before building the screen - the mic has a ~160ms power-up transient to drop, which overlaps
       with building 40 lv_obj, so the first frame doesn't sink first because "the level isn't ready yet". */
    game_mic_start();
    if (kSfxEnabled) game_sfx_start();
    bsp_lvgl_lock(-1);
    build_ui();                       // building the screen must hold the lock: start runs in the input task context
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, kFrameMs, nullptr);
    bsp_lvgl_unlock();
    ESP_LOGI(kTag, "started; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static bool game_prepare_stop(void) {
    s_active.store(false);
    const bool sound_done = !kSfxEnabled || game_sfx_prepare_stop();
    const bool mic_done = game_mic_prepare_stop();
    return sound_done && mic_done;
}

static void game_stop(void) {
    s_active.store(false);
    // Stop the sfx task first (it may be blocked writing I2S), then mic capture, then tear down the screen - the order matters.
    if (kSfxEnabled) game_sfx_stop();
    // 2026-09-13 fix (B2): deleting the screen used to be inside if (s_timer) - when the heap was tight
    // lv_timer_create returned NULL, the whole branch was skipped, and the screen and all children were never freed.
    // Now the timer and the UI are handled separately and the screen is deleted **unconditionally**.
    bsp_lvgl_lock(-1);
    if (s_timer) {              // delete the timer first so tick can't re-enter while the screen is being torn down
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    bsp_lvgl_unlock();
    s_dog = s_dog_ear = s_dog_mouth = s_dog_tongue = s_bark_lbl = nullptr;
    s_score_lbl = s_hint_lbl = s_vu_fill = nullptr;
    s_dead_panel = s_dead_score = s_dead_best = s_dead_rating = nullptr;
    for (int i = 0; i < GC_PIPES; ++i)
        for (int k = 0; k < 4; ++k) s_pipe_obj[i][k] = nullptr;
    game_mic_stop();
    bsp_audio_deinit();
    ESP_LOGI(kTag, "stopped; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void game_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_active.load() || !s_screen || event != BTN_EVT_CLICK) return;
    if (btn != BSP_BTN_OK && btn != BSP_BTN_UP) return;

    /* This callback runs in the input task: lv_* calls such as set_hint must hold the lock. An unlocked
       invalidate at game start races with the render task and corrupts disp->inv_p to >65535;
       lv_inv_area's uint16 loop variable then wraps → LVGL infinite loop (caught by WDT on real hardware).
       (LVGL's lv_timer callbacks hold the same lock, so tick and this function are naturally mutually exclusive,
       and the non-atomic state below needs no extra locking.) */
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) lv_timer_set_period(s_timer, kFrameMs);

    switch (gc_phase()) {
    case GC_READY:
        start_run(); /* READY should no longer be entered; fallback: take off directly */
        break;
    case GC_PLAYING:
        gc_flap();
        break;
    case GC_DEAD:
        if (s_dying > 0) {
            /* Still playing the fall: treat this OK as "fast-forward" straight to results instead of restarting -
               otherwise two quick presses restart instantly and the player never gets to see their score. */
            s_dying = 0;
            s_panel_anim = kPanelAnimFrames;
        } else {
            start_run(); /* OK on the death panel starts another round immediately */
        }
        break;
    }

    bsp_lvgl_unlock();
}

const passport_app_t g_game_app = {
    .id = APP_ID_GAME,
    .name = "Why Barking",
    .en_name = "WHY BARKING",
    .desc = "Make noise to rise, go quiet to fall\nUse your voice to clear the pipes",
    .tag = "DOG",
    .theme_color = 0xEEA438,
    .init = game_init,
    .start = game_start,
    .stop = game_stop,
    .on_key = game_on_key,
    .prepare_stop = game_prepare_stop,
};
}
