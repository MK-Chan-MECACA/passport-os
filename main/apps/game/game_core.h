// Voice-controlled bird (game_core): pure logic layer, no LVGL.
// C3 has no FPU; everything uses Q4 fixed point (1 unit = 1/16 pixel), multiply/divide only via shifts/small constants.
// Call gc_step() once per frame (33ms); frame length is fixed and physics is tuned for 30fps.
//
// 2026-09-13 optimization: no values changed (gameplay balance kept 1:1), only
//   1. moved the feel/difficulty constants scattered in the .c here into one "tuning section", so feel can be tried by editing one place;
//   2. added magic numbers (hitbox inset etc.) that previously lived only in comments, so the UI layer can reference the same definitions;
//   3. the death snapshot gets a single clear interface, so a restart doesn't keep the previous round's coordinates.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_BIRD_X 56      /* fixed x of the character (px) */
#define GC_BIRD_W 22
#define GC_BIRD_H 18
#define GC_TOP 26         /* bottom edge of the top bar: upper bound of the flight area */
#define GC_GROUND_Y 284   /* top edge of the grass: lower bound of the flight area */
#define GC_PIPE_W 34
#define GC_PIPE_LIP 10    /* height of the wider pipe lip */
#define GC_PIPES 3        /* pipe pool size: recycled, zero allocation during play */

/* ---- Hitbox ---- */

/* How many px the hitbox is inset from each side of the sprite. 3 = visual leniency (dog nose/ears poking out 3px don't count).
   Smaller is harder (2 = slightly harder, 0 = pixel-exact); larger is more lenient. **Changing this changes
   difficulty**; to revert, set back to 3. */
#define GC_HIT_INSET 3

/* How many px the pipe lip extends past the pipe body on each side. The lip looks wider, but collision only uses the body -
   a deliberate leniency so "grazing past the lip" doesn't kill the player for no visible reason. */
#define GC_PIPE_LIP_OVERHANG 4

/* ---- Difficulty curve tuning section ----
   The 6 values below are the only sources of difficulty; current values are identical to before the optimization.
   To adjust difficulty, only touch these; each notes the effect of raising/lowering it, for easy rollback. */

/* Pipe horizontal speed (Q4/frame). 16 Q4 = 1px/frame ≈ 30px/s.
   Higher = shorter reaction time (harder); lower = more relaxed. Current 52 ≈ 98px/s. */
#define GC_BASE_SPEED 52

/* Speed increment per point and the score where it caps. Currently 2/8 → +16 Q4 at the cap (≈112px/s).
   Higher = tenser late game; lower cap score = reaches constant speed sooner. */
#define GC_SPEED_GAIN 2
#define GC_SPEED_BONUS_MAX 8

/* Horizontal pipe spacing (px). Current 140 ≈ one every 1.3~1.4s, leaving enough time to adjust.
   Lower = denser (harder), but below 120 it squeezes the reachability constraint. */
#define GC_SPACING 140

/* Gap height: starts at 120, narrows by 2 per point, minimum 96.
   Lower GAP_MIN = more hardcore; the current 96 is already quite lenient for an 18px-tall dog. */
#define GC_GAP_START 120
#define GC_GAP_MIN 96
#define GC_GAP_SHRINK 2

/* Max vertical shift between consecutive gaps (px). This is the "reachability constraint": beyond the dog's climb/sink ability it's certain death,
   and the player feels "I lost without hitting anything" (added after two such deaths in a row at score 12).
   Higher = more thrilling but unreachable gaps appear; lower = gentler. */
#define GC_GAP_STEP_MAX 56

typedef enum {
    GC_READY = 0,  /* waiting to start: bird hovers/bobs; make a sound or press OK to start */
    GC_PLAYING,
    GC_DEAD,
} gc_phase_t;

// Death snapshot: reason 1=hit the ground 2=hit above the gap 3=hit below the gap; coordinates in px.
typedef struct {
    int reason;
    int32_t bird_y;
    int32_t pipe_x;
    int32_t gap_top;
    int32_t gap_h;
} gc_death_info_t;

typedef struct {
    int32_t x;      /* Q4 */
    int32_t gap_y;  /* gap top, px (Q4) */
    int32_t gap_h;  /* gap height, px */
    bool scored;
} gc_pipe_t;

// Back to READY; clear pipes and score.
void gc_reset(void);

// READY → PLAYING. seed drives the gap sequence (fed from esp_timer, different every round).
void gc_start(uint32_t seed);

// Advance one frame (only meaningful in PLAYING; READY/DEAD return immediately).
void gc_step(void);

// Sound level 0..255: 0 = steady slow sink, 255 = fastest steady climb, linear in between.
// Speed is clamped to the level endpoints, so talking never rockets it up and silence never drops it instantly.
void gc_set_thrust(uint8_t thrust);

// Flap (OK button): set this frame's velocity straight to the flap value, applied before sound thrust.
void gc_flap(void);

gc_phase_t gc_phase(void);
int32_t gc_bird_y_q4(void);   /* bird top edge, Q4 */
int32_t gc_bird_vy_q4(void);  /* vertical velocity, Q4/frame */
const gc_pipe_t *gc_pipe(int index);
int gc_score(void);
gc_death_info_t gc_death_info(void);  /* valid in DEAD: cause of death + both sides' coordinates, for log diagnosis */

#ifdef __cplusplus
}
#endif
