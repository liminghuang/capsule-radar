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
static uint16_t      *s_buf1  = nullptr;

static volatile uint32_t s_frameCount = 0;
uint32_t display_frames() { return s_frameCount; }

static volatile uint8_t s_rot = 0;
static uint16_t *s_rotBuf    = nullptr;    // PSRAM scratch for 90/270° transpose
static uint16_t *s_baseFrame = nullptr;    // PSRAM scene copy for ripple compositor (RGB565)
static bool s_baseRows[SCREEN_H] = {};
static bool s_baseReady = false;
static display::RippleWave s_oldRipple[2];
static int s_oldRippleCount = 0;
static uint32_t s_rippleRgb = 0x39FF14;
static bool s_restoreRippleAfterFlush = false;
// In LVGL v9, lv_color_t = RGB888 (3 bytes). Our buffers store RGB565 (uint16_t).
#define RIPPLE_TILE_ROWS 16
static uint16_t s_spanTile[SCREEN_W * RIPPLE_TILE_ROWS];
static uint8_t s_dirty[SCREEN_W], s_alpha[SCREEN_W];

// LVGL v9 flush callback.  px_map is uint8_t* in v9; cast to lv_color_t* for our code.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // In LVGL v9, lv_color_t is always RGB888 (3 bytes) regardless of LV_COLOR_DEPTH.
    // The actual pixel format in px_map is determined by lv_display_set_color_format(),
    // which we set to LV_COLOR_FORMAT_RGB565 → px_map contains 2-byte RGB565 pixels.
    // All pixel operations must use uint16_t* (NOT lv_color_t*).
    const int w = (int)(area->x2 - area->x1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    uint16_t *px  = (uint16_t *)px_map;
    uint16_t *out = px;
    int16_t  dx = area->x1, dy = area->y1;
    uint16_t dw = (uint16_t)w, dh = (uint16_t)h;

    // Capture logical-coordinate scene for ripple compositor (RGB565 pixels).
    if (s_baseFrame) {
        uint16_t *bf = (uint16_t *)s_baseFrame;  // treat baseFrame as RGB565
        for (int row = 0; row < h; ++row)
            memcpy(bf + (area->y1 + row) * SCREEN_W + area->x1,
                   px + row * w, (size_t)w * sizeof(uint16_t));
        if (area->x1 == 0 && area->x2 == SCREEN_W - 1) {
            for (int row = area->y1; row <= area->y2; ++row) {
                if (row >= 0 && row < SCREEN_H) s_baseRows[row] = true;
            }
            if (!s_baseReady) {
                s_baseReady = true;
                for (int row = 0; row < SCREEN_H; ++row) {
                    if (!s_baseRows[row]) { s_baseReady = false; break; }
                }
            }
        }
    }

    switch (s_rot) {
        case 2: {  // 180°
            for (int i = 0, j = w * h - 1; i < j; ++i, --j) { uint16_t t = px[i]; px[i] = px[j]; px[j] = t; }
            dx = (int16_t)(SCREEN_W - 1 - area->x2);
            dy = (int16_t)(SCREEN_H - 1 - area->y2);
            break;
        }
        case 1: {  // 90° CW
            uint16_t *rb = (uint16_t *)s_rotBuf;
            if (rb) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        rb[i * h + (h - 1 - j)] = px[j * w + i];
                out = rb; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = (int16_t)(SCREEN_H - 1 - area->y2); dy = area->x1;
            }
            break;
        }
        case 3: {  // 270° CW
            uint16_t *rb = (uint16_t *)s_rotBuf;
            if (rb) {
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i)
                        rb[(w - 1 - i) * h + j] = px[j * w + i];
                out = rb; dw = (uint16_t)h; dh = (uint16_t)w;
                dx = area->y1; dy = (int16_t)(SCREEN_W - 1 - area->x2);
            }
            break;
        }
        default: break;  // 0°
    }
    // Arduino_GFX's generic RGB bitmap function writes one QSPI transaction per
    // pixel. Use the panel's bulk path so LVGL partial flushes and Ripple spans
    // are transferred as contiguous QSPI payloads instead.
    s_gfx->startWrite();
    s_gfx->writeAddrWindow(dx, dy, dw, dh);
    s_gfx->writePixels(out, (uint32_t)dw * dh);
    s_gfx->endWrite();
    // A large LVGL scene update (notably the 2 s ADS-B refresh) overwrites the
    // direct overlay. Restore the current ring before yielding so it cannot
    // visibly blink off for a frame.
    if (s_oldRippleCount > 0 && w * h >= (SCREEN_W * SCREEN_H) / 4) {
        s_restoreRippleAfterFlush = true;
    }
    if (lv_display_flush_is_last(disp)) {
        if (s_restoreRippleAfterFlush && display::rippleSnapshotReady()) {
            s_restoreRippleAfterFlush = false;
            display::rippleOverlay(s_oldRipple, s_oldRippleCount, s_rippleRgb);
        }
        s_frameCount++;
    }
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
    s_buf1 = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }

    // v9: create display object, set buffers and callbacks.
    s_disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    // Set color format BEFORE set_buffers — v9.2.2 uses the current format to
    // calculate stride when initialising the draw buffer in set_buffers().
    // Wrong order → stride=0 or h=0 → buf_act->data_size=0 → refr_timer bails out.
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf1, nullptr,
                           buf_px * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    // CO5300 alignment fix: even x-start, odd x-end.
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);

    // PSRAM scratch buffer for software 90°/270° rotation transpose.
    s_rotBuf    = (uint16_t *)heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_baseFrame = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
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
    lv_draw_dispatch();
    Serial.println("[display] LVGL v9 ready");
    return true;
}

void loop() {
    lv_timer_handler();
}

void setBrightness(uint8_t v) { if (s_gfx) s_gfx->setBrightness(v); }

void setRotation(uint8_t quarters) {
    s_rot = (uint8_t)(quarters & 3);
    s_oldRippleCount = 0;
    s_baseReady = false;
    memset(s_baseRows, 0, sizeof(s_baseRows));
    radar::setTheme(radar::theme());
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);
}
uint8_t rotation() { return s_rot; }
const uint16_t *baseFrame() { return s_baseFrame; }
bool rippleSnapshotReady() { return s_baseFrame && s_baseReady && s_rot == 0; }

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
#if RIPPLE_GLOW_WIDTH_PX > 0
    // Draw broad, faint bands first, then progressively tighter/brighter bands.
    // markSpan keeps the strongest value at overlaps, producing a radial alpha
    // gradient without a full-screen alpha buffer.
    for (int layer = RIPPLE_GLOW_LAYERS; layer >= 1; --layer) {
        const float width = (float)RIPPLE_GLOW_WIDTH_PX * (float)layer / (float)RIPPLE_GLOW_LAYERS;
        const uint8_t percent = (uint8_t)((RIPPLE_GLOW_OPACITY_PERCENT * (RIPPLE_GLOW_LAYERS - layer + 1)) / RIPPLE_GLOW_LAYERS);
        const RippleRowSpans halo = rippleRowSpans(SCREEN_CX, SCREEN_CY, wave.radius, width, y, 0, SCREEN_W - 1);
        if (halo.valid) markSpan(halo, draw, (uint8_t)((wave.opacity * percent) / 100));
    }
#endif
    if (draw) {
        const RippleRowSpans core = rippleRowSpans(SCREEN_CX, SCREEN_CY, wave.radius, 2.0f, y, 0, SCREEN_W - 1);
        if (core.valid) markSpan(core, true, wave.opacity);
    }
}

struct SpanBounds {
    int16_t start = SCREEN_W;
    int16_t end = -1;
    bool valid() const { return start <= end; }
};

static void markDirtyRow(int16_t y, const display::RippleWave *waves, int count) {
    memset(s_dirty, 0, sizeof(s_dirty));
    memset(s_alpha, 0, sizeof(s_alpha));
    for (int i = 0; i < s_oldRippleCount; ++i) markRing(y, s_oldRipple[i], false);
    for (int i = 0; i < count; ++i) markRing(y, waves[i], true);
}

static void collectBounds(SpanBounds &left, SpanBounds &right) {
    for (int x = 0; x < SCREEN_W; ++x) {
        if (!s_dirty[x]) continue;
        SpanBounds &bounds = x <= SCREEN_CX ? left : right;
        if (x < bounds.start) bounds.start = x;
        if (x > bounds.end) bounds.end = x;
    }
}

static bool alignBounds(SpanBounds &bounds) {
    if (!bounds.valid()) return false;
    bounds.start &= ~1;
    bounds.end = LV_MIN(SCREEN_W - 1, bounds.end | 1);
    return true;
}

static uint16_t blendRipplePixel(uint16_t base, uint16_t color565, uint8_t alpha) {
    if (!alpha) return base;
    const uint8_t invAlpha = 255 - alpha;
    const uint8_t red = (((color565 >> 11) * alpha + (base >> 11) * invAlpha) / 255) & 0x1F;
    const uint8_t green = ((((color565 >> 5) & 0x3F) * alpha + ((base >> 5) & 0x3F) * invAlpha) / 255) & 0x3F;
    const uint8_t blue = (((color565 & 0x1F) * alpha + (base & 0x1F) * invAlpha) / 255) & 0x1F;
    return (red << 11) | (green << 5) | blue;
}

static void writeSpanTile(int16_t y, int rows, const SpanBounds &bounds,
                          const display::RippleWave *waves, int count,
                          uint16_t color565, const uint16_t *baseFrame) {
    const int width = bounds.end - bounds.start + 1;
    for (int row = 0; row < rows; ++row) {
        markDirtyRow(y + row, waves, count);
        uint16_t *out = s_spanTile + row * width;
        const uint16_t *base = baseFrame + (y + row) * SCREEN_W + bounds.start;
        for (int x = 0; x < width; ++x) {
            out[x] = blendRipplePixel(base[x], color565, s_alpha[bounds.start + x]);
        }
    }
    s_gfx->writeAddrWindow(bounds.start, y, width, rows);
    s_gfx->writePixels(s_spanTile, (uint32_t)width * rows);
}

bool rippleOverlay(const RippleWave *waves, int count, uint32_t rgb) {
    if (!s_gfx || !rippleSnapshotReady() || !waves || count < 1 || count > 2) return false;
    // baseFrame is stored as RGB565 (uint16_t), matching the display color format.
    const uint16_t *bf = s_baseFrame;
    // Convert rgb (0xRRGGBB) to RGB565 for blending
    const uint8_t r = (rgb >> 16) & 0xFF;
    const uint8_t g = (rgb >> 8) & 0xFF;
    const uint8_t b = rgb & 0xFF;
    const uint16_t color565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    s_rippleRgb = rgb;
    const int16_t yFirst = LV_MAX(0, SCREEN_CY - RIPPLE_R_OUTER_PX - RIPPLE_GLOW_WIDTH_PX);
    const int16_t yLast  = LV_MIN(SCREEN_H - 1, SCREEN_CY + RIPPLE_R_OUTER_PX + RIPPLE_GLOW_WIDTH_PX);
    s_gfx->startWrite();
    for (int16_t y = yFirst; y <= yLast; y += RIPPLE_TILE_ROWS) {
        const int rows = LV_MIN(RIPPLE_TILE_ROWS, yLast - y + 1);
        SpanBounds left, right;
        for (int row = 0; row < rows; ++row) {
            markDirtyRow(y + row, waves, count);
            collectBounds(left, right);
        }
        const bool haveLeft = alignBounds(left);
        const bool haveRight = alignBounds(right);
        if (haveLeft && haveRight && left.end + 1 >= right.start) {
            left.end = right.end;
            writeSpanTile(y, rows, left, waves, count, color565, bf);
        } else {
            if (haveLeft) writeSpanTile(y, rows, left, waves, count, color565, bf);
            if (haveRight) writeSpanTile(y, rows, right, waves, count, color565, bf);
        }
    }
    s_gfx->endWrite();
    s_oldRippleCount = count;
    for (int i = 0; i < count; ++i) s_oldRipple[i] = waves[i];
    return true;
}

void clearRippleOverlay() { s_oldRippleCount = 0; }

uint32_t inactiveMs() {
    // lv_display_get_inactive_time returns a very large value before the first
    // indev read (no activity recorded yet). Cap it so the idle-dim logic doesn't
    // fire spuriously on boot.
    const uint32_t raw = lv_display_get_inactive_time(nullptr);
    const uint32_t up  = (uint32_t)millis();
    return (raw > up) ? 0 : raw;   // if inactive > uptime, treat as "just active"
}

} // namespace display
