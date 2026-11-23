// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — every screen and every key press. No hardware and no
 * threads here: the model is plain data, wt_ui_draw() draws it on a Canvas
 * and wt_ui_input() turns a key into the next screen and, sometimes, a
 * command for the app to carry out. The host tests draw the same screens with
 * the device's fonts (tests/support/) and check that nothing is cut off.
 *
 * The screen is 128 x 64. Most screens are a 12 px title bar, a body, and a
 * line of key hints along the bottom.
 */
#include <gui/canvas.h>
#include "wt_link.h"
#include "wt_icons.h"
#include "wt_cars.h"

typedef enum {
    WT_SCR_HOME = 0,
    WT_SCR_SCAN_RUN, /* also step 1 of the ship flow */
    WT_SCR_SCAN_RESULT,
    WT_SCR_SCAN_DETAIL,
    WT_SCR_SCAN_FAIL,
    WT_SCR_WHERE, /* which group of places */
    WT_SCR_ASSIGN, /* which wheel of a set */
    WT_SCR_SPARE_PICK, /* which spare-sensor slot */
    WT_SCR_SHIP_INTRO,
    WT_SCR_SHIP_ALREADY,
    WT_SCR_SHIP_CONFIRM,
    WT_SCR_SHIP_RUN,
    WT_SCR_SHIP_RESULT,
    WT_SCR_MONITOR,
    WT_SCR_MONITOR_LIST,
    WT_SCR_WHEELS, /* "My wheels": the sets and the spares */
    WT_SCR_SETTINGS,
    WT_SCR_FAULT,
    /* Added after 0.4.0, at the end: a paired phone is told the screen by number. */
    WT_SCR_CARS, /* "Which cars?": the makes expected to carry this sensor */
    WT_SCR_CAR, /* one make: years and models */
    /* Added after 0.4.1, at the end again. */
    WT_SCR_DRAGONS, /* the warning in front of the Developer menu */
    WT_SCR_DEV, /* Developer: the phone link and the lab tools */
} WtScreen;

typedef enum { WT_KEY_UP = 0, WT_KEY_DOWN, WT_KEY_LEFT, WT_KEY_RIGHT, WT_KEY_OK, WT_KEY_BACK } WtKey;

typedef enum {
    WT_CMD_NONE = 0,
    WT_CMD_SCAN, /* run wt_engine_scan; if ui->scan_new, ignoring wt_ui_skip_list() */
    WT_CMD_SHIP, /* run wt_engine_ship(before = ui->reading) */
    WT_CMD_MONITOR,
    WT_CMD_CANCEL, /* stop whatever is running */
    WT_CMD_EXIT,
    WT_CMD_LAUNCH, /* start the lab tool wt_dev_tools[ui->launch], then leave */
} WtCmd;

enum { WT_MENU_SCAN = 0, WT_MENU_SHIP, WT_MENU_MONITOR, WT_MENU_WHEELS, WT_MENU_SETTINGS, WT_MENU_COUNT };
enum {
    WT_SET_PRESSURE = 0,
    WT_SET_TEMP,
    WT_SET_ON_CAR,
    WT_SET_ASK,
    WT_SET_BAND,
    WT_SET_CARS, /* not a setting: opens "Which cars?" */
    WT_SET_FORGET,
    WT_SET_DEV, /* not a setting: the way to the Developer menu, last on purpose */
    WT_SET_COUNT
};
/* ----- Developer: what a driver fixing a sensor does not need -------------------
 * The phone link, and the lab tools this app grew out of. The tools are apps
 * of their own, kept out of the Flipper's Apps menu in this app's folder on
 * the SD card (apps_data/<id>/dev/); this app only starts them. */
typedef struct {
    const char* name;
    const char* fap; /* file name in the dev folder */
    const char* hint;
} WtDevTool;
static const WtDevTool wt_dev_tools[] = {
    {"LF Lab", "continental_tg1c_lf.fap", "Test frames to a sensor"},
    {"Status Watch", "tpms_watch.fap", "Listens and logs only"},
    {"Tool Recorder", "tpms_tool_recorder.fap", "Records another tool's LF"},
};
#define WT_DEV_TOOLS ((uint8_t)(sizeof(wt_dev_tools) / sizeof(wt_dev_tools[0])))
#define WT_DEV_PHONE_ROWS (WT_PHONE_LINK ? 1u : 0u) /* "Phone link" is the first row, in a build that has it */
#define WT_DEV_COUNT (WT_DEV_PHONE_ROWS + WT_DEV_TOOLS)
#define WT_CAR_ROWS 4u /* lines of one make on the screen at once */
/* Rows of the "Where is it?" list: one per set, then these. */
enum { WT_WHERE_TYRE = WT_SETS, WT_WHERE_SPARE, WT_WHERE_NONE, WT_WHERE_COUNT };
#define WT_WHEELS_PAGES (WT_SETS + 1u) /* one per set, then the spares */
#define WT_TOAST_TICKS 8u /* 2 s at 4 ticks a second */
#define WT_LIST_ROWS 4u
#define WT_REJECT_MAX 8u /* sensors answered "Not it" in one run of scans */

typedef struct {
    uint8_t screen; /* WtScreen */
    uint8_t menu, set_row, wheel_pick, return_to, list_sel;
    uint8_t where_sel, assign_set, spare_pick, wheels_page;
    uint8_t car_sel, car_line; /* "Which cars?": the make, and the first line shown of it */
    uint8_t dev_row, launch; /* Developer: the row, and the tool WT_CMD_LAUNCH asks for */
    bool wheels_ids; /* "My wheels" shows sensor IDs in place of readings */
    bool dirty; /* cfg has changed: the app writes it to storage and clears this */
    bool mon_touched; /* the monitor has fresher readings than the settings file */
    WtDate today; /* set by the app before every event */
    bool ship_flow; /* the scan is step 1 of "Exit ship mode" */
    bool scan_new; /* the scan ignores sensors that have a place, and those in rejected[] */
    uint8_t nrejected;
    uint32_t rejected[WT_REJECT_MAX];
    uint32_t op_seq; /* number of the operation in progress; older results are dropped */
    uint32_t anim; /* counts 4 times a second */
    uint8_t toast_ticks;
    char toast[20];
    WtConfig cfg;
    WtProgress prog;
    WtReading reading; /* the last scan */
    WtShip ship;
    uint32_t assign_id;
    char fault[28];
    WtSensor sensors[WT_MAX_SENSORS];
    uint8_t nsensors;
    uint32_t now; /* tick of the monitor snapshot */
    uint8_t link; /* WtLinkState: the phone link, while the monitor runs */
} WtUi;

static inline void wt_ui_init(WtUi* ui) {
    memset(ui, 0, sizeof(*ui));
}

/* ===== small drawing helpers ================================================= */

static inline void wt_text(Canvas* c, Font font, int32_t x, int32_t y, const char* s) {
    canvas_set_font(c, font);
    canvas_draw_str(c, x, y, s);
}
static inline void wt_text_right(Canvas* c, Font font, int32_t right, int32_t y, const char* s) {
    canvas_set_font(c, font);
    canvas_draw_str(c, right - (int32_t)canvas_string_width(c, s), y, s);
}
static inline void wt_text_centre(Canvas* c, Font font, int32_t cx, int32_t y, const char* s) {
    canvas_set_font(c, font);
    canvas_draw_str(c, cx - (int32_t)canvas_string_width(c, s) / 2, y, s);
}
/* Title bar: white text on black, 12 px high. */
static inline void wt_title(Canvas* c, const char* title) {
    canvas_set_color(c, ColorBlack);
    canvas_draw_box(c, 0, 0, 128, 12);
    canvas_set_color(c, ColorWhite);
    wt_text(c, FontPrimary, 3, 10, title);
    canvas_set_color(c, ColorBlack);
}
/* Text in a filled rounded box. Returns the box width. */
static inline int32_t wt_badge(Canvas* c, int32_t x, int32_t y, const char* s, bool filled) {
    canvas_set_font(c, FontPrimary);
    int32_t w = (int32_t)canvas_string_width(c, s) + 8;
    canvas_set_color(c, ColorBlack);
    if(filled) {
        canvas_draw_rbox(c, x, y, (size_t)w, 12, 2);
        canvas_set_color(c, ColorWhite);
    } else {
        canvas_draw_rframe(c, x, y, (size_t)w, 12, 2);
    }
    canvas_draw_str(c, x + 4, y + 10, s);
    canvas_set_color(c, ColorBlack);
    return w;
}
/* One row of a list, 10 px high: a label, a value on the right, filled when
 * it is the selected one. */
static inline void wt_row(Canvas* c, int32_t y, bool sel, int32_t x, const char* left, const char* right) {
    if(sel) canvas_draw_rbox(c, 1, y, 126, 10, 2);
    canvas_set_color(c, sel ? ColorWhite : ColorBlack);
    wt_text(c, FontPrimary, x, y + 8, left);
    if(right) wt_text_right(c, FontSecondary, 123, y + 8, right);
    canvas_set_color(c, ColorBlack);
}
/* One key hint: a key picture and a word. Returns the x after it. */
typedef enum { WT_HINT_OK = 0, WT_HINT_RIGHT, WT_HINT_LEFT, WT_HINT_BACK } WtHint;
static inline int32_t wt_hint(Canvas* c, int32_t x, WtHint key, const char* word) {
    const int32_t y = 55;
    if(key == WT_HINT_OK)
        canvas_draw_xbm(c, x, y, WT_ICON_KEY_OK_W, WT_ICON_KEY_OK_H, wt_icon_key_ok);
    else if(key == WT_HINT_RIGHT)
        canvas_draw_xbm(c, x + 1, y, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
    else if(key == WT_HINT_LEFT)
        canvas_draw_xbm(c, x + 1, y, WT_ICON_KEY_LEFT_W, WT_ICON_KEY_LEFT_H, wt_icon_key_left);
    else
        canvas_draw_xbm(c, x, y, WT_ICON_KEY_BACK_W, WT_ICON_KEY_BACK_H, wt_icon_key_back);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, x + 10, 62, word);
    return x + 10 + (int32_t)canvas_string_width(c, word) + 8;
}
/* Signal strength, four bars, 13 x 8 px with its bottom-left corner at (x, y). */
static inline void wt_bars_draw(Canvas* c, int32_t x, int32_t y, uint8_t bars) {
    for(uint8_t i = 0; i < 4; i++) {
        int32_t h = 2 + 2 * i;
        if(i < bars)
            canvas_draw_box(c, x + 3 * i, y - h, 2, (size_t)h);
        else
            canvas_draw_box(c, x + 3 * i, y - 1, 2, 1);
    }
}
/* "2", a degree ring and the unit letter, starting at x. Returns the x after it. */
static inline int32_t wt_temp_draw(Canvas* c, Font font, int32_t x, int32_t y, uint8_t b6, uint8_t tunit) {
    char num[8];
    snprintf(num, sizeof(num), "%d", wt_temp(b6, tunit));
    canvas_set_font(c, font);
    canvas_draw_str(c, x, y, num);
    x += (int32_t)canvas_string_width(c, num) + 1;
    int32_t top = y - (font == FontPrimary ? 8 : 7);
    canvas_draw_dot(c, x + 1, top);
    canvas_draw_dot(c, x, top + 1);
    canvas_draw_dot(c, x + 2, top + 1);
    canvas_draw_dot(c, x + 1, top + 2);
    x += 4;
    canvas_draw_str(c, x, y, wt_tunit_name(tunit));
    return x + (int32_t)canvas_string_width(c, wt_tunit_name(tunit));
}
static inline void wt_age_text(char* out, size_t cap, uint32_t age_ms) {
    uint32_t s = age_ms / 1000u;
    if(s < 3u)
        snprintf(out, cap, "now");
    else if(s < 100u)
        snprintf(out, cap, "%lus", (unsigned long)s);
    else if(s < 6000u)
        snprintf(out, cap, "%lum", (unsigned long)(s / 60u));
    else
        snprintf(out, cap, ">1h");
}
/* A progress bar: frame w x 5 at (x, y). */
static inline void wt_progress_bar(Canvas* c, int32_t x, int32_t y, int32_t w, uint32_t done, uint32_t total) {
    canvas_draw_rframe(c, x, y, (size_t)w, 5, 1);
    if(!total) return;
    if(done > total) done = total;
    int32_t fill = (int32_t)((uint64_t)(w - 2) * done / total);
    if(fill > 0) canvas_draw_box(c, x + 1, y + 1, (size_t)fill, 3);
}
/* A wheel seen from the side, 27 px across, with rings that travel outwards
 * while the coil is sending. */
static inline void wt_wheel_draw(Canvas* c, int32_t cx, int32_t cy, uint32_t anim, bool waves) {
    canvas_draw_circle(c, cx, cy, 13);
    canvas_draw_circle(c, cx, cy, 12);
    canvas_draw_circle(c, cx, cy, 11);
    canvas_draw_circle(c, cx, cy, 6);
    canvas_draw_disc(c, cx, cy, 2);
    /* spokes */
    for(int i = 0; i < 4; i++) {
        static const int8_t dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        canvas_draw_line(c, cx + dx[i] * 3, cy + dy[i] * 3, cx + dx[i] * 5, cy + dy[i] * 5);
    }
    /* the valve */
    canvas_draw_box(c, cx + 8, cy + 8, 2, 2);
    if(waves) {
        /* an arc of dots on the side facing the Flipper, one of three radii in turn */
        static const int8_t ax[7] = {100, 98, 94, 87, 77, 64, 50}, ay[7] = {0, 17, 34, 50, 64, 77, 87};
        int32_t r = 16 + 3 * (int32_t)(anim % 3u);
        for(int i = 0; i < 7; i++) {
            int32_t x = cx + (r * ax[i] + 50) / 100, y = (r * ay[i] + 50) / 100;
            canvas_draw_dot(c, x, cy - y);
            canvas_draw_dot(c, x, cy + y);
        }
    }
}
/* A car from above, 22 x 46 with its top-left corner at (x, y); wheels stick
 * out 3 px each side. hl: wheel to fill, or WT_NO_WHEEL. */
#define WT_CAR_W 22
#define WT_CAR_H 46
static inline void wt_wheel_box(Canvas* c, int32_t x, int32_t y, bool on) {
    if(on)
        canvas_draw_box(c, x, y, 4, 10);
    else
        canvas_draw_frame(c, x, y, 4, 10);
}
static inline void wt_car_draw(Canvas* c, int32_t x, int32_t y, uint8_t hl, bool blink) {
    canvas_draw_rframe(c, x, y, WT_CAR_W, WT_CAR_H, 6);
    /* windscreen, roof, rear window */
    canvas_draw_line(c, x + 4, y + 13, x + WT_CAR_W - 5, y + 13);
    canvas_draw_line(c, x + 3, y + 9, x + 4, y + 13);
    canvas_draw_line(c, x + WT_CAR_W - 4, y + 9, x + WT_CAR_W - 5, y + 13);
    canvas_draw_line(c, x + 3, y + 9, x + WT_CAR_W - 4, y + 9);
    canvas_draw_line(c, x + 4, y + 33, x + WT_CAR_W - 5, y + 33);
    canvas_draw_line(c, x + 5, y + 37, x + WT_CAR_W - 6, y + 37);
    canvas_draw_line(c, x + 4, y + 33, x + 5, y + 37);
    canvas_draw_line(c, x + WT_CAR_W - 5, y + 33, x + WT_CAR_W - 6, y + 37);
    static const int8_t wx[WT_WHEELS] = {-3, WT_CAR_W - 1, -3, WT_CAR_W - 1};
    static const int8_t wy[WT_WHEELS] = {5, 5, 31, 31};
    for(uint8_t w = 0; w < WT_WHEELS; w++) {
        canvas_set_color(c, ColorWhite);
        canvas_draw_box(c, x + wx[w], y + wy[w], 4, 10);
        canvas_set_color(c, ColorBlack);
        wt_wheel_box(c, x + wx[w], y + wy[w], w == hl && !blink);
    }
}
/* Three numbered steps joined by a line, pitch px apart, the current one
 * filled and the ones before it ticked. (x, y) is the centre of the first. */
static inline void wt_stepper(Canvas* c, int32_t x, int32_t y, uint8_t step, int32_t pitch) {
    canvas_draw_line(c, x, y, x + 2 * pitch, y);
    for(uint8_t i = 0; i < 3; i++) {
        int32_t cx = x + pitch * i;
        canvas_set_color(c, ColorWhite);
        canvas_draw_disc(c, cx, y, 5);
        canvas_set_color(c, ColorBlack);
        char n[2] = {(char)('1' + i), 0};
        if(i + 1 == step) {
            canvas_draw_disc(c, cx, y, 5);
            canvas_set_color(c, ColorWhite);
        } else {
            canvas_draw_circle(c, cx, y, 5);
        }
        if(i + 1 < step) { /* done: a small tick in place of the number */
            canvas_draw_line(c, cx - 3, y, cx - 1, y + 2);
            canvas_draw_line(c, cx - 1, y + 2, cx + 3, y - 2);
        } else {
            canvas_set_font(c, FontSecondary);
            canvas_draw_str(c, cx - 2, y + 4, n);
        }
        canvas_set_color(c, ColorBlack);
    }
}
static inline void wt_time_text(char* out, size_t cap, uint32_t ms) {
    snprintf(out, cap, "%lu:%02lu", (unsigned long)(ms / 60000u), (unsigned long)(ms / 1000u % 60u));
}

/* The sensors a "Not it" scan ignores: every one that has a place, and every
 * one the user has said "Not it" to since the scan was started from the menu. */
static inline uint8_t wt_ui_skip_list(const WtUi* ui, uint32_t out[WT_SKIP_MAX]) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < WT_SLOTS; i++)
        if(ui->cfg.slot[i].id) out[n++] = ui->cfg.slot[i].id;
    for(uint8_t i = 0; i < ui->nrejected && n < WT_SKIP_MAX; i++) {
        bool have = false;
        for(uint8_t k = 0; k < n; k++) have |= out[k] == ui->rejected[i];
        if(!have) out[n++] = ui->rejected[i];
    }
    return n;
}

/* ===== screens ============================================================== */

static inline void wt_draw_home(Canvas* c, const WtUi* ui) {
    static const char* const label[WT_MENU_COUNT] = {"Scan wheel", "Exit ship mode", "Live monitor", "My wheels", "Settings"};
    static const uint8_t* const icon[WT_MENU_COUNT] = {
        wt_icon_menu_scan, wt_icon_menu_ship, wt_icon_menu_monitor, wt_icon_menu_wheels, wt_icon_menu_settings};
    wt_title(c, WT_APP_NAME);
    canvas_set_color(c, ColorWhite);
    wt_text_right(c, FontSecondary, 125, 9, "v" WT_VERSION);
    canvas_set_color(c, ColorBlack);
    for(uint8_t i = 0; i < WT_MENU_COUNT; i++) {
        int32_t y = 13 + 10 * i;
        bool sel = i == ui->menu;
        wt_row(c, y, sel, 19, label[i], NULL);
        canvas_set_color(c, sel ? ColorWhite : ColorBlack);
        canvas_draw_xbm(c, 5, y, 9, 9, icon[i]);
        if(sel) canvas_draw_xbm(c, 118, y + 1, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
        canvas_set_color(c, ColorBlack);
    }
}

static inline void wt_draw_scan_run(Canvas* c, const WtUi* ui) {
    char t[16];
    if(ui->ship_flow) {
        wt_title(c, "Exit ship mode");
        canvas_set_color(c, ColorWhite);
        wt_text_right(c, FontSecondary, 125, 9, "1 Read");
        canvas_set_color(c, ColorBlack);
    } else {
        wt_title(c, ui->scan_new ? "Scan: another sensor" : "Scan wheel");
    }
    wt_wheel_draw(c, 20, 34, ui->anim, ui->prog.lf_on && !ui->prog.heard);
    if(ui->prog.heard) {
        wt_text(c, FontPrimary, 48, 24, "Sensor found");
        wt_text(c, FontSecondary, 48, 35, "Reading it...");
    } else {
        wt_text(c, FontPrimary, 48, 24, "Searching...");
        wt_text(c, FontSecondary, 48, 34, "Hold Flipper's back");
        wt_text(c, FontSecondary, 48, 43, "on the tyre valve.");
    }
    wt_progress_bar(c, 48, 47, 76, ui->prog.elapsed_ms, ui->prog.limit_ms);
    wt_time_text(t, sizeof(t), ui->prog.elapsed_ms);
    wt_text(c, FontSecondary, 48, 62, t);
    if(ui->scan_new && !ui->ship_flow) {
        uint32_t skip[WT_SKIP_MAX];
        char note[20];
        snprintf(note, sizeof(note), "ignoring %u", wt_ui_skip_list(ui, skip));
        wt_text_right(c, FontSecondary, 125, 62, note);
    } else {
        wt_text_right(c, FontSecondary, 125, 62, "up to 1 min");
    }
}

static inline void wt_draw_reading_big(Canvas* c, const WtUi* ui, const WtReading* r, int32_t y) {
    char p[12];
    wt_fmt_pressure(p, sizeof(p), r->raw[5], ui->cfg.punit);
    canvas_set_font(c, FontBigNumbers);
    canvas_draw_str(c, 3, y, p);
    int32_t x = 3 + (int32_t)canvas_string_width(c, p) + 3;
    wt_text(c, FontPrimary, x, y, wt_punit_name(ui->cfg.punit));
    /* Temperature, right-aligned: "2", the degree ring, the unit letter. */
    char num[8];
    snprintf(num, sizeof(num), "%d", wt_temp(r->raw[6], ui->cfg.tunit));
    canvas_set_font(c, FontPrimary);
    int32_t tw = (int32_t)canvas_string_width(c, num) + 5 + (int32_t)canvas_string_width(c, wt_tunit_name(ui->cfg.tunit));
    canvas_draw_xbm(c, 125 - tw - 8, y - 12, WT_ICON_THERMO_W, WT_ICON_THERMO_H, wt_icon_thermo);
    wt_temp_draw(c, FontPrimary, 125 - tw, y - 4, r->raw[6], ui->cfg.tunit);
}

static inline void wt_draw_scan_result(Canvas* c, const WtUi* ui) {
    const WtReading* r = &ui->reading;
    char id[24], place[8];
    uint8_t slot = wt_config_slot_of(&ui->cfg, r->id);
    WtMode mode = wt_mode(wt_b4(r));
    wt_slot_short(place, sizeof(place), slot);
    if(slot != WT_NO_SLOT)
        snprintf(id, sizeof(id), "%s %08lX", place, (unsigned long)r->id);
    else
        snprintf(id, sizeof(id), "ID %08lX", (unsigned long)r->id);
    wt_title(c, id);
    canvas_set_color(c, ColorWhite);
    wt_bars_draw(c, 112, 10, wt_bars(r->rssi_dbm));
    canvas_set_color(c, ColorBlack);
    wt_draw_reading_big(c, ui, r, 31);
    /* The mode in words. A sensor in ship mode blinks. */
    char label[16];
    wt_mode_label(label, sizeof(label), wt_b4(r));
    bool filled = mode != WT_MODE_SHIP || (ui->anim & 2u) == 0;
    int32_t bw = wt_badge(c, 3, 36, label, filled);
    wt_text(
        c,
        FontSecondary,
        3 + bw + 4,
        45,
        mode == WT_MODE_PARK  ? "at rest" :
        mode == WT_MODE_DRIVE ? "moving" :
        mode == WT_MODE_SHIP  ? "asleep" :
        mode == WT_MODE_BUSY  ? "scan again" :
                                "not known");
    if(ui->toast_ticks) {
        wt_text(c, FontSecondary, 3, 62, ui->toast);
    } else {
        /* Left: this is a neighbour's sensor, look for another one. */
        int32_t x = wt_hint(c, 3, WT_HINT_OK, "Place");
        x = wt_hint(c, x - 2, WT_HINT_LEFT, "Not it");
        wt_hint(c, x - 2, WT_HINT_RIGHT, "More");
    }
}

static inline void wt_draw_scan_detail(Canvas* c, const WtUi* ui) {
    const WtReading* r = &ui->reading;
    char line[40], bits[9];
    wt_title(c, "Sensor details");
    for(int i = 0; i < 8; i++) bits[i] = (wt_b4(r) & (0x80u >> i)) ? '1' : '0';
    bits[8] = 0;
    char place[8];
    wt_slot_short(place, sizeof(place), wt_config_slot_of(&ui->cfg, r->id));
    snprintf(line, sizeof(line), "ID %08lX   place %s", (unsigned long)r->id, place);
    wt_text(c, FontSecondary, 3, 21, line);
    snprintf(line, sizeof(line), "Status %02X/%02X  %s", wt_b4(r), wt_b7(r), bits);
    wt_text(c, FontSecondary, 3, 30, line);
    snprintf(
        line,
        sizeof(line),
        "%lu.%lu kPa  %d C  batt %s",
        (unsigned long)(wt_kpa_x10(r->raw[5]) / 10u),
        (unsigned long)(wt_kpa_x10(r->raw[5]) % 10u),
        wt_temp(r->raw[6], WT_T_C),
        wt_batt_name(wt_batt(wt_b4(r))));
    wt_text(c, FontSecondary, 3, 39, line);
    snprintf(
        line,
        sizeof(line),
        "%d dBm  %u fr  after %lu.%lu s",
        r->rssi_dbm,
        r->frames > 999 ? 999 : r->frames,
        (unsigned long)(r->answer_ms / 1000u),
        (unsigned long)(r->answer_ms / 100u % 10u));
    wt_text(c, FontSecondary, 3, 48, line);
    char* p = line;
    for(int i = 0; i < WT_FRAME_LEN; i++) p += snprintf(p, 4, "%02X", r->raw[i]);
    wt_text(c, FontKeyboard, 3, 60, line);
}

static inline void wt_draw_scan_fail(Canvas* c, const WtUi* ui) {
    if(ui->scan_new && !ui->ship_flow) {
        /* Nothing but the sensors being ignored answered in 84 s. */
        wt_title(c, "Scan: another sensor");
        wt_text(c, FontPrimary, 3, 23, "No other sensor heard");
        wt_text(c, FontSecondary, 3, 33, "Known sensors were ignored.");
        wt_text(c, FontSecondary, 3, 42, "This one may be flat, missing");
        wt_text(c, FontSecondary, 3, 51, "or out of reach.");
        wt_hint(c, 3, WT_HINT_OK, "Try again");
        return;
    }
    wt_title(c, ui->ship_flow ? "Exit ship mode" : "Scan wheel");
    wt_text(c, FontPrimary, 3, 23, "No sensor heard");
    wt_text(c, FontSecondary, 3, 33, "Put the Flipper's back flat on");
    wt_text(c, FontSecondary, 3, 42, "the tyre, right at the valve,");
    wt_text(c, FontSecondary, 3, 51, "and try again.");
    wt_hint(c, 3, WT_HINT_OK, "Try again");
}

/* Which row of the "Where is it?" list a place belongs to. */
static inline uint8_t wt_where_row_of(uint8_t slot) {
    return slot < WT_SLOT_TYRE ? wt_slot_set(slot) : slot == WT_SLOT_TYRE ? WT_WHERE_TYRE : slot < WT_SLOTS ? WT_WHERE_SPARE : WT_WHERE_NONE;
}
/* "No place" is offered only to a sensor that has one. */
static inline uint8_t wt_where_rows(const WtUi* ui) {
    return wt_config_slot_of(&ui->cfg, ui->assign_id) != WT_NO_SLOT ? WT_WHERE_COUNT : WT_WHERE_NONE;
}
static inline void wt_draw_where(Canvas* c, const WtUi* ui) {
    char left[20], right[16];
    snprintf(left, sizeof(left), "Where is %08lX?", (unsigned long)ui->assign_id);
    wt_title(c, left);
    uint8_t rows = wt_where_rows(ui), here = wt_where_row_of(wt_config_slot_of(&ui->cfg, ui->assign_id));
    uint8_t first = ui->where_sel >= WT_LIST_ROWS ? (uint8_t)(ui->where_sel - WT_LIST_ROWS + 1u) : 0;
    for(uint8_t row = 0; row < WT_LIST_ROWS && first + row < rows; row++) {
        uint8_t i = (uint8_t)(first + row);
        int32_t y = 13 + 10 * row;
        if(i < WT_SETS) {
            snprintf(left, sizeof(left), "%s set", wt_set_name(i));
            snprintf(right, sizeof(right), "%s%u/4", i == ui->cfg.on_car ? "on car  " : "", wt_config_set_count(&ui->cfg, i));
        } else if(i == WT_WHERE_TYRE) {
            snprintf(left, sizeof(left), "Spare tyre");
            snprintf(right, sizeof(right), "%s", ui->cfg.slot[WT_SLOT_TYRE].id ? "in use" : "empty");
        } else if(i == WT_WHERE_SPARE) {
            snprintf(left, sizeof(left), "Spare sensor");
            snprintf(right, sizeof(right), "%u/%u", wt_config_spare_count(&ui->cfg), (unsigned)WT_SPARES);
        } else {
            snprintf(left, sizeof(left), "No place");
            right[0] = 0;
        }
        wt_row(c, y, i == ui->where_sel, 12, left, right);
        if(i == here) { /* a dot marks where it is now */
            canvas_set_color(c, i == ui->where_sel ? ColorWhite : ColorBlack);
            canvas_draw_xbm(c, 5, y + 3, WT_ICON_DOT_W, WT_ICON_DOT_H, wt_icon_dot);
            canvas_set_color(c, ColorBlack);
        }
    }
    int32_t x = wt_hint(c, 3, WT_HINT_OK, "Choose");
    wt_hint(c, x, WT_HINT_BACK, "Skip");
}

static inline void wt_draw_assign(Canvas* c, const WtUi* ui) {
    char line[24];
    snprintf(line, sizeof(line), "%s: which wheel?", wt_set_name(ui->assign_set));
    wt_title(c, line);
    wt_car_draw(c, 12, 15, ui->wheel_pick, (ui->anim & 1u) != 0);
    wt_text(c, FontPrimary, 50, 25, wt_wheel_long(ui->wheel_pick));
    snprintf(line, sizeof(line), "Sensor %08lX", (unsigned long)ui->assign_id);
    wt_text(c, FontSecondary, 50, 35, line);
    uint32_t there = ui->cfg.slot[wt_slot(ui->assign_set, ui->wheel_pick)].id;
    if(there && there != ui->assign_id) {
        snprintf(line, sizeof(line), "was %08lX", (unsigned long)there);
        wt_text(c, FontSecondary, 50, 44, line);
    } else {
        wt_text(c, FontSecondary, 50, 44, "Arrows choose.");
    }
    wt_hint(c, 62, WT_HINT_OK, "Save");
}

static inline void wt_draw_spare_pick(Canvas* c, const WtUi* ui) {
    char left[16], right[16];
    wt_title(c, "Which spare slot?");
    for(uint8_t i = 0; i < WT_SPARES; i++) {
        uint32_t id = ui->cfg.slot[WT_SLOT_SPARE0 + i].id;
        snprintf(left, sizeof(left), "Spare %u", i + 1u);
        if(id == ui->assign_id)
            snprintf(right, sizeof(right), "this one");
        else if(id)
            snprintf(right, sizeof(right), "%08lX", (unsigned long)id);
        else
            snprintf(right, sizeof(right), "empty");
        wt_row(c, 13 + 10 * i, i == ui->spare_pick, 5, left, right);
    }
    wt_hint(c, 3, WT_HINT_OK, "Save");
}

static inline void wt_draw_ship_intro(Canvas* c, const WtUi* ui) {
    (void)ui;
    wt_title(c, "Exit ship mode");
    wt_stepper(c, 22, 19, 0, 42);
    wt_text_centre(c, FontSecondary, 22, 33, "Read");
    wt_text_centre(c, FontSecondary, 64, 33, "Send");
    wt_text_centre(c, FontSecondary, 106, 33, "Check");
    wt_text_centre(c, FontSecondary, 64, 43, "For a new sensor the car does");
    wt_text_centre(c, FontSecondary, 64, 52, "not show. Hold it on the valve.");
    wt_hint(c, 3, WT_HINT_OK, "Start");
}

static inline void wt_draw_ship_already(Canvas* c, const WtUi* ui) {
    wt_title(c, "Exit ship mode");
    wt_draw_reading_big(c, ui, &ui->reading, 31);
    char label[16];
    wt_mode_label(label, sizeof(label), wt_b4(&ui->reading));
    int32_t bw = wt_badge(c, 3, 36, label, true);
    wt_text(c, FontSecondary, 3 + bw + 4, 45, "already OK");
    int32_t x = wt_hint(c, 3, WT_HINT_OK, "Done");
    wt_hint(c, x, WT_HINT_RIGHT, "Send anyway");
}

static inline void wt_draw_ship_confirm(Canvas* c, const WtUi* ui) {
    const WtReading* r = &ui->reading;
    WtMode mode = wt_mode(wt_b4(r));
    char line[32];
    wt_title(c, "Exit ship mode");
    canvas_set_color(c, ColorWhite);
    wt_text_right(c, FontSecondary, 125, 9, "2 Send");
    canvas_set_color(c, ColorBlack);
    char label[16];
    wt_mode_label(label, sizeof(label), wt_b4(r));
    int32_t bw = wt_badge(c, 3, 15, label, mode != WT_MODE_SHIP || (ui->anim & 2u) == 0);
    snprintf(line, sizeof(line), "%08lX", (unsigned long)r->id);
    wt_text(c, FontSecondary, 3 + bw + 5, 24, line);
    wt_text(c, FontPrimary, 3, 39, mode == WT_MODE_SHIP ? "Wake this sensor up?" : "Send the command?");
    wt_text(c, FontSecondary, 3, 49, "Keep the Flipper on the valve.");
    int32_t x = wt_hint(c, 3, WT_HINT_OK, "Send");
    wt_hint(c, x, WT_HINT_BACK, "Cancel");
}

static inline void wt_draw_ship_run(Canvas* c, const WtUi* ui) {
    char t[16];
    uint8_t ph = ui->prog.phase;
    wt_title(c, "Exit ship mode");
    canvas_set_color(c, ColorWhite);
    wt_text_right(c, FontSecondary, 125, 9, ph == WT_PH_CHECK ? "3 Check" : "2 Send");
    canvas_set_color(c, ColorBlack);
    wt_wheel_draw(c, 20, 34, ui->anim, ui->prog.lf_on && !ui->prog.heard);
    if(ph == WT_PH_CHECK) {
        wt_text(c, FontPrimary, 48, 24, ui->prog.heard ? "It answered" : "Checking...");
        wt_text(c, FontSecondary, 48, 34, "Keep holding. It");
        wt_text(c, FontSecondary, 48, 43, "can take a minute.");
    } else if(ph == WT_PH_SETTLE) {
        wt_text(c, FontPrimary, 48, 24, "It answered");
        wt_text(c, FontSecondary, 48, 34, "Waiting for it to");
        wt_text(c, FontSecondary, 48, 43, "finish...");
    } else {
        wt_text(c, FontPrimary, 48, 24, "Sending...");
        wt_text(c, FontSecondary, 48, 34, "Keep the Flipper");
        wt_text(c, FontSecondary, 48, 43, "on the valve.");
    }
    wt_progress_bar(c, 48, 47, 76, ui->prog.elapsed_ms, ui->prog.limit_ms);
    wt_time_text(t, sizeof(t), ui->prog.elapsed_ms);
    wt_text(c, FontSecondary, 48, 62, t);
    wt_stepper(c, 84, 57, ph == WT_PH_CHECK ? 3 : 2, 17);
}

static inline void wt_draw_ship_result(Canvas* c, const WtUi* ui) {
    const WtShip* s = &ui->ship;
    char line[40];
    WtMode was = wt_mode(wt_b4(&s->before));
    wt_title(c, "Exit ship mode");
    canvas_draw_disc(c, 16, 30, 13);
    canvas_set_color(c, ColorWhite);
    const char* head;
    const char* l1;
    const char* l2;
    if(s->verdict == WT_V_DONE) {
        canvas_draw_xbm(c, 8, 24, WT_ICON_TICK_W, WT_ICON_TICK_H, wt_icon_tick);
        head = "Sensor is awake";
        l1 = "Now in PARK MODE.";
        l2 = "Drive to confirm.";
    } else if(s->verdict == WT_V_NO_CHANGE && wt_mode_working(was)) {
        canvas_draw_xbm(c, 8, 24, WT_ICON_TICK_W, WT_ICON_TICK_H, wt_icon_tick);
        head = "Still working";
        l1 = "It was in park mode";
        l2 = "before and still is.";
    } else if(s->verdict == WT_V_NO_CHANGE) {
        canvas_draw_xbm(c, 11, 25, WT_ICON_CROSS_W, WT_ICON_CROSS_H, wt_icon_cross);
        head = "No change";
        l1 = s->have_ack ? "It answered but did" : "No answer to the";
        l2 = s->have_ack ? "not change. Retry." : "command. Hold closer.";
    } else if(s->verdict == WT_V_SILENT) {
        canvas_draw_xbm(c, 12, 24, WT_ICON_QUESTION_W, WT_ICON_QUESTION_H, wt_icon_question);
        head = "No answer yet";
        l1 = "Command sent. Scan";
        l2 = "it again in a minute.";
    } else {
        canvas_draw_xbm(c, 12, 24, WT_ICON_QUESTION_W, WT_ICON_QUESTION_H, wt_icon_question);
        head = "Check again";
        l1 = "Status not known to";
        l2 = "this app. Scan again.";
    }
    canvas_set_color(c, ColorBlack);
    wt_text(c, FontPrimary, 34, 23, head);
    wt_text(c, FontSecondary, 34, 33, l1);
    wt_text(c, FontSecondary, 34, 42, l2);
    /* For the curious: the status byte before, in the answer, and after. */
    char* p = line;
    p += snprintf(p, 12, "%02X", wt_b4(&s->before));
    if(s->have_ack) p += snprintf(p, 12, " > %02X", wt_b4(&s->ack));
    if(s->have_after) p += snprintf(p, 12, " > %02X", wt_b4(&s->after));
    wt_text(c, FontSecondary, 34, 52, "Status");
    wt_text(c, FontSecondary, 66, 52, line);
    wt_hint(c, 3, WT_HINT_OK, "Done");
}

/* One wheel beside the car: three short lines, at the left edge x or, with
 * right set, ending at the right edge x. */
static inline void wt_tile_line(Canvas* c, Font font, int32_t x, int32_t y, bool right, const char* s) {
    if(right)
        wt_text_right(c, font, x, y, s);
    else
        wt_text(c, font, x, y, s);
}
/* "FL drive" on the left, "drive FR" on the right. Ship mode blinks. */
static inline void wt_tile_head(Canvas* c, const WtUi* ui, uint8_t wheel, uint8_t b4, int32_t x, int32_t y, bool right) {
    char head[16];
    WtMode mode = wt_mode(b4);
    const char* word = mode == WT_MODE_SHIP && (ui->anim & 1u) ? "" : wt_mode_short(mode);
    if(right)
        snprintf(head, sizeof(head), "%s %s", word, wt_wheel_name(wheel));
    else
        snprintf(head, sizeof(head), "%s %s", wt_wheel_name(wheel), word);
    wt_tile_line(c, FontSecondary, x, y + 7, right, head);
}
/* One wheel in the monitor: its name and mode, pressure large, temperature
 * and age small. A wheel not heard yet says "waiting": from inside the car
 * readings can be minutes apart, so that is not a fault. */
static inline void wt_draw_tile(Canvas* c, const WtUi* ui, uint8_t wheel, int32_t x, int32_t y, bool right) {
    uint32_t id = ui->cfg.slot[wt_slot(ui->cfg.on_car, wheel)].id;
    const WtSensor* s = NULL;
    for(uint8_t i = 0; i < ui->nsensors; i++)
        if(id && ui->sensors[i].id == id) s = &ui->sensors[i];
    char p[12], age[8];
    if(!id) {
        wt_tile_line(c, FontSecondary, x, y + 7, right, wt_wheel_name(wheel));
        wt_tile_line(c, FontSecondary, x, y + 18, right, "not set");
        return;
    }
    if(!s) {
        wt_tile_line(c, FontSecondary, x, y + 7, right, wt_wheel_name(wheel));
        wt_tile_line(c, FontPrimary, x, y + 19, right, "-.--");
        wt_tile_line(c, FontSecondary, x, y + 28, right, "waiting");
        return;
    }
    wt_tile_head(c, ui, wheel, s->raw[4], x, y, right);
    wt_fmt_pressure(p, sizeof(p), s->raw[5], ui->cfg.punit);
    wt_age_text(age, sizeof(age), ui->now - s->last_tick);
    wt_tile_line(c, FontPrimary, x, y + 19, right, p);
    /* "2°C 12s" */
    char num[8];
    snprintf(num, sizeof(num), "%d", wt_temp(s->raw[6], ui->cfg.tunit));
    canvas_set_font(c, FontSecondary);
    int32_t tw = (int32_t)canvas_string_width(c, num) + 5 + (int32_t)canvas_string_width(c, wt_tunit_name(ui->cfg.tunit));
    int32_t aw = (int32_t)canvas_string_width(c, age);
    int32_t tx = right ? x - tw - 4 - aw : x;
    int32_t after = wt_temp_draw(c, FontSecondary, tx, y + 28, s->raw[6], ui->cfg.tunit);
    canvas_draw_str(c, after + 4, y + 28, age);
}

static inline uint8_t wt_ui_unassigned(const WtUi* ui) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < ui->nsensors; i++)
        if(wt_config_slot_of(&ui->cfg, ui->sensors[i].id) == WT_NO_SLOT) n++;
    return n;
}

/* The Bluetooth rune, 5 x 7, for the phone link: steady with a phone
 * connected, blinking while it waits for one, struck through when Bluetooth
 * could not be started. */
static inline void wt_link_mark(Canvas* c, int32_t x, int32_t y, uint8_t link, uint32_t anim) {
    if(link == WT_LINK_OFF || (link == WT_LINK_WAIT && (anim & 2u))) return;
    canvas_draw_line(c, x + 2, y, x + 2, y + 6);
    canvas_draw_line(c, x + 3, y + 1, x + 4, y + 2);
    canvas_draw_line(c, x + 3, y + 3, x, y + 5);
    canvas_draw_line(c, x + 3, y + 5, x + 4, y + 4);
    canvas_draw_line(c, x + 3, y + 3, x, y + 1);
    if(link == WT_LINK_FAIL) canvas_draw_line(c, x - 1, y + 6, x + 5, y);
}

static inline void wt_draw_monitor(Canvas* c, const WtUi* ui) {
    char line[16];
    wt_car_draw(c, 53, 2, WT_NO_WHEEL, false);
    wt_text_centre(c, FontSecondary, 64, 29, wt_punit_name(ui->cfg.punit));
    wt_draw_tile(c, ui, WT_FL, 1, 0, false);
    wt_draw_tile(c, ui, WT_FR, 127, 0, true);
    wt_draw_tile(c, ui, WT_RL, 1, 33, false);
    wt_draw_tile(c, ui, WT_RR, 127, 33, true);
    /* Under the car: the set being shown, or how many sensors heard have no
     * place yet; either way, the way to the list. */
    uint8_t extra = wt_ui_unassigned(ui);
    if(extra)
        snprintf(line, sizeof(line), "+%u", extra);
    else
        snprintf(line, sizeof(line), "%s", wt_set_name(ui->cfg.on_car));
    canvas_set_font(c, FontSecondary);
    int32_t w = (int32_t)canvas_string_width(c, line) + 7;
    canvas_draw_str(c, 64 - w / 2, 62, line);
    canvas_draw_xbm(c, 64 - w / 2 + w - 5, 55, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
    wt_link_mark(c, 62, 48, ui->link, ui->anim); /* under the car, above the set name */
}

static inline void wt_draw_monitor_list(Canvas* c, const WtUi* ui) {
    char line[40], p[12], age[8], place[8];
    snprintf(line, sizeof(line), "Sensors heard: %u", ui->nsensors);
    wt_title(c, line);
    if(!ui->nsensors) {
        wt_text(c, FontSecondary, 3, 24, "Nothing yet. Sensors send");
        wt_text(c, FontSecondary, 3, 33, "while the car moves. Inside it,");
        wt_text(c, FontSecondary, 3, 42, "minutes can pass between");
        wt_text(c, FontSecondary, 3, 51, "readings. That is normal.");
        wt_hint(c, 3, WT_HINT_LEFT, "Car view");
        return;
    }
    uint8_t first = ui->list_sel >= WT_LIST_ROWS ? (uint8_t)(ui->list_sel - WT_LIST_ROWS + 1u) : 0;
    for(uint8_t row = 0; row < WT_LIST_ROWS && first + row < ui->nsensors; row++) {
        const WtSensor* s = &ui->sensors[first + row];
        int32_t y = 13 + 10 * row;
        bool sel = first + row == ui->list_sel;
        if(sel) canvas_draw_box(c, 0, y, 128, 10);
        canvas_set_color(c, sel ? ColorWhite : ColorBlack);
        wt_fmt_pressure(p, sizeof(p), s->raw[5], ui->cfg.punit);
        wt_age_text(age, sizeof(age), ui->now - s->last_tick);
        wt_slot_short(place, sizeof(place), wt_config_slot_of(&ui->cfg, s->id));
        wt_text(c, FontSecondary, 2, y + 8, place);
        snprintf(line, sizeof(line), "%08lX", (unsigned long)s->id);
        wt_text(c, FontSecondary, 30, y + 8, line);
        wt_text_right(c, FontSecondary, 105, y + 8, p);
        wt_text_right(c, FontSecondary, 126, y + 8, age);
        canvas_set_color(c, ColorBlack);
    }
    int32_t x = wt_hint(c, 3, WT_HINT_OK, "Set place");
    wt_hint(c, x, WT_HINT_LEFT, "Car view");
}

/* One wheel of a set in "My wheels": what its sensor last told this Flipper
 * and when, or its ID. */
static inline void wt_draw_slot_tile(Canvas* c, const WtUi* ui, uint8_t slot, int32_t x, int32_t y, bool right) {
    const WtSlot* s = &ui->cfg.slot[slot];
    uint8_t wheel = wt_slot_wheel(slot);
    char p[12];
    if(!s->id) {
        wt_tile_line(c, FontSecondary, x, y + 7, right, wt_wheel_name(wheel));
        wt_tile_line(c, FontSecondary, x, y + 18, right, "not set");
        return;
    }
    if(ui->wheels_ids) {
        wt_tile_line(c, FontSecondary, x, y + 7, right, wt_wheel_name(wheel));
        snprintf(p, sizeof(p), "%04lX", (unsigned long)(s->id >> 16));
        wt_tile_line(c, FontPrimary, x, y + 18, right, p);
        snprintf(p, sizeof(p), "%04lX", (unsigned long)(s->id & 0xFFFFu));
        wt_tile_line(c, FontPrimary, x, y + 28, right, p);
        return;
    }
    if(!s->seen) {
        wt_tile_line(c, FontSecondary, x, y + 7, right, wt_wheel_name(wheel));
        wt_tile_line(c, FontPrimary, x, y + 19, right, "-.--");
        wt_tile_line(c, FontSecondary, x, y + 28, right, "no data");
        return;
    }
    wt_tile_head(c, ui, wheel, s->b4, x, y, right);
    wt_fmt_pressure(p, sizeof(p), s->b5, ui->cfg.punit);
    wt_tile_line(c, FontPrimary, x, y + 19, right, p);
    wt_date_text(p, sizeof(p), s->date);
    wt_tile_line(c, FontSecondary, x, y + 28, right, p[0] ? p : "no date");
}

/* "My wheels": a page per set, drawn like the monitor but from what is kept
 * in the settings file, then a page for the spare tyre and spare sensors. */
static inline void wt_draw_wheels(Canvas* c, const WtUi* ui) {
    char line[24], p[12], date[10];
    if(ui->wheels_page < WT_SETS) {
        uint8_t set = ui->wheels_page;
        wt_car_draw(c, 53, 2, WT_NO_WHEEL, false);
        wt_text_centre(c, FontSecondary, 64, 29, ui->wheels_ids ? "ID" : wt_punit_name(ui->cfg.punit));
        wt_draw_slot_tile(c, ui, wt_slot(set, WT_FL), 1, 0, false);
        wt_draw_slot_tile(c, ui, wt_slot(set, WT_FR), 127, 0, true);
        wt_draw_slot_tile(c, ui, wt_slot(set, WT_RL), 1, 33, false);
        wt_draw_slot_tile(c, ui, wt_slot(set, WT_RR), 127, 33, true);
        /* Under the car: the set's name, filled if it is the one on the car. */
        canvas_set_font(c, FontSecondary);
        int32_t w = (int32_t)canvas_string_width(c, wt_set_name(set)) + 6;
        if(set == ui->cfg.on_car) {
            canvas_draw_rbox(c, 64 - w / 2, 54, (size_t)w, 10, 2);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 64 - w / 2 + 3, 62, wt_set_name(set));
        canvas_set_color(c, ColorBlack);
        canvas_draw_xbm(c, 64 - w / 2 - 7, 56, WT_ICON_KEY_LEFT_W, WT_ICON_KEY_LEFT_H, wt_icon_key_left);
        canvas_draw_xbm(c, 64 + w / 2 + 3, 56, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
        return;
    }
    wt_title(c, ui->wheels_ids ? "Spares: IDs" : "Spares");
    canvas_set_color(c, ColorWhite);
    canvas_draw_xbm(c, 111, 3, WT_ICON_KEY_LEFT_W, WT_ICON_KEY_LEFT_H, wt_icon_key_left);
    canvas_draw_xbm(c, 119, 3, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
    canvas_set_color(c, ColorBlack);
    for(uint8_t i = 0; i <= WT_SPARES; i++) {
        const WtSlot* s = &ui->cfg.slot[WT_SLOT_TYRE + i];
        int32_t y = 13 + 10 * i;
        wt_slot_short(line, sizeof(line), (uint8_t)(WT_SLOT_TYRE + i));
        wt_text(c, FontSecondary, 2, y + 8, line);
        if(!s->id) {
            wt_text(c, FontSecondary, 30, y + 8, "empty");
        } else if(ui->wheels_ids) {
            snprintf(line, sizeof(line), "%08lX", (unsigned long)s->id);
            wt_text(c, FontSecondary, 30, y + 8, line);
        } else if(!s->seen) {
            wt_text(c, FontSecondary, 30, y + 8, "no data");
        } else {
            /* "TYRE  park  2.75  27 Sep" */
            wt_text(c, FontSecondary, 30, y + 8, wt_mode_short(wt_mode(s->b4)));
            wt_fmt_pressure(p, sizeof(p), s->b5, ui->cfg.punit);
            wt_date_text(date, sizeof(date), s->date);
            wt_text_right(c, FontSecondary, 88, y + 8, p);
            wt_text_right(c, FontSecondary, 126, y + 8, date[0] ? date : "no date");
        }
    }
}

static inline void wt_draw_settings(Canvas* c, const WtUi* ui) {
    static const char* const label[WT_SET_COUNT] = {"Pressure",   "Temperature", "On the car",    "Ask place",
                                                   "Radio band", "Which cars?", "Forget places", "Developer"};
    wt_title(c, "Settings");
    uint8_t first = ui->set_row >= WT_LIST_ROWS ? (uint8_t)(ui->set_row - WT_LIST_ROWS + 1u) : 0;
    for(uint8_t row = 0; row < WT_LIST_ROWS && first + row < WT_SET_COUNT; row++) {
        uint8_t i = (uint8_t)(first + row);
        int32_t y = 13 + 10 * row;
        bool sel = i == ui->set_row;
        const char* val = i == WT_SET_PRESSURE ? wt_punit_name(ui->cfg.punit) :
                          i == WT_SET_TEMP     ? (ui->cfg.tunit == WT_T_F ? "deg F" : "deg C") :
                          i == WT_SET_ON_CAR   ? wt_set_name(ui->cfg.on_car) :
                          i == WT_SET_ASK ? (ui->cfg.ask == WT_ASK_ALWAYS ? "always" : ui->cfg.ask == WT_ASK_NEVER ? "never" : "if new") :
                          i == WT_SET_BAND ? wt_band_name(ui->cfg.band) :
                                            "";
        wt_row(c, y, sel, 5, label[i], NULL);
        canvas_set_color(c, sel ? ColorWhite : ColorBlack);
        if(i == WT_SET_FORGET || i == WT_SET_CARS || i == WT_SET_DEV) {
            if(sel) canvas_draw_xbm(c, 114, y + 1, WT_ICON_KEY_OK_W, WT_ICON_KEY_OK_H, wt_icon_key_ok);
        } else {
            wt_text_right(c, FontPrimary, 114, y + 8, val);
            if(sel) {
                canvas_draw_xbm(c, 118, y + 1, WT_ICON_KEY_RIGHT_W, WT_ICON_KEY_RIGHT_H, wt_icon_key_right);
                canvas_set_font(c, FontPrimary);
                int32_t vx = 114 - (int32_t)canvas_string_width(c, val);
                canvas_draw_xbm(c, vx - 8, y + 1, WT_ICON_KEY_LEFT_W, WT_ICON_KEY_LEFT_H, wt_icon_key_left);
            }
        }
        canvas_set_color(c, ColorBlack);
    }
    if(ui->toast_ticks)
        wt_text(c, FontSecondary, 3, 62, ui->toast);
    else if(ui->set_row == WT_SET_ASK)
        wt_text(c, FontSecondary, 3, 62, "Ask where a scanned sensor is");
    else if(ui->set_row == WT_SET_ON_CAR)
        wt_text(c, FontSecondary, 3, 62, "The set the monitor shows");
    else if(ui->set_row == WT_SET_DEV)
        wt_text(c, FontSecondary, 3, 62, "Lab tools. Here be dragons");
    else if(ui->set_row == WT_SET_BAND)
        wt_text(c, FontSecondary, 3, 62, "433 Europe, 315 N. America");
    else if(ui->set_row == WT_SET_CARS)
        wt_text(c, FontSecondary, 3, 62, "Cars expected to work");
    else
        wt_text(c, FontSecondary, 3, 62, "Back saves.  Continental TG1C");
}

/* The warning in front of the Developer menu. */
static inline void wt_draw_dragons(Canvas* c, const WtUi* ui) {
    (void)ui;
    wt_title(c, "Here be dragons");
    wt_text(c, FontSecondary, 3, 22, "The lab tools this app grew");
    wt_text(c, FontSecondary, 3, 31, "out of, and a phone link still");
    wt_text(c, FontSecondary, 3, 40, "on test. Not needed to read or");
    wt_text(c, FontSecondary, 3, 49, "fix a sensor.");
    wt_hint(c, wt_hint(c, 3, WT_HINT_OK, "Go on"), WT_HINT_BACK, "Back");
}
/* Developer: the phone link's switch, then one row per lab tool. */
static inline void wt_draw_dev(Canvas* c, const WtUi* ui) {
    uint8_t sel = ui->dev_row < WT_DEV_COUNT ? ui->dev_row : 0;
    uint8_t first = sel >= WT_LIST_ROWS ? (uint8_t)(sel - WT_LIST_ROWS + 1u) : 0;
    wt_title(c, "Developer");
    for(uint8_t row = 0; row < WT_LIST_ROWS && first + row < WT_DEV_COUNT; row++) {
        uint8_t i = (uint8_t)(first + row);
        int32_t y = 13 + 10 * row;
        bool phone = i < WT_DEV_PHONE_ROWS;
        wt_row(c, y, i == sel, 5, phone ? "Phone link" : wt_dev_tools[i - WT_DEV_PHONE_ROWS].name, NULL);
        canvas_set_color(c, i == sel ? ColorWhite : ColorBlack);
        if(phone)
            wt_text_right(c, FontPrimary, 123, y + 8, ui->cfg.phone ? "on" : "off");
        else if(i == sel)
            canvas_draw_xbm(c, 114, y + 1, WT_ICON_KEY_OK_W, WT_ICON_KEY_OK_H, wt_icon_key_ok);
        canvas_set_color(c, ColorBlack);
    }
    wt_text(
        c, FontSecondary, 3, 62,
        ui->toast_ticks ? ui->toast :
        sel < WT_DEV_PHONE_ROWS ? "Readings to a paired phone" :
                                  wt_dev_tools[sel - WT_DEV_PHONE_ROWS].hint);
}

/* "Which cars?": the makes a workshop tool treats like the sensor this app
 * was built on (wt_cars.h), the radio band each is fitted with, and along the
 * bottom how much of that has been tried. */
static inline void wt_draw_cars(Canvas* c, const WtUi* ui) {
    uint8_t sel = ui->car_sel < WT_CARS_COUNT ? ui->car_sel : 0;
    uint8_t first = sel >= WT_LIST_ROWS ? (uint8_t)(sel - WT_LIST_ROWS + 1u) : 0;
    wt_title(c, "Which cars?");
    for(uint8_t row = 0; row < WT_LIST_ROWS && first + row < WT_CARS_COUNT; row++) {
        const WtCarMake* m = &wt_cars[first + row];
        wt_row(c, 13 + 10 * row, first + row == sel, 5, m->make, wt_car_bands(m->bands));
    }
    wt_text(c, FontSecondary, 3, 62, wt_car_status(wt_cars[sel].status));
}
/* One make: the years, then the models, four lines at a time. */
static inline void wt_draw_car(Canvas* c, const WtUi* ui) {
    const WtCarMake* m = &wt_cars[ui->car_sel < WT_CARS_COUNT ? ui->car_sel : 0];
    wt_title(c, m->make);
    canvas_set_color(c, ColorWhite);
    wt_text_right(c, FontSecondary, 125, 9, wt_car_bands(m->bands));
    canvas_set_color(c, ColorBlack);
    for(uint8_t row = 0; row < WT_CAR_ROWS && ui->car_line + row < m->nlines; row++)
        wt_text(c, FontSecondary, 3, 22 + 9 * row, m->lines[ui->car_line + row]);
    if(m->nlines > WT_CAR_ROWS) { /* a scroll bar: where these four lines are in the list */
        int32_t track = 36, thumb = track * (int32_t)WT_CAR_ROWS / m->nlines;
        int32_t at = (track - thumb) * ui->car_line / (m->nlines - (int32_t)WT_CAR_ROWS);
        canvas_draw_line(c, 126, 15, 126, 15 + track - 1);
        canvas_draw_box(c, 125, 15 + at, 3, (size_t)thumb);
    }
    wt_text(c, FontSecondary, 3, 62, wt_car_status(m->status));
}

static inline void wt_draw_fault(Canvas* c, const WtUi* ui) {
    wt_title(c, "Problem");
    wt_text(c, FontPrimary, 3, 26, ui->fault);
    wt_text(c, FontSecondary, 3, 38, "Close other apps that use the");
    wt_text(c, FontSecondary, 3, 47, "radio or RFID, then try again.");
    wt_hint(c, 3, WT_HINT_OK, "Home");
}

static inline void wt_ui_draw(Canvas* c, const WtUi* ui) {
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    switch(ui->screen) {
    case WT_SCR_HOME:
        wt_draw_home(c, ui);
        break;
    case WT_SCR_SCAN_RUN:
        wt_draw_scan_run(c, ui);
        break;
    case WT_SCR_SCAN_RESULT:
        wt_draw_scan_result(c, ui);
        break;
    case WT_SCR_SCAN_DETAIL:
        wt_draw_scan_detail(c, ui);
        break;
    case WT_SCR_SCAN_FAIL:
        wt_draw_scan_fail(c, ui);
        break;
    case WT_SCR_WHERE:
        wt_draw_where(c, ui);
        break;
    case WT_SCR_ASSIGN:
        wt_draw_assign(c, ui);
        break;
    case WT_SCR_SPARE_PICK:
        wt_draw_spare_pick(c, ui);
        break;
    case WT_SCR_WHEELS:
        wt_draw_wheels(c, ui);
        break;
    case WT_SCR_SHIP_INTRO:
        wt_draw_ship_intro(c, ui);
        break;
    case WT_SCR_SHIP_ALREADY:
        wt_draw_ship_already(c, ui);
        break;
    case WT_SCR_SHIP_CONFIRM:
        wt_draw_ship_confirm(c, ui);
        break;
    case WT_SCR_SHIP_RUN:
        wt_draw_ship_run(c, ui);
        break;
    case WT_SCR_SHIP_RESULT:
        wt_draw_ship_result(c, ui);
        break;
    case WT_SCR_MONITOR:
        wt_draw_monitor(c, ui);
        break;
    case WT_SCR_MONITOR_LIST:
        wt_draw_monitor_list(c, ui);
        break;
    case WT_SCR_SETTINGS:
        wt_draw_settings(c, ui);
        break;
    case WT_SCR_CARS:
        wt_draw_cars(c, ui);
        break;
    case WT_SCR_CAR:
        wt_draw_car(c, ui);
        break;
    case WT_SCR_DRAGONS:
        wt_draw_dragons(c, ui);
        break;
    case WT_SCR_DEV:
        wt_draw_dev(c, ui);
        break;
    default:
        wt_draw_fault(c, ui);
        break;
    }
}

/* ===== events ============================================================== */

static inline void wt_ui_toast(WtUi* ui, const char* text) {
    snprintf(ui->toast, sizeof(ui->toast), "%s", text);
    ui->toast_ticks = WT_TOAST_TICKS;
}
/* Four times a second, from the app's timer. */
static inline void wt_ui_tick(WtUi* ui) {
    ui->anim++;
    if(ui->toast_ticks) ui->toast_ticks--;
}
static inline WtCmd wt_ui_begin(WtUi* ui, WtScreen screen, WtCmd cmd) {
    ui->screen = (uint8_t)screen;
    ui->op_seq++;
    memset(&ui->prog, 0, sizeof(ui->prog));
    return cmd;
}
/* Open "Where is it?" for a sensor, on the row of the place it has now. */
static inline void wt_ui_open_where(WtUi* ui, uint32_t id, WtScreen return_to) {
    if(!id) return;
    ui->assign_id = id;
    ui->return_to = (uint8_t)return_to;
    uint8_t slot = wt_config_slot_of(&ui->cfg, id);
    ui->where_sel = slot != WT_NO_SLOT ? wt_where_row_of(slot) : ui->cfg.on_car;
    ui->screen = WT_SCR_WHERE;
}
/* What is known about a sensor right now: the last scan, or the monitor. */
static inline void wt_ui_note_now(WtUi* ui, uint32_t id) {
    if(id && ui->reading.id == id)
        wt_config_note(&ui->cfg, id, ui->reading.raw[4], ui->reading.raw[5], ui->reading.raw[6], ui->today);
    for(uint8_t i = 0; i < ui->nsensors; i++)
        if(ui->sensors[i].id == id)
            wt_config_note(&ui->cfg, id, ui->sensors[i].raw[4], ui->sensors[i].raw[5], ui->sensors[i].raw[6], ui->today);
}
/* Give the sensor being placed a place, and go back to where the user was. */
static inline void wt_ui_place(WtUi* ui, uint8_t slot) {
    char name[8];
    wt_config_assign(&ui->cfg, slot, ui->assign_id);
    wt_ui_note_now(ui, ui->assign_id);
    wt_slot_short(name, sizeof(name), slot);
    snprintf(ui->toast, sizeof(ui->toast), "Saved as %s", name);
    ui->toast_ticks = WT_TOAST_TICKS;
    ui->dirty = true;
    ui->screen = ui->return_to;
}

/* The engine's progress, from the worker (the caller holds the model). */
static inline void wt_ui_progress(WtUi* ui, const WtEngine* e) {
    ui->prog = e->prog;
    if(e->prog.phase == WT_PH_MONITOR) {
        memcpy(ui->sensors, e->sensors, sizeof(ui->sensors));
        ui->nsensors = e->nsensors;
        ui->now = e->last_tick > e->shown ? e->last_tick : e->shown;
        if(ui->list_sel >= ui->nsensors) ui->list_sel = ui->nsensors ? (uint8_t)(ui->nsensors - 1u) : 0;
        /* Keep the latest reading of every sensor that has a place. It goes
         * to the settings file when the monitor is left, not while it runs. */
        for(uint8_t i = 0; i < ui->nsensors; i++) {
            const WtSensor* s = &ui->sensors[i];
            if(wt_config_note(&ui->cfg, s->id, s->raw[4], s->raw[5], s->raw[6], ui->today)) ui->mon_touched = true;
        }
    }
}
static inline void wt_ui_set_fault(WtUi* ui, const char* fault) {
    snprintf(ui->fault, sizeof(ui->fault), "%s", fault ? fault : "Something went wrong");
    ui->screen = WT_SCR_FAULT;
}
/* A scan has ended. seq is the op_seq the worker was started with. */
static inline void wt_ui_scan_done(WtUi* ui, uint32_t seq, WtResult result, const WtReading* r, const char* fault) {
    if(seq != ui->op_seq || ui->screen != WT_SCR_SCAN_RUN) return;
    if(result == WT_R_FAULT) {
        wt_ui_set_fault(ui, fault);
    } else if(result == WT_R_NO_REPLY) {
        ui->screen = WT_SCR_SCAN_FAIL;
    } else if(result == WT_R_OK) {
        ui->reading = *r;
        bool placed = wt_config_note(&ui->cfg, r->id, r->raw[4], r->raw[5], r->raw[6], ui->today);
        if(placed) ui->dirty = true;
        if(ui->ship_flow) {
            ui->screen = wt_mode_working(wt_mode(wt_b4(r))) ? WT_SCR_SHIP_ALREADY : WT_SCR_SHIP_CONFIRM;
        } else {
            ui->screen = WT_SCR_SCAN_RESULT;
            /* The "Ask place" setting: offer a place for the sensor just read. */
            if(ui->cfg.ask == WT_ASK_ALWAYS || (ui->cfg.ask == WT_ASK_NEW && !placed))
                wt_ui_open_where(ui, r->id, WT_SCR_SCAN_RESULT);
        }
    } else {
        ui->screen = WT_SCR_HOME;
    }
}
static inline void wt_ui_ship_done(WtUi* ui, uint32_t seq, WtResult result, const WtShip* s, const char* fault) {
    if(seq != ui->op_seq || ui->screen != WT_SCR_SHIP_RUN) return;
    ui->ship = *s;
    if(s->have_after &&
       wt_config_note(&ui->cfg, s->after.id, s->after.raw[4], s->after.raw[5], s->after.raw[6], ui->today))
        ui->dirty = true;
    if(result == WT_R_FAULT)
        wt_ui_set_fault(ui, fault);
    else if(result == WT_R_OK)
        ui->screen = WT_SCR_SHIP_RESULT;
    else
        ui->screen = WT_SCR_HOME;
}
static inline void wt_ui_monitor_done(WtUi* ui, uint32_t seq, WtResult result, const char* fault) {
    if(seq != ui->op_seq) return;
    if(result == WT_R_FAULT && (ui->screen == WT_SCR_MONITOR || ui->screen == WT_SCR_MONITOR_LIST))
        wt_ui_set_fault(ui, fault);
}

static inline WtCmd wt_ui_input(WtUi* ui, WtKey key) {
    switch(ui->screen) {
    case WT_SCR_HOME:
        if(key == WT_KEY_UP) ui->menu = (uint8_t)((ui->menu + WT_MENU_COUNT - 1u) % WT_MENU_COUNT);
        if(key == WT_KEY_DOWN) ui->menu = (uint8_t)((ui->menu + 1u) % WT_MENU_COUNT);
        if(key == WT_KEY_BACK) return WT_CMD_EXIT;
        if(key == WT_KEY_OK || key == WT_KEY_RIGHT) {
            if(ui->menu == WT_MENU_SCAN) {
                ui->ship_flow = false;
                ui->scan_new = false;
                ui->nrejected = 0;
                return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
            }
            if(ui->menu == WT_MENU_SHIP) ui->screen = WT_SCR_SHIP_INTRO;
            if(ui->menu == WT_MENU_MONITOR) {
                ui->nsensors = 0;
                ui->list_sel = 0;
                return wt_ui_begin(ui, WT_SCR_MONITOR, WT_CMD_MONITOR);
            }
            if(ui->menu == WT_MENU_WHEELS) {
                ui->wheels_page = ui->cfg.on_car;
                ui->wheels_ids = false;
                ui->screen = WT_SCR_WHEELS;
            }
            if(ui->menu == WT_MENU_SETTINGS) {
                ui->set_row = 0;
                ui->screen = WT_SCR_SETTINGS;
            }
        }
        return WT_CMD_NONE;

    case WT_SCR_SCAN_RUN:
    case WT_SCR_SHIP_RUN:
        if(key == WT_KEY_BACK) {
            ui->screen = WT_SCR_HOME;
            ui->op_seq++; /* whatever the worker reports now is stale */
            return WT_CMD_CANCEL;
        }
        return WT_CMD_NONE;

    case WT_SCR_SCAN_RESULT:
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_RIGHT) ui->screen = WT_SCR_SCAN_DETAIL;
        if(key == WT_KEY_OK) wt_ui_open_where(ui, ui->reading.id, WT_SCR_SCAN_RESULT);
        if(key == WT_KEY_LEFT) {
            /* "Not it": a neighbour answered. Scan again, ignoring it and
             * every sensor that has a place. */
            if(ui->nrejected == WT_REJECT_MAX) {
                memmove(ui->rejected, ui->rejected + 1, (WT_REJECT_MAX - 1u) * sizeof(uint32_t));
                ui->nrejected--;
            }
            ui->rejected[ui->nrejected++] = ui->reading.id;
            ui->scan_new = true;
            return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
        }
        return WT_CMD_NONE;

    case WT_SCR_SCAN_DETAIL:
        if(key == WT_KEY_BACK || key == WT_KEY_LEFT || key == WT_KEY_OK) ui->screen = WT_SCR_SCAN_RESULT;
        return WT_CMD_NONE;

    case WT_SCR_SCAN_FAIL:
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_OK) return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
        return WT_CMD_NONE;

    case WT_SCR_WHERE: {
        uint8_t rows = wt_where_rows(ui);
        uint8_t slot = wt_config_slot_of(&ui->cfg, ui->assign_id);
        if(key == WT_KEY_UP) ui->where_sel = (uint8_t)((ui->where_sel + rows - 1u) % rows);
        if(key == WT_KEY_DOWN) ui->where_sel = (uint8_t)((ui->where_sel + 1u) % rows);
        if(key == WT_KEY_BACK) ui->screen = ui->return_to;
        if(key == WT_KEY_OK || key == WT_KEY_RIGHT) {
            if(ui->where_sel < WT_SETS) {
                /* the wheel it has in that set, or the first free one */
                uint8_t w = 0;
                ui->assign_set = ui->where_sel;
                if(slot < WT_SLOT_TYRE && wt_slot_set(slot) == ui->assign_set)
                    w = wt_slot_wheel(slot);
                else
                    while(w < WT_WHEELS - 1 && ui->cfg.slot[wt_slot(ui->assign_set, w)].id) w++;
                ui->wheel_pick = w;
                ui->screen = WT_SCR_ASSIGN;
            } else if(ui->where_sel == WT_WHERE_TYRE) {
                wt_ui_place(ui, WT_SLOT_TYRE);
            } else if(ui->where_sel == WT_WHERE_SPARE) {
                uint8_t i = 0;
                if(slot >= WT_SLOT_SPARE0 && slot < WT_SLOTS)
                    i = (uint8_t)(slot - WT_SLOT_SPARE0);
                else
                    while(i < WT_SPARES - 1 && ui->cfg.slot[WT_SLOT_SPARE0 + i].id) i++;
                ui->spare_pick = i;
                ui->screen = WT_SCR_SPARE_PICK;
            } else {
                wt_config_remove(&ui->cfg, ui->assign_id);
                wt_ui_toast(ui, "Place removed");
                ui->dirty = true;
                ui->screen = ui->return_to;
            }
        }
        return WT_CMD_NONE;
    }

    case WT_SCR_ASSIGN:
        if(key == WT_KEY_UP || key == WT_KEY_DOWN) ui->wheel_pick ^= 2u; /* front <-> rear */
        if(key == WT_KEY_LEFT || key == WT_KEY_RIGHT) ui->wheel_pick ^= 1u; /* left <-> right */
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_WHERE;
        if(key == WT_KEY_OK) wt_ui_place(ui, wt_slot(ui->assign_set, ui->wheel_pick));
        return WT_CMD_NONE;

    case WT_SCR_SPARE_PICK:
        if(key == WT_KEY_UP) ui->spare_pick = (uint8_t)((ui->spare_pick + WT_SPARES - 1u) % WT_SPARES);
        if(key == WT_KEY_DOWN) ui->spare_pick = (uint8_t)((ui->spare_pick + 1u) % WT_SPARES);
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_WHERE;
        if(key == WT_KEY_OK) wt_ui_place(ui, (uint8_t)(WT_SLOT_SPARE0 + ui->spare_pick));
        return WT_CMD_NONE;

    case WT_SCR_WHEELS:
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_LEFT) ui->wheels_page = (uint8_t)((ui->wheels_page + WT_WHEELS_PAGES - 1u) % WT_WHEELS_PAGES);
        if(key == WT_KEY_RIGHT) ui->wheels_page = (uint8_t)((ui->wheels_page + 1u) % WT_WHEELS_PAGES);
        if(key == WT_KEY_OK) ui->wheels_ids = !ui->wheels_ids;
        return WT_CMD_NONE;

    case WT_SCR_SHIP_INTRO:
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_OK) {
            ui->ship_flow = true;
            ui->scan_new = false;
            return wt_ui_begin(ui, WT_SCR_SCAN_RUN, WT_CMD_SCAN);
        }
        return WT_CMD_NONE;

    case WT_SCR_SHIP_ALREADY:
        if(key == WT_KEY_BACK || key == WT_KEY_OK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_RIGHT) ui->screen = WT_SCR_SHIP_CONFIRM;
        return WT_CMD_NONE;

    case WT_SCR_SHIP_CONFIRM:
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_HOME;
        if(key == WT_KEY_OK) return wt_ui_begin(ui, WT_SCR_SHIP_RUN, WT_CMD_SHIP);
        return WT_CMD_NONE;

    case WT_SCR_SHIP_RESULT:
        if(key == WT_KEY_BACK || key == WT_KEY_OK) ui->screen = WT_SCR_HOME;
        return WT_CMD_NONE;

    case WT_SCR_MONITOR:
        if(key == WT_KEY_BACK) {
            ui->screen = WT_SCR_HOME;
            ui->op_seq++;
            if(ui->mon_touched) ui->dirty = true; /* keep what was heard */
            ui->mon_touched = false;
            return WT_CMD_CANCEL;
        }
        if(key == WT_KEY_RIGHT || key == WT_KEY_OK) ui->screen = WT_SCR_MONITOR_LIST;
        return WT_CMD_NONE;

    case WT_SCR_MONITOR_LIST:
        if(key == WT_KEY_BACK || key == WT_KEY_LEFT) ui->screen = WT_SCR_MONITOR;
        if(key == WT_KEY_UP && ui->list_sel) ui->list_sel--;
        if(key == WT_KEY_DOWN && ui->list_sel + 1u < ui->nsensors) ui->list_sel++;
        if(key == WT_KEY_OK && ui->list_sel < ui->nsensors)
            wt_ui_open_where(ui, ui->sensors[ui->list_sel].id, WT_SCR_MONITOR_LIST);
        return WT_CMD_NONE;

    case WT_SCR_SETTINGS:
        if(key == WT_KEY_UP) ui->set_row = (uint8_t)((ui->set_row + WT_SET_COUNT - 1u) % WT_SET_COUNT);
        if(key == WT_KEY_DOWN) ui->set_row = (uint8_t)((ui->set_row + 1u) % WT_SET_COUNT);
        if(key == WT_KEY_BACK) {
            ui->screen = WT_SCR_HOME;
            ui->dirty = true;
        }
        if(key == WT_KEY_LEFT || key == WT_KEY_RIGHT || key == WT_KEY_OK) {
            uint8_t step = key == WT_KEY_LEFT ? 1u : 0u; /* left goes back one */
            if(ui->set_row == WT_SET_PRESSURE)
                ui->cfg.punit = (uint8_t)((ui->cfg.punit + (step ? WT_P_COUNT - 1u : 1u)) % WT_P_COUNT);
            if(ui->set_row == WT_SET_TEMP)
                ui->cfg.tunit = (uint8_t)((ui->cfg.tunit + (step ? WT_T_COUNT - 1u : 1u)) % WT_T_COUNT);
            if(ui->set_row == WT_SET_ON_CAR) ui->cfg.on_car = (uint8_t)((ui->cfg.on_car + 1u) % WT_SETS);
            if(ui->set_row == WT_SET_ASK)
                ui->cfg.ask = (uint8_t)((ui->cfg.ask + (step ? WT_ASK_COUNT - 1u : 1u)) % WT_ASK_COUNT);
            if(ui->set_row == WT_SET_DEV && key != WT_KEY_LEFT) ui->screen = WT_SCR_DRAGONS;
            if(ui->set_row == WT_SET_BAND) ui->cfg.band = (uint8_t)((ui->cfg.band + 1u) % WT_BAND_COUNT);
            if(ui->set_row == WT_SET_CARS && key != WT_KEY_LEFT) {
                ui->car_sel = 0;
                ui->screen = WT_SCR_CARS;
            }
            if(ui->set_row == WT_SET_FORGET && key == WT_KEY_OK) {
                memset(ui->cfg.slot, 0, sizeof(ui->cfg.slot));
                wt_ui_toast(ui, "Places forgotten");
            }
        }
        return WT_CMD_NONE;

    case WT_SCR_DRAGONS:
        if(key == WT_KEY_BACK || key == WT_KEY_LEFT) ui->screen = WT_SCR_SETTINGS;
        if(key == WT_KEY_OK) {
            ui->dev_row = 0;
            ui->screen = WT_SCR_DEV;
        }
        return WT_CMD_NONE;

    case WT_SCR_DEV:
        if(key == WT_KEY_UP) ui->dev_row = (uint8_t)((ui->dev_row + WT_DEV_COUNT - 1u) % WT_DEV_COUNT);
        if(key == WT_KEY_DOWN) ui->dev_row = (uint8_t)((ui->dev_row + 1u) % WT_DEV_COUNT);
        if(key == WT_KEY_BACK) ui->screen = WT_SCR_SETTINGS;
        if(ui->dev_row < WT_DEV_PHONE_ROWS) {
            if(key == WT_KEY_LEFT || key == WT_KEY_RIGHT || key == WT_KEY_OK) {
                ui->cfg.phone = !ui->cfg.phone;
                ui->dirty = true;
            }
        } else if(key == WT_KEY_OK) { /* only OK starts a tool, as only OK forgets places */
            ui->launch = (uint8_t)(ui->dev_row - WT_DEV_PHONE_ROWS);
            return WT_CMD_LAUNCH;
        }
        return WT_CMD_NONE;

    case WT_SCR_CARS:
        if(key == WT_KEY_UP) ui->car_sel = (uint8_t)((ui->car_sel + WT_CARS_COUNT - 1u) % WT_CARS_COUNT);
        if(key == WT_KEY_DOWN) ui->car_sel = (uint8_t)((ui->car_sel + 1u) % WT_CARS_COUNT);
        if(key == WT_KEY_BACK || key == WT_KEY_LEFT) ui->screen = WT_SCR_SETTINGS;
        if(key == WT_KEY_OK || key == WT_KEY_RIGHT) {
            ui->car_line = 0;
            ui->screen = WT_SCR_CAR;
        }
        return WT_CMD_NONE;

    case WT_SCR_CAR: {
        uint8_t n = wt_cars[ui->car_sel < WT_CARS_COUNT ? ui->car_sel : 0].nlines;
        if(key == WT_KEY_UP && ui->car_line) ui->car_line--;
        if(key == WT_KEY_DOWN && ui->car_line + WT_CAR_ROWS < n) ui->car_line++;
        if(key == WT_KEY_BACK || key == WT_KEY_LEFT || key == WT_KEY_OK) ui->screen = WT_SCR_CARS;
        return WT_CMD_NONE;
    }

    default: /* fault */
        ui->screen = WT_SCR_HOME;
        return WT_CMD_NONE;
    }
}

/* True while the screen belongs to an operation that is running. */
static inline bool wt_ui_busy(const WtUi* ui) {
    return ui->screen == WT_SCR_SCAN_RUN || ui->screen == WT_SCR_SHIP_RUN || ui->screen == WT_SCR_MONITOR ||
           ui->screen == WT_SCR_MONITOR_LIST ||
           ((ui->screen == WT_SCR_WHERE || ui->screen == WT_SCR_ASSIGN || ui->screen == WT_SCR_SPARE_PICK) &&
            ui->return_to == WT_SCR_MONITOR_LIST);
}
