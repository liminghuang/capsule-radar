#!/usr/bin/env python3
"""
PROTOTYPE — LVGL 8.4 → 9.x Migration Explorer
Question: What does the migration look like for Capsule Radar's critical paths?
           Which files change, how many lines, and what's the complexity?

Run:  python3 tools/proto_lvgl9_migration.py

Navigation: [←]/[→] or [j]/[k] to move, [q] quit.
DELETE THIS FILE once the verdict is captured in a commit/ADR.
"""

import sys
import tty
import termios
import textwrap

# ── ANSI helpers ─────────────────────────────────────────────────────────────
RESET  = "\x1b[0m"
BOLD   = "\x1b[1m"
DIM    = "\x1b[2m"
RED    = "\x1b[31m"
GREEN  = "\x1b[32m"
YELLOW = "\x1b[33m"
CYAN   = "\x1b[36m"
WHITE  = "\x1b[37m"
BG_RED    = "\x1b[41m"
BG_GREEN  = "\x1b[42m"
BG_YELLOW = "\x1b[43m"
BG_BLUE   = "\x1b[44m"

def clr(): print("\x1b[2J\x1b[H", end="")
def bold(s): return f"{BOLD}{s}{RESET}"
def dim(s): return f"{DIM}{s}{RESET}"
def red(s): return f"{RED}{s}{RESET}"
def green(s): return f"{GREEN}{s}{RESET}"
def yellow(s): return f"{YELLOW}{s}{RESET}"
def cyan(s): return f"{CYAN}{s}{RESET}"

COMPLEXITY_COLOR = {"easy": GREEN, "medium": YELLOW, "hard": RED}
COMPLEXITY_EMOJI = {"easy": "●", "medium": "●●", "hard": "●●●"}

def complexity_badge(level):
    c = COMPLEXITY_COLOR[level]
    e = COMPLEXITY_EMOJI[level]
    return f"{c}{BOLD}{e} {level.upper()}{RESET}"

# ── Migration items ──────────────────────────────────────────────────────────
ITEMS = [
    {
        "title": "Display driver init (display.cpp)",
        "files": ["src/display.cpp"],
        "lines_changed": 18,
        "complexity": "medium",
        "v8": """\
// Declarations
lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t      s_disp_drv;

// Init
lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, buf_px);

lv_disp_drv_init(&s_disp_drv);
s_disp_drv.hor_res  = SCREEN_W;
s_disp_drv.ver_res  = SCREEN_H;
s_disp_drv.flush_cb = flush_cb;
s_disp_drv.rounder_cb = rounder_cb;
s_disp_drv.draw_buf = &s_draw_buf;
lv_disp_drv_register(&s_disp_drv);""",
        "v9": """\
// No draw_buf struct, no drv struct needed
lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
lv_display_set_flush_cb(disp, flush_cb);
lv_display_set_buffers(disp, s_buf1, nullptr,
    buf_px * sizeof(lv_color_t),
    LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_PARTIAL);
// rounder_cb: still lv_display_set_flush_area_cb or override
// (check LVGL 9.x docs — may need custom approach)""",
        "note": "lv_disp_draw_buf_t and lv_disp_drv_t are GONE. New lv_display_t API is cleaner. rounder_cb equivalent needs verification.",
    },
    {
        "title": "flush_cb signature (display.cpp)",
        "files": ["src/display.cpp"],
        "lines_changed": 4,
        "complexity": "easy",
        "v8": """\
static void flush_cb(lv_disp_drv_t *drv,
                     const lv_area_t *area,
                     lv_color_t *px) {
    // ...
    if (lv_disp_flush_is_last(drv)) s_frameCount++;
    lv_disp_flush_ready(drv);
}""",
        "v9": """\
static void flush_cb(lv_display_t *disp,
                     const lv_area_t *area,
                     uint8_t *px_map) {  // note: uint8_t* not lv_color_t*
    lv_color_t *px = (lv_color_t *)px_map;
    // ...
    if (lv_display_flush_is_last(disp)) s_frameCount++;
    lv_display_flush_ready(disp);
}""",
        "note": "Parameter renamed, px_map is uint8_t* now. lv_disp_flush_ready → lv_display_flush_ready. Straightforward rename.",
    },
    {
        "title": "Touch input driver (display.cpp)",
        "files": ["src/display.cpp"],
        "lines_changed": 8,
        "complexity": "easy",
        "v8": """\
lv_indev_drv_t s_indev_drv;

static void touch_read_cb(lv_indev_drv_t *drv,
                           lv_indev_data_t *data) { ... }

lv_indev_drv_init(&s_indev_drv);
s_indev_drv.type = LV_INDEV_TYPE_POINTER;
s_indev_drv.read_cb = touch_read_cb;
lv_indev_drv_register(&s_indev_drv);""",
        "v9": """\
// No drv struct needed
static void touch_read_cb(lv_indev_t *indev,
                           lv_indev_data_t *data) { ... }  // param type changes

lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, touch_read_cb);""",
        "note": "lv_indev_drv_t is GONE. Callback first param changes to lv_indev_t*. Otherwise logic is identical.",
    },
    {
        "title": "inactive time / display ref (display.cpp)",
        "files": ["src/display.cpp"],
        "lines_changed": 1,
        "complexity": "easy",
        "v8": """\
uint32_t inactiveMs() {
    return lv_disp_get_inactive_time(NULL);
}""",
        "v9": """\
uint32_t inactiveMs() {
    return lv_display_get_inactive_time(NULL);
}""",
        "note": "Pure rename. lv_disp_* → lv_display_*",
    },
    {
        "title": "Draw event callback context (radar_view.cpp, airports.cpp, coastline.cpp)",
        "files": ["src/radar_view.cpp", "src/airports.cpp", "src/coastline.cpp"],
        "lines_changed": 15,
        "complexity": "medium",
        "v8": """\
// In every LV_EVENT_DRAW_MAIN handler:
lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);

// Then pass to draw functions:
airports_draw(dctx, color, opa);
coastline_draw(dctx, color, opa, 1);
lv_draw_arc(dctx, &dsc, &center, r, 0, 360);
lv_draw_line(dctx, &dsc, &p1, &p2);""",
        "v9": """\
// lv_draw_ctx_t is GONE, replaced by lv_layer_t*
lv_layer_t *layer = lv_event_get_layer(e);

// Then pass to draw functions:
airports_draw(layer, color, opa);
coastline_draw(layer, color, opa, 1);
lv_draw_arc(layer, &dsc);   // ← center/radius now IN dsc!
lv_draw_line(layer, &dsc, &p1, &p2);  // or similar""",
        "note": "lv_draw_ctx_t → lv_layer_t everywhere. Function signatures for airports_draw(), coastline_draw() all need updating.",
    },
    {
        "title": "lv_draw_arc() API (radar_view.cpp, airports.cpp, sweep_draw_cb)",
        "files": ["src/radar_view.cpp", "src/airports.cpp"],
        "lines_changed": 30,
        "complexity": "hard",
        "v8": """\
lv_draw_arc_dsc_t dsc;
lv_draw_arc_dsc_init(&dsc);
dsc.color = s_cLead;
dsc.width = RIPPLE_WIDTH;
dsc.opa   = coreOpa;
// center and radius are CALL-SITE arguments:
lv_draw_arc(dctx, &dsc, &center, radius, 0, 360);""",
        "v9": """\
lv_draw_arc_dsc_t dsc;
lv_draw_arc_dsc_init(&dsc);
dsc.color       = s_cLead;
dsc.width       = RIPPLE_WIDTH;
dsc.opa         = coreOpa;
// center and radius are now INSIDE dsc:
dsc.center.x    = s_cx;
dsc.center.y    = s_cy;
dsc.radius      = (uint32_t)radius;
dsc.start_angle = 0;
dsc.end_angle   = 360;
lv_draw_arc(layer, &dsc);  // no separate center/radius args""",
        "note": "Breaking: center and radius move from call args into dsc. Every lv_draw_arc() call in radar_view.cpp (ripple, scope rings, ORB waves) and airports.cpp needs updating.",
    },
    {
        "title": "lv_draw_line() API (radar_view.cpp, coastline.cpp)",
        "files": ["src/radar_view.cpp", "src/coastline.cpp"],
        "lines_changed": 20,
        "complexity": "easy",
        "v8": """\
lv_draw_line_dsc_t d;
lv_draw_line_dsc_init(&d);
d.color = color;
d.width = 1;
lv_draw_line(ctx, &d, &p1, &p2);""",
        "v9": """\
lv_draw_line_dsc_t d;
lv_draw_line_dsc_init(&d);
d.color = color;
d.width = 1;
d.p1 = p1;  // ← points now IN dsc (similar to arc)
d.p2 = p2;
lv_draw_line(layer, &d);""",
        "note": "Points move into dsc — same pattern as arc. ~50 lv_draw_line() calls in radar_view.cpp need updating.",
    },
    {
        "title": "Canvas API (radar_view.cpp — flow particles)",
        "files": ["src/radar_view.cpp"],
        "lines_changed": 10,
        "complexity": "medium",
        "v8": """\
lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);""",
        "v9": """\
// lv_canvas API largely unchanged in v9, but check:
// - lv_canvas_fill_bg → may still work
// - lv_canvas_draw_line → lv_canvas_draw_line_dsc changes?
// NEEDS TESTING against actual lvgl 9.x headers
lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);  // possibly same""",
        "note": "Canvas API is lower-priority — the flow animation is decorative. Verify against v9 headers before estimating.",
    },
    {
        "title": "lv_conf.h changes",
        "files": ["include/lv_conf.h"],
        "lines_changed": 5,
        "complexity": "easy",
        "v8": """\
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0""",
        "v9": """\
// v9 uses the same macros but may need:
#define LV_COLOR_DEPTH 16
// LV_COLOR_16_SWAP removed or renamed in 9.x
// Check new rendering pipeline flags
// (LV_DRAW_SW_COMPLEX etc.)""",
        "note": "lv_conf.h needs a fresh v9 template — copy from LVGL 9.x examples and re-apply our custom values.",
    },
]

SUMMARY = {
    "total_lines": sum(i["lines_changed"] for i in ITEMS),
    "files": sorted(set(f for i in ITEMS for f in i["files"])),
    "by_complexity": {
        "easy": sum(1 for i in ITEMS if i["complexity"] == "easy"),
        "medium": sum(1 for i in ITEMS if i["complexity"] == "medium"),
        "hard": sum(1 for i in ITEMS if i["complexity"] == "hard"),
    },
    "verdict": None,  # fill in after exploring
}


def render_item(idx: int, total: int, item: dict):
    clr()
    # Header
    print(f"{BOLD}{CYAN}LVGL 8.4 → 9.x Migration Prototype{RESET}")
    print(f"{DIM}Question: What's the scope and complexity of the Capsule Radar upgrade?{RESET}")
    print("─" * 72)
    print(f"\n  {bold(f'[{idx+1}/{total}]')}  {bold(item['title'])}")
    print(f"  Files: {dim(', '.join(item['files']))}   Lines: {dim(str(item['lines_changed']))}   {complexity_badge(item['complexity'])}")
    print()

    # Code columns
    w = 34
    print(f"  {BOLD}{RED}▼ v8 (current){RESET}{'':>{w-18}}{BOLD}{GREEN}▼ v9 (target){RESET}")
    v8_lines  = item["v8"].splitlines()
    v9_lines  = item["v9"].splitlines()
    max_rows  = max(len(v8_lines), len(v9_lines))
    for r in range(max_rows):
        l8 = v8_lines[r] if r < len(v8_lines) else ""
        l9 = v9_lines[r] if r < len(v9_lines) else ""
        # truncate + pad
        l8 = (l8[:w-1] + "…") if len(l8) > w else l8
        l9 = (l9[:w-1] + "…") if len(l9) > w else l9
        print(f"  {RED}{l8:<{w}}{RESET}  {GREEN}{l9}{RESET}")

    print()
    # Note
    note_wrapped = textwrap.wrap(item["note"], width=68)
    print(f"  {DIM}Note:{RESET}")
    for line in note_wrapped:
        print(f"    {dim(line)}")

    # Nav bar
    print("\n" + "─" * 72)
    print(f"  {bold('[←]')} prev   {bold('[→]')} next   {bold('[s]')} summary   {bold('[q]')} quit")


def render_summary():
    clr()
    print(f"{BOLD}{CYAN}LVGL 8.4 → 9.x Migration — Capsule Radar Summary{RESET}")
    print(f"{DIM}Question: What's the scope and complexity of the upgrade?{RESET}")
    print("─" * 72)
    print()
    print(f"  {bold('Total estimated lines changed:')} {SUMMARY['total_lines']}")
    print()
    print(f"  {bold('Files affected:')}")
    for f in SUMMARY["files"]:
        print(f"    {dim(f)}")
    print()
    print(f"  {bold('Complexity breakdown:')}")
    for level in ("easy", "medium", "hard"):
        count = SUMMARY["by_complexity"][level]
        bar = "█" * count
        print(f"    {complexity_badge(level):<40}  {bar} ({count} items)")
    print()
    print("─" * 72)
    print(f"  {bold('Key risks:')}")
    risks = [
        "lv_draw_arc(): center/radius move into dsc — every arc call changes",
        "lv_draw_line(): points move into dsc — ~50 calls in radar_view.cpp",
        "lv_draw_ctx_t → lv_layer_t — affects airports.cpp + coastline.cpp API",
        "rounder_cb equivalent in v9 needs verification (CO5300 alignment fix)",
        "lv_conf.h needs fresh v9 template — old config may not compile",
    ]
    for r in risks:
        wrapped = textwrap.wrap(r, width=65)
        print(f"    {RED}•{RESET} {wrapped[0]}")
        for w in wrapped[1:]:
            print(f"      {w}")
    print()
    print(f"  {bold('Estimated effort:')} 2-3 days of mechanical refactoring")
    print(f"  {bold('Expected FPS gain:')} 1.5–3× (new rendering pipeline + better partial refresh)")
    print()
    print(f"  {DIM}Verdict (fill in after exploring):{RESET}")
    if SUMMARY["verdict"]:
        print(f"    {bold(SUMMARY['verdict'])}")
    else:
        print(f"    {dim('— not yet captured —')}")
    print()
    print("─" * 72)
    print(f"  {bold('[←]')} back to items   {bold('[q]')} quit")


def getch():
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        ch = sys.stdin.read(1)
        if ch == "\x1b":
            ch2 = sys.stdin.read(2)
            if ch2 == "[D": return "left"
            if ch2 == "[C": return "right"
        return ch
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def main():
    idx = 0
    total = len(ITEMS)
    view = "item"  # "item" | "summary"

    while True:
        if view == "summary":
            render_summary()
        else:
            render_item(idx, total, ITEMS[idx])

        key = getch()

        if key in ("q", "\x03"):
            clr()
            print(f"{bold('PROTOTYPE NOTE:')} Capture the verdict before deleting this file.")
            print(f"  tools/proto_lvgl9_migration.py → upgrade or defer?")
            print()
            break

        if view == "summary":
            if key in ("left", "h", "j", "k"):
                view = "item"
        else:
            if key in ("right", "l", "\r", " "):
                idx = min(idx + 1, total - 1)
            elif key in ("left", "h"):
                idx = max(idx - 1, 0)
            elif key == "s":
                view = "summary"


if __name__ == "__main__":
    main()
