// components/bsp/src/bsp_display.c
// Ported from trae_card/components/platform/platform_esp32/src/disp_st7789.c
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool                      s_bl_ready;
static uint8_t                   s_requested_brightness = 100;
static uint8_t                   s_master_brightness = 80;

// ---------------------------------------------------------------------------
// ST7789P3 vendor-specific init sequence (porch / power / gamma).
// These are the VALUES FROM THE PANEL VENDOR'S REFERENCE TFT_init() ROUTINE, not generic ST7789 defaults -
// a different panel needs its own sequence from its vendor; copying this one will most likely display incorrectly.
//
// The following four are done by the built-in esp_lcd driver, so they are not repeated here:
//   0x11 SLPOUT / 0x3A COLMOD → esp_lcd_panel_init()
//   0x21 INVON                → esp_lcd_panel_invert_color()
//   0x29 DISPON               → esp_lcd_panel_disp_on_off()
//   0x36 MADCTL               → esp_lcd_panel_mirror() (⚠ do not write 0x36 manually again; it gets overwritten)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},   // PORCTRL frame rate porch
    {0xB7, {0x35}, 1, 0},                            // GCTRL gate
    {0xBB, {0x21}, 1, 0},                            // VCOMS
    {0xC0, {0x2C}, 1, 0},                            // LCMCTRL
    {0xC2, {0x01}, 1, 0},                            // VDVVRHEN
    {0xC3, {0x0B}, 1, 0},                            // VRHS
    {0xC4, {0x20}, 1, 0},                            // VDVSET
    {0xC6, {0x0F}, 1, 0},                            // FRCTRL2 60Hz dot inversion
    {0xD0, {0xA7, 0xA1}, 2, 0},                      // PWCTRL1
    {0xD0, {0xA4, 0xA1}, 2, 0},                      // PWCTRL1 (re-sent by the reference routine, overrides the previous entry)
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},   // PVGAMCTRL positive gamma
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},  // NVGAMCTRL negative gamma
};

static void backlight_init(void) {
    if (BSP_LCD_BL < 0) { ESP_LOGW(TAG, "backlight pin not wired to MCU, brightness fixed"); return; }
    ledc_timer_config_t t = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES,
        .freq_hz         = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_USE_XTAL_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(e)); return; }

    ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(e)); return; }

    s_bl_ready = true;
    ESP_LOGI(TAG, "backlight LEDC ready gpio=%d", BSP_LCD_BL);
}

esp_err_t bsp_display_init(void) {
    if (s_panel) return ESP_OK;

    spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI,
        .sclk_io_num = BSP_LCD_SCLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_W * 80 * 2,
    };
    esp_err_t e = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (%s) — check MOSI=GPIO%d / SCLK=GPIO%d for conflicts",
                 esp_err_to_name(e), BSP_LCD_MOSI, BSP_LCD_SCLK);
        return e;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .spi_mode = BSP_LCD_SPI_MODE,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io);
    if (e != ESP_OK) { ESP_LOGE(TAG, "panel_io creation failed: %s", esp_err_to_name(e)); return e; }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_RST,          // -1 → SWRESET soft reset
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    if (e != ESP_OK) { ESP_LOGE(TAG, "panel creation failed: %s", esp_err_to_name(e)); return e; }

    esp_lcd_panel_reset(s_panel);   // uses SWRESET when rst=-1
    esp_lcd_panel_init(s_panel);    // SLPOUT / COLMOD / RAMCTRL

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *c = &ST7789P3_CMDS[i];
        esp_err_t r = esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len);
        if (r != ESP_OK) ESP_LOGE(TAG, "vendor init command 0x%02X failed: %s", c->cmd, esp_err_to_name(r));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    esp_lcd_panel_invert_color(s_panel, BSP_LCD_INVERT_COLOR);   // 0x21 / 0x20
    esp_lcd_panel_mirror(s_panel, false, false);                 // 0x36 MADCTL: this board needs no mirroring (XY double mirror = picture rotated 180°)
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);                    // 0x29 DISPON

    // After DISPON and before turning on the backlight, fill the whole GRAM with black first.
    // ST7789 GRAM is uninitialized at power-up and shows white; whenever the backlight comes on even slightly
    // before the first frame is fully flushed, a white flash shows. Pinning it to black here removes the source of these flashes.
    {
        esp_err_t fr = bsp_display_fill(0x0000);
        if (fr != ESP_OK) ESP_LOGW(TAG, "screen clear failed: %s", esp_err_to_name(fr));
    }

    backlight_init();
    ESP_LOGI(TAG, "display ready %dx%d", BSP_LCD_W, BSP_LCD_H);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }

esp_lcd_panel_io_handle_t bsp_display_io(void) { return s_io; }

// Fill the whole GRAM synchronously with a solid color, bypassing LVGL.
//
// Why it is needed: LVGL here uses a **12-line single buffer** (bsp_display_lvgl.c), so the 320-line screen is
// pushed in 27 chunks, and flush is asynchronous. So lv_refr_now() may not finish pushing the first frame - any area
// not yet pushed when the backlight turns on shows "uninitialized white GRAM at power-up / LVGL's default white screen",
// which is the white flash at the moment the backlight turns on at boot.
//
// Here esp_lcd_panel_draw_bitmap writes chunk by chunk synchronously (no callback → polling transfer, returns only when done),
// guaranteeing the whole screen is a known color on return. Call it before turning on the backlight and the white flash is gone.
//
// color is LVGL RGB565 (same order as lv_color_to_u16); the panel uses swap_bytes,
// so swap the high and low bytes here ourselves, matching esp_lvgl_port's .swap_bytes = true.
esp_err_t bsp_display_fill(uint16_t color) {
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    const int    chunk = 16;                       // 16 lines/chunk: 16*240*2 = 7.5KB
    const size_t n     = (size_t)BSP_LCD_W * chunk;
    uint16_t    *buf   = (uint16_t *)heap_caps_malloc(n * sizeof(uint16_t),
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) return ESP_ERR_NO_MEM;

    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));   // → big-endian
    for (size_t i = 0; i < n; ++i) buf[i] = be;

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < BSP_LCD_H; y += chunk) {
        const int h = (y + chunk <= BSP_LCD_H) ? chunk : (BSP_LCD_H - y);
        esp_err_t r = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_W, y + h, buf);
        if (r != ESP_OK) { ret = r; break; }
    }
    // Wait until all queued SPI DMA transfers have fully landed, so DMA does not read freed memory and corrupt data
    if (s_io) {
        esp_lcd_panel_io_tx_param(s_io, 0x00, NULL, 0);
    }
    heap_caps_free(buf);
    return ret;
}

void bsp_display_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_requested_brightness = percent;
    percent = (uint8_t)(((uint16_t)percent * s_master_brightness) / 100U);
    if (!s_bl_ready) return;
    uint32_t max_duty = (1u << BSP_BL_LEDC_RES) - 1u;
    uint32_t duty = (max_duty * percent) / 100u;
    ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

void bsp_display_set_master_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_master_brightness = percent;
    bsp_display_backlight(s_requested_brightness);
}

uint8_t bsp_display_get_master_brightness(void) {
    return s_master_brightness;
}

void bsp_display_power(bool on) {
    if (!s_panel || !s_io) return;      // no-op if not initialized (the init sequence already sent DISPON)
    if (on) {
        // Panel power-up SPI activity couples into ADC1_CH0 and corrupts button readings (fake UP press → fake PTT).
        // The window covers SLPOUT+120ms+DISPON+LVGL full-screen refresh; during it button checks report "not pressed".
        bsp_button_suppress_panel_glitch();
        esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);    // SLPOUT wake
        vTaskDelay(pdMS_TO_TICKS(120));                     // wait for the internal clock to settle (required after SLPIN)
        esp_lcd_panel_disp_on_off(s_panel, true);           // DISPON resume display
    } else {
        // Order matters: DISPOFF(0x28) first to stop pixel drive, then SLPIN(0x10) to stop the internal oscillator.
        // On the P3 panel, sending only SLPIN was measured not to blank the screen - the LCD is still scan-driven and leaves ghosting.
        esp_lcd_panel_disp_on_off(s_panel, false);          // DISPOFF all pixels off
        esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0);     // SLPIN sleep
    }
}
