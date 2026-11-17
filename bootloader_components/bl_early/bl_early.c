// Boot white-screen fix: pull the backlight pin (GPIO21) low and enable its output at the very start of the second-stage bootloader.
//
// Background: when the MCU pin floats the backlight circuit gets "lit", while the ST7789 panel is neither initialized
// nor has anything written to GRAM - an undriven transmissive panel under backlight is just white. app_main() does
// pull the pin low, but that is ~560ms after power-on; the time the second-stage bootloader spends loading/verifying the whole
// app image is most of the white-screen window, and app-side code can never cover it.
//
// This uses IDF's bootloader hooks (bootloader_before_init / bootloader_after_init,
// both weak, overridden by strong definitions in this project) to move "pull backlight low" to the very start of the second-stage bootloader.
// Only ROM functions and direct register writes, no dependency on driver/any runtime service (at before_init
// not even the flash cache is up yet).
//
// At before_init the peripheral clock may not be on yet; to be sure it takes effect, write again in after_init (idempotent).

#include <stdint.h>

#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"

// Keep in sync with BSP_LCD_BL in components/bsp/include/bsp_pins.h.
// The bootloader is not part of main's component dependencies, so it is hardcoded here (change it here too if the backlight pin changes).
#define BSP_LCD_BL_EARLY 21

// Set to 1 to enable boot diagnostic output (prints GPIO21 out/oe/IE + floating level per stage).
// Normally 0, leaving only a single "pulled low" confirmation log line.
#define BL_EARLY_DIAG 0

static void backlight_drive_low(void) {
    // 1. Connect the pad to the GPIO matrix
    esp_rom_gpio_pad_select_gpio(BSP_LCD_BL_EARLY);
    // 2. Select the GPIO register itself as the output source (not some peripheral signal)
    esp_rom_gpio_connect_out_signal(BSP_LCD_BL_EARLY, SIG_GPIO_OUT_IDX, false, false);
    // 3. Write 0 first, then enable output: in the reverse order the moment output is enabled would drive the leftover high level
    REG_WRITE(GPIO_OUT_W1TC_REG, (1u << BSP_LCD_BL_EARLY));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1u << BSP_LCD_BL_EARLY));
}

#if BL_EARLY_DIAG
// Read back the GPIO21 config to confirm the "pull low" really took effect and was not reverted by a later stage.
// Only reads registers, never touches any CSR - ⚠ ESP32-C3 **does not implement the standard cycle CSR (0xC00)**
// (SOC_CPU_HAS_CSR_PC=1, cycle counting uses private CSR 0x7e2). Reading 0xC00 this early causes an immediate
// Illegal instruction → infinite bootloader reboot.
//   out=output level oe=output enable in=pin readback IE=input enable WPU/WPD=internal pull-up/pull-down
static void diag_dump(const char *where) {
    uint32_t out = REG_READ(GPIO_OUT_REG);
    uint32_t oe  = REG_READ(GPIO_ENABLE_REG);
    uint32_t in  = REG_READ(GPIO_IN_REG);
    uint32_t pin = REG_READ(GPIO_PIN21_REG);
    esp_rom_printf("[bl_early] %s out=%u oe=%u in=%u IE=%u WPU=%u WPD=%u\n",
                   where,
                   (unsigned)((out >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((oe >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((in >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((pin >> 8) & 1u),
                   (unsigned)((pin >> 7) & 1u),
                   (unsigned)((pin >> 6) & 1u));
}

// Measure "what level the external circuit pulls the pin to when floating".
// ⚠ esp_rom_gpio_pad_select_gpio() only switches MCU_SEL to the GPIO function and **does not** enable
//   the input buffer (measured: IE is still 0 after the call, so reading GPIO_IN always gives 0, which is meaningless).
//   IE=1 must be set explicitly and the internal pull-up/down disabled; only then is the level "decided by the external circuit".
// in=1 → there is a pull-up on the board (or on the backlight driver EN pin); floating lights the backlight = the direct cause of the white screen.
static void diag_float_level(void) {
    esp_rom_gpio_pad_select_gpio(BSP_LCD_BL_EARLY);
    REG_SET_BIT(GPIO_PIN21_REG, 1u << 8);                 // IE=1 enables the input buffer
    REG_CLR_BIT(GPIO_PIN21_REG, (1u << 7) | (1u << 6));   // WPU=0 WPD=0
    uint32_t pin = REG_READ(GPIO_PIN21_REG);
    uint32_t in  = REG_READ(GPIO_IN_REG);

    // Then read again with the internal pull-down (~45kΩ) attached: if still high, the external pull-up is strong (or more than
    // weak leakage), and floating will definitely light the backlight.
    REG_SET_BIT(GPIO_PIN21_REG, 1u << 6);                 // WPD=1
    uint32_t in_pd = REG_READ(GPIO_IN_REG);
    REG_CLR_BIT(GPIO_PIN21_REG, 1u << 6);                 // Restore

    esp_rom_printf("[bl_early] float IE=%u WPU=%u WPD=%u in=%u in_pd=%u\n",
                   (unsigned)((pin >> 8) & 1u),
                   (unsigned)((pin >> 7) & 1u),
                   (unsigned)((pin >> 6) & 1u),
                   (unsigned)((in >> BSP_LCD_BL_EARLY) & 1u),
                   (unsigned)((in_pd >> BSP_LCD_BL_EARLY) & 1u));
}
#endif

// The IDF bootloader forces this symbol to be kept with `-u bootloader_hooks_include`.
// Reason: the hooks are **weak references** in bootloader_start.c, and the linker does **not** extract archive members
// for "weak undefined" symbols - defining only bootloader_before_init() gets it
// dropped outright (measured: in the disassembly both hook calls in call_start_cpu0 compile to
// `li a5,0 / beqz` and never run). Defining this symbol is what pulls this object file into the link.
void bootloader_hooks_include(void);
void bootloader_hooks_include(void) {}

void bootloader_before_init(void) {
#if BL_EARLY_DIAG
    // 1) What level the external circuit pulls it to when floating - to find the white-screen cause
    diag_float_level();
    diag_dump("pre_hook ");
#endif
    backlight_drive_low();
#if BL_EARLY_DIAG
    diag_dump("post_hook");
#endif
}

void bootloader_after_init(void) {
    backlight_drive_low();
#if BL_EARLY_DIAG
    diag_dump("after_init");
#else
    // The bootloader console is ready at this point; leave one line of trace so we can later confirm the hook really ran.
    esp_rom_printf("[bl_early] backlight GPIO%d driven low\n", BSP_LCD_BL_EARLY);
#endif
}
