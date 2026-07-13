/**
 * Capsule Radar — LVGL v9.x configuration.
 * Reached via -DLV_CONF_INCLUDE_SIMPLE (LVGL does #include "lv_conf.h").
 * Only the settings we care about are listed; everything else falls back to the
 * library defaults. Tuned for the CO5300 466x466 AMOLED on ESP32-S3.
 */
#if 1 /* Enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
/* 16-bit RGB565 — native order. Byte-swapping (if ever needed) is handled via
   lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAP) in display.cpp.
   LV_COLOR_16_SWAP no longer exists in v9; swap is set per-display. */
#define LV_COLOR_DEPTH 16

#define LV_COLOR_MIX_ROUND_OFS 0

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
/* LV_STDLIB_CLIB: use the system malloc/free (which on ESP32-S3 with PSRAM is
   backed by heap_caps and can draw from SPIRAM when internal heap is tight).
   This avoids the fixed-size TLSF pool that crashes when LVGL v9's draw task
   pipeline needs more than LV_MEM_SIZE bytes during ui_create(). */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* Only used when LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN; kept for reference.
   At LV_STDLIB_CLIB this block is ignored. */
#define LV_MEM_SIZE (128U * 1024U)
#define LV_MEM_ADR  0

/*====================
   HAL SETTINGS
 *====================*/
/* v9 uses a single refresh period for display + indev (was two separate macros).
   Tick source: call lv_tick_set_cb(millis) in display::begin() for Arduino.
   LV_TICK_CUSTOM no longer exists; use the runtime API instead. */
#define LV_DEF_REFR_PERIOD 16   /* ms; ~60 Hz cap */

/* A snapshot can invalidate old + new glyph bounds for up to 20 aircraft.
 * The LVGL default (32) overflows here and silently upgrades the refresh to
 * the whole 466x466 screen, which visibly pauses the Ripple every poll. */
#define LV_INV_BUF_SIZE 64

#define LV_DPI_DEF 130

/*=================
   OPERATING SYSTEM
 *=================*/
/* LV_OS_NONE: Arduino single-threaded. Rendering is driven manually:
   call lv_draw_dispatch() in the loop to service pending draw tasks.
   Do NOT use LV_OS_FREERTOS here — it spawns rendering threads that
   conflict with lv_timer_handler() running in the Arduino loop task. */
#define LV_USE_OS   LV_OS_NONE

/*=======================
   RENDERING CONFIGURATION
 *=======================*/
/* Disable ARM Helium ASM — this firmware targets ESP32-S3 (Xtensa), not ARM Cortex-M85.
   Without this, LVGL 9.x tries to compile lv_blend_helium.S which fails on xtensa-as. */
#define LV_USE_DRAW_SW                 1
#define LV_USE_NATIVE_HELIUM_ASM       0
#define LV_USE_DRAW_SW_ASM             LV_DRAW_SW_ASM_NONE

/*==================
   LOG (serial)
 *==================*/
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
#endif

/*==================
   ASSERTS / DEBUG
 *==================*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*==================
   FONTS
 *==================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================
   WIDGETS
 *==================*/
#define LV_USE_ARC      1
#define LV_USE_LABEL    1
#define LV_USE_SPINNER  1
#define LV_USE_LIST     1
#define LV_USE_TILEVIEW 1
#define LV_USE_CANVAS   1
#define LV_USE_IMAGE    1

#endif /* LV_CONF_H */
#endif /* Enable content */
