// main/boot_splash.c - boot animation: phosphor green on black.
//   "PASSPORT OS" appears typewriter-style with a blinking trailing cursor; below it a 14-cell green dot progress bar lights up cell by cell.
//   Total ~0.9s, runs at the same time as the boot sound (chime), in sync.
#include "boot_splash.h"
#include "ui_palette.h"

#include "bsp_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>

static const char *TAG = "boot_splash";

// Same phosphor palette as the home screen
#define C_BG      UI_BG
#define C_PHOS    UI_ACCENT
#define C_PHOSDIM UI_TEXT_DIM
#define C_BORDER  UI_BORDER

#define BAR_N     14
#define STEP_MS   62          // 14 cells × 62ms ≈ 0.87s

static lv_obj_t *s_splash;

static lv_obj_t *blk(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

void boot_splash_run(void) {
    bsp_lvgl_lock(-1);

    // Make sure any in-flight SPI DMA transaction has fully completed
    esp_lcd_panel_io_handle_t io = bsp_display_io();
    if (io) {
        esp_lcd_panel_io_tx_param(io, 0x00, NULL, 0);
    }

    s_splash = lv_obj_create(NULL);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_splash, 0, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_set_style_outline_width(s_splash, 0, 0);
    lv_obj_set_style_shadow_width(s_splash, 0, 0);
    lv_obj_set_style_pad_all(s_splash, 0, 0);

    // Main title: PASSPORT OS (the typewriter effect comes from a "cursor block" - the full text is laid out first,
    // covered by a mask in the background color that moves right on each beat to reveal it; same as typing, without re-laying out the text)
    lv_obj_t *title = lv_label_create(s_splash);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_PHOS), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "PASSPORT OS");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -26);

    // Subtitle
    lv_obj_t *sub = lv_label_create(s_splash);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_PHOSDIM), 0);
    lv_obj_set_style_text_letter_space(sub, 1, 0);
    lv_label_set_text(sub, "TRAVEL INSTRUMENT");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 4);

    // Measure the actual title rather than assuming a 150 px width. A mask
    // reveals the existing text, so its alignment never jumps between frames.
    lv_obj_update_layout(title);
    const int title_x = lv_obj_get_x(title);
    const int title_y = lv_obj_get_y(title);
    const int title_w = lv_obj_get_width(title);
    const int title_h = lv_obj_get_height(title);
    lv_obj_t *mask = blk(s_splash, title_x, title_y, title_w, title_h, C_BG);
    lv_obj_t *cursor = blk(s_splash, title_x, title_y, 2, title_h, C_PHOS);

    // Decorative startup rhythm: these cells do not measure network progress.
    (void)blk(s_splash, (240 - (BAR_N * 13 - 3)) / 2, 186,
              BAR_N * 13 - 3, 10, UI_SURFACE_ALT);
    lv_obj_t *cells[BAR_N];
    for (int i = 0; i < BAR_N; ++i) {
        cells[i] = blk(s_splash, (240 - (BAR_N * 13 - 3)) / 2 + i * 13, 186,
                       10, 10, C_PHOS);
        lv_obj_add_flag(cells[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_screen_load(s_splash);

    // While holding the lock throughout, synchronously push the complete first frame of 320 rows (27 blocks) to the ST7789
    // with no unlock in between, fully preventing the top red line/misalignment caused by taskLVGL racing with direct DMA writes
    const int64_t t0 = esp_timer_get_time();
    lv_refr_now(lv_display_get_default());
    if (io) {
        esp_lcd_panel_io_tx_param(io, 0x00, NULL, 0); // Block until the last DMA block has landed
    }
    const int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "first frame synced: refr=%lldms", (long long)((t1 - t0) / 1000));
    bsp_lvgl_unlock();

    // Only after the first frame has fully landed, fade in the backlight (140ms, CRT power-on feel, no white flash and no red line)
    for (int b = 0; b <= 100; b += 10) {
        bsp_display_backlight(b);
        vTaskDelay(pdMS_TO_TICKS(14));
    }

    // Beat: the cursor moves right across the title width (~150px) with progress, dots light up in sync
    for (int i = 0; i < BAR_N; ++i) {
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        if (!bsp_lvgl_lock(200)) continue;
        lv_obj_clear_flag(cells[i], LV_OBJ_FLAG_HIDDEN);
        const int revealed = title_w * (i + 1) / BAR_N;
        lv_obj_set_x(mask, title_x + revealed);
        lv_obj_set_width(mask, title_w - revealed);
        lv_obj_set_x(cursor, title_x + revealed);
        if (i + 1 == BAR_N) {
            lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
        }
        // Cursor blink: semi-transparent on even cells
        lv_obj_set_style_opa(cursor, (i % 2) ? LV_OPA_40 : LV_OPA_COVER, 0);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(140));
    ESP_LOGI(TAG, "splash done");
}

void boot_splash_end(void) {
    if (!s_splash) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_obj_delete(s_splash);
    s_splash = NULL;
    bsp_lvgl_unlock();
}
