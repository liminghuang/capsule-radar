// M0 bring-up: CO5300 466x466 AMOLED via Arduino_GFX (QSPI) + LVGL v9.
// Pins come from config.h (confirmed against the Waveshare board definition and a
// working Arduino_GFX port for this exact panel). The panel runs off the always-on
// DC1 rail, so it lights up without configuring the AXP2101 PMIC.
// The actual UI is built by ui_create() (shared with the native SDL sim).
#include "display.h"
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "touch_cst9217.h"
#include "ripple_compositor.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

// --- Arduino_GFX panel -------------------------------------------------------
static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

// --- LVGL v9 plumbing --------------------------------------------------------
#define LVGL_BUF_LINES 40    // partial draw-buffer height; kept in fast internal RAM
static lv_display_t  *s_disp  = nullptr;
static lv_indev_t    *s_indev = nullptr;
static lv_color_t    *s_buf1  = nullptr;

static volatile uint32_t s_frameCount = 0;
uint32_t display_frames() { return s_frameCount; }

static volatile uint8_t s_rot = 0;
static lv_color_t *s_rotBuf    = nullptr;  // PSRAM scratch for 90/270° transpose
static lv_color_t *s_baseFrame = nullptr;  // PSRAM scene copy for ripple compositor
static display::RippleWave s_oldRipple[2];
static int s_oldRippleCount = 0;
static lv_color_t s_line[SCREEN_W];
static uint8_t s_dirty[SCREEN_W], s_alpha[SCREEN_W];

// LVGL v9 flush callback.  px_map is uint8_t* in v9; cast to lv_color_t* for our code.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int w = (int)(area->x2 - area->x1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    lv_color_t *px  = (lv_color_t *)px_map;
    lv_color_t *out = px;
    int16_t  dx = area->x1, dy = area->y1;
    uint16_t dw = (uint16_t)w, dh = (uint16_t)h;

    // Capture logical-coordinate scene BEFORE rotation transform (ripple compositor).
    if (s_baseFrame) {
        for (int row = 0; row < h; ++row)
            memcpy(s_baseFrame + (area->y1 + row) * SCREEN_W + area->x1,
                   px + row * w, (size_t)w * sizeof(lv_color_t));
    }

    switch (s_rot) {
        case 2:  // 180°
            for (int i = 0, j = w * h - 1; i < j; ++i, --j) { lv_color_t t = px[i]; px[i] = px[j]; px[j] = t; }
            dx = (int16_t)(SCREEN_W - 1 - area->x2);
            dy = (int16_t)(SCREEN_H - 1 - area->y2);
            break;
        case 1:  // 90° CW
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[i * h + (h - 1 - j)] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = (int16_t)(SCREEN_H - 1 - area->y2); dy = area->x1;
            }
            break;
        case 3:  // 270° CW
            if (s_rotBuf) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        s_rotBuf[(w - 1 - i) * h + j] = px[j * w + i];
                out = s_rotBuf; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = area->y1; dy = (int16_t)(SCREEN_W - 1 - area->x2);
            }
            break;
        default: break;  // 0°
    }
    // LV_COLOR_16_SWAP removed in v9; panel uses native RGB565 (no byte-swap).
    s_gfx->draw16bitRGBBitmap(dx, dy, (uint16_t *)out, dw, dh);
    if (lv_display_flush_is_last(disp)) s_frameCount++;
    lv_display_flush_ready(disp);
}

// CO5300 (QSPI) requires 2-pixel-aligned flush windows: even start, odd end.
// In v9, rounder_cb attaches to the display object (same signature, new type).
static void rounder_cb(lv_event_t *e) {
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (!area) return;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// CST9217 touch -> LVGL pointer.  First param is lv_indev_t* in v9.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        uint16_t lx = x, ly = y;
        switch (s_rot) {
            case 1: lx = y;                             ly = (uint16_t)(SCREEN_H - 1 - x); break;
            case 2: lx = (uint16_t)(SCREEN_W - 1 - x); ly = (uint16_t)(SCREEN_H - 1 - y); break;
            case 3: lx = (uint16_t)(SCREEN_W - 1 - y); ly = x;                             break;
            default: break;
        }
        data->point.x = (lv_coord_t)lx;
        data->point.y = (lv_coord_t)ly;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

namespace display {

bool begin() {
    Serial.println("[display] init CO5300 QSPI...");
    s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK,
                                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0 /*rotation*/,
                               SCREEN_W, SCREEN_H,
                               LCD_COL_OFFSET, LCD_ROW_OFFSET, 0, 0);
    if (!s_gfx->begin(LCD_QSPI_HZ)) {
        Serial.println("[display] gfx->begin() FAILED");
        return false;
    }
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setBrightness(BRIGHTNESS_DEFAULT);
    Serial.println("[display] panel up; init LVGL v9...");

    lv_init();
    // v9: set tick source via callback (replaces LV_TICK_CUSTOM in lv_conf.h).
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Draw buffer in INTERNAL DMA RAM (faster than PSRAM for rendering).
    const size_t buf_px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }

    // v9: create display object, set buffers and callbacks.
    s_disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, nullptr,
                           buf_px * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    // CO5300 alignment fix: even x-start, odd x-end.
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);

    // PSRAM scratch buffer for software 90°/270° rotation transpose.
    s_rotBuf    = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_baseFrame = (lv_color_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_baseFrame) Serial.println("[display] direct Ripple base frame unavailable");

    // Touch input (CST9217) -> LVGL pointer indev.
    if (touch_begin()) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        Serial.println("[display] CST9217 touch registered");
    }

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();
    Serial.println("[display] LVGL v9 ready");
    return true;
}

void loop() { lv_timer_handler(); }

void setBrightness(uint8_t v) { if (s_gfx) s_gfx->setBrightness(v); }

void setRotation(uint8_t quarters) {
    s_rot = (uint8_t)(quarters & 3);
    s_oldRippleCount = 0;
    radar::setTheme(radar::theme());
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);
}
uint8_t rotation() { return s_rot; }
const uint16_t *baseFrame() { return (const uint16_t *)s_baseFrame; }

static void markSpan(const RippleRowSpans &spans, bool draw, uint8_t alpha) {
    const int16_t starts[2] = {spans.leftStart, spans.rightStart};
    const int16_t ends[2]   = {spans.leftEnd,   spans.rightEnd};
    for (int n = 0; n < 2; ++n) for (int x = starts[n]; x <= ends[n]; ++x) {
        if (x < 0 || x >= SCREEN_W) continue;
        s_dirty[x] = 1;
        if (draw && alpha > s_alpha[x]) s_alpha[x] = alpha;
    }
}

static void markRing(int16_t y, const RippleWave &wave, bool draw) {
    const RippleRowSpans halo = rippleRowSpans(SCREEN_CX, SCREEN_CY, wave.radius, (float)RIPPLE_GLOW_WIDTH_PX, y, 0, SCREEN_W - 1);
    if (halo.valid) markSpan(halo, draw, (uint8_t)((wave.opacity * RIPPLE_GLOW_OPACITY_PERCENT) / 100));
    if (draw) {
        const RippleRowSpans core = rippleRowSpans(SCREEN_CX, SCREEN_CY, wave.radius, 2.0f, y, 0, SCREEN_W - 1);
        if (core.valid) markSpan(core, true, wave.opacity);
    }
}

bool rippleOverlay(const RippleWave *waves, int count, uint32_t rgb) {
    if (!s_gfx || !s_baseFrame || s_rot != 0 || !waves || count < 1 || count > 2) return false;
    const lv_color_t color = lv_color_hex(rgb);
    const int16_t yFirst = LV_MAX(0, SCREEN_CY - RIPPLE_R_OUTER_PX - RIPPLE_GLOW_WIDTH_PX);
    const int16_t yLast  = LV_MIN(SCREEN_H - 1, SCREEN_CY + RIPPLE_R_OUTER_PX + RIPPLE_GLOW_WIDTH_PX);
    s_gfx->startWrite();
    for (int16_t y = yFirst; y <= yLast; ++y) {
        memset(s_dirty, 0, sizeof(s_dirty)); memset(s_alpha, 0, sizeof(s_alpha));
        for (int i = 0; i < s_oldRippleCount; ++i) markRing(y, s_oldRipple[i], false);
        for (int i = 0; i < count; ++i) markRing(y, waves[i], true);
        for (int x = 0; x < SCREEN_W;) {
            while (x < SCREEN_W && !s_dirty[x]) ++x;
            const int dirtyStart = x;
            while (x < SCREEN_W && s_dirty[x]) ++x;
            if (x > dirtyStart) {
                const int start = dirtyStart & ~1;
                const int end = LV_MIN(SCREEN_W - 1, (x - 1) | 1);
                for (int px = start; px <= end; ++px) {
                    const lv_color_t base = s_baseFrame[y * SCREEN_W + px];
                    s_line[px - start] = s_dirty[px] && s_alpha[px]
                        ? lv_color_mix(color, base, s_alpha[px]) : base;
                }
                s_gfx->draw16bitRGBBitmap(start, y, (uint16_t *)s_line, end - start + 1, 1);
            }
        }
    }
    s_gfx->endWrite();
    s_oldRippleCount = count;
    for (int i = 0; i < count; ++i) s_oldRipple[i] = waves[i];
    return true;
}

void clearRippleOverlay() { s_oldRippleCount = 0; }

uint32_t inactiveMs() { return lv_display_get_inactive_time(nullptr); }

} // namespace display

