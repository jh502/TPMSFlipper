// SPDX-License-Identifier: GPL-2.0-or-later
/* Host test for flipper/tpmsflipper/wt_ui.h: every screen is drawn with
 * the device's fonts on the host canvas (tests/support/gui/canvas.h), saved
 * as a PBM, and must not have a single pixel or string cut off by the screen
 * edge. Then the key handling: each path through the menus, and that a result
 * from an operation the user has left does not change the screen.
 *
 *   tpmsflipper_ui <out_dir>
 *
 * The sensor IDs here are made up: these screens become the screenshots in
 * the documentation.
 */
#define WT_CANVAS_IMPL
#include "wt_remote.h"
#include <assert.h>

static const char* out_dir;
static int shots, failed;

static void shot(const WtUi* ui, const char* name) {
    Canvas c;
    char path[256];
    canvas_host_init(&c);
    wt_ui_draw(&c, ui);
    if(c.clipped || c.text_clipped) {
        fprintf(stderr, "FAIL: screen %s: %u pixels off-screen, text \"%s\"\n", name, c.clipped, c.last_clipped);
        failed++;
    }
    snprintf(path, sizeof(path), "%s/%s.pbm", out_dir, name);
    assert(canvas_host_write_pbm(&c, path));
    shots++;
}
static WtReading reading(uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, int rssi) {
    WtReading r = {.id = id, .rssi_dbm = (int8_t)rssi, .frames = 3, .answer_ms = 680};
    uint8_t raw[9] = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id, b4, b5, b6, 0x01, 0};
    for(int i = 0; i < 8; i++) raw[8] ^= raw[i];
    memcpy(r.raw, raw, 9);
    return r;
}
static WtSensor sensor(uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, uint32_t last) {
    WtReading r = reading(id, b4, b5, b6, -60);
    WtSensor s = {.id = id, .rssi_dbm = -60, .count = 5, .last_tick = last};
    memcpy(s.raw, r.raw, 9);
    return s;
}
/* A place with a sensor in it and, if b5 is not 0, a reading on a date. */
static void put(WtUi* ui, uint8_t slot, uint32_t id, uint8_t b4, uint8_t b5, uint8_t b6, uint8_t month, uint8_t day) {
    ui->cfg.slot[slot] = (WtSlot){.id = id};
    if(b5) wt_config_note(&ui->cfg, id, b4, b5, b6, (WtDate){2026, month, day});
}
enum { A = 0x0F21B4E8, B = 0x0F6A07D5, C = 0x0F3C91A7, D = 0x0F48C2F1, SPARE = 0x0F7E15B9 };
/* The owner's kind of garage: summer set on the car, winter set in the shed,
 * a spare tyre and one loose sensor. */
static void garage(WtUi* ui) {
    memset(ui->cfg.slot, 0, sizeof(ui->cfg.slot));
    put(ui, wt_slot(0, WT_FL), A, 0x86, 0xA7, 0x34, 11, 13);
    put(ui, wt_slot(0, WT_FR), B, 0x86, 0xA7, 0x34, 11, 13);
    put(ui, wt_slot(0, WT_RL), C, 0x86, 0xA7, 0x33, 11, 12);
    put(ui, wt_slot(0, WT_RR), D, 0x86, 0xAB, 0x34, 11, 13);
    put(ui, wt_slot(1, WT_FL), 0x0E5510A1, 0x87, 0x96, 0x44, 4, 2);
    put(ui, wt_slot(1, WT_FR), 0x0E5510B7, 0x87, 0x98, 0x44, 4, 2);
    put(ui, wt_slot(1, WT_RL), 0x0E5511C0, 0, 0, 0, 0, 0);
    put(ui, WT_SLOT_TYRE, 0x0F0A66D3, 0x87, 0xC8, 0x40, 9, 27);
    put(ui, WT_SLOT_SPARE0, SPARE, 0x87, 0x02, 0x49, 11, 12);
}

static void screens(void) {
    WtUi ui;
    wt_ui_init(&ui);
    ui.today = (WtDate){2026, 11, 13};
    shot(&ui, "home");
    for(uint8_t m = 1; m < WT_MENU_COUNT; m++) {
        char name[16];
        ui.menu = m;
        snprintf(name, sizeof(name), "home-%u", m);
        shot(&ui, name);
    }
    ui.menu = 0;

    /* Scan */
    ui.screen = WT_SCR_SCAN_RUN;
    ui.prog = (WtProgress){.phase = WT_PH_SCAN, .burst_no = 2, .burst_max = 7, .lf_on = true, .elapsed_ms = 14200, .limit_ms = 84000};
    for(uint32_t a = 0; a < 3; a++) {
        char name[20];
        ui.anim = a;
        snprintf(name, sizeof(name), a ? "scan-running-%lu" : "scan-running", (unsigned long)a);
        shot(&ui, name);
    }
    ui.prog.heard = true;
    shot(&ui, "scan-found");
    ui.screen = WT_SCR_SCAN_RESULT;
    ui.anim = 0;
    ui.reading = reading(C, 0x86, 0xA7, 0x33, -45);
    shot(&ui, "scan-result");
    garage(&ui);
    shot(&ui, "scan-result-wheel");
    wt_ui_toast(&ui, "Saved as S-RL");
    shot(&ui, "scan-result-saved");
    ui.toast_ticks = 0;
    ui.reading = reading(0x0F0A66D3, 0x87, 0xC8, 0x40, -50);
    shot(&ui, "scan-result-tyre");
    ui.reading = reading(SPARE, 0x87, 0x02, 0x49, -48);
    shot(&ui, "scan-result-spare");
    ui.reading = reading(C, 0xF6, 0xA7, 0x34, -46);
    shot(&ui, "scan-result-ship");
    ui.anim = 2;
    shot(&ui, "scan-result-ship-blink");
    ui.anim = 0;
    ui.reading = reading(C, 0x77, 0x02, 0x49, -92);
    shot(&ui, "scan-result-busy");
    ui.reading = reading(C, 0x66, 0xB1, 0x38, -70);
    shot(&ui, "scan-result-drive");
    ui.reading = reading(C, 0xB7, 0x90, 0x40, -60);
    shot(&ui, "scan-result-unknown");
    memset(ui.cfg.slot, 0, sizeof(ui.cfg.slot));
    ui.reading = reading(0xFFFFFFFF, 0x37, 0xFF, 0xFF, -128);
    shot(&ui, "scan-result-extreme");
    /* every unit, at the largest value a frame can carry and at zero */
    for(uint8_t pu = 0; pu < WT_P_COUNT; pu++)
        for(uint8_t tu = 0; tu < WT_T_COUNT; tu++)
            for(int v = 0; v < 2; v++) {
                char name[32];
                ui.cfg.punit = pu, ui.cfg.tunit = tu;
                ui.reading = reading(C, 0x86, v ? 0xFF : 0, v ? 0xFF : 0, -45);
                snprintf(name, sizeof(name), "unit-%s-%s-%d", wt_punit_name(pu), wt_tunit_name(tu), v);
                shot(&ui, name);
            }
    ui.cfg.punit = WT_P_PSI;
    ui.reading = reading(C, 0x86, 0xA7, 0x33, -45);
    shot(&ui, "scan-result-psi");
    ui.cfg.punit = WT_P_BAR, ui.cfg.tunit = WT_T_C;
    garage(&ui);
    ui.screen = WT_SCR_SCAN_DETAIL;
    shot(&ui, "scan-detail");
    ui.reading = reading(0xFFFFFFFF, 0xFF, 0xFF, 0x00, -128);
    ui.reading.frames = 65535, ui.reading.answer_ms = 83900;
    put(&ui, WT_SLOT_TYRE, 0xFFFFFFFF, 0, 0, 0, 0, 0);
    shot(&ui, "scan-detail-extreme");
    ui.reading = reading(C, 0x86, 0xA7, 0x33, -45);
    ui.screen = WT_SCR_SCAN_FAIL;
    shot(&ui, "scan-fail");
    /* "Not it": the scan that ignores known sensors, and its own way of finding nothing */
    garage(&ui);
    ui.scan_new = true, ui.nrejected = 1, ui.rejected[0] = 0x0E5510A1;
    ui.screen = WT_SCR_SCAN_RUN;
    ui.prog = (WtProgress){.phase = WT_PH_SCAN, .burst_no = 3, .burst_max = 7, .lf_on = true, .elapsed_ms = 31000, .limit_ms = 84000};
    shot(&ui, "scan-new-running");
    for(uint8_t i = 0; i < WT_SLOTS; i++) put(&ui, i, 0xFFFFFFF0u + i, 0, 0, 0, 0, 0);
    ui.nrejected = WT_REJECT_MAX;
    for(uint8_t i = 0; i < WT_REJECT_MAX; i++) ui.rejected[i] = 0x0A000000u + i;
    ui.prog.elapsed_ms = 83900;
    shot(&ui, "scan-new-running-extreme");
    ui.screen = WT_SCR_SCAN_FAIL;
    shot(&ui, "scan-new-fail");
    ui.scan_new = false, ui.nrejected = 0;

    /* Where is it? */
    memset(ui.cfg.slot, 0, sizeof(ui.cfg.slot));
    wt_ui_open_where(&ui, C, WT_SCR_SCAN_RESULT);
    shot(&ui, "where-new");
    garage(&ui);
    wt_ui_open_where(&ui, C, WT_SCR_SCAN_RESULT);
    shot(&ui, "where");
    for(uint8_t r = 1; r < WT_WHERE_COUNT; r++) {
        char name[16];
        ui.where_sel = r;
        snprintf(name, sizeof(name), "where-%u", r);
        shot(&ui, name);
    }
    ui.cfg.on_car = 1;
    wt_ui_open_where(&ui, 0xFFFFFFFF, WT_SCR_SCAN_RESULT);
    put(&ui, wt_slot(1, WT_RR), 0xFFFFFFFF, 0, 0, 0, 0, 0);
    shot(&ui, "where-winter-on-car");
    ui.cfg.on_car = 0;
    garage(&ui);

    /* Which wheel of a set */
    ui.assign_id = C, ui.assign_set = 0, ui.wheel_pick = WT_RL, ui.screen = WT_SCR_ASSIGN;
    shot(&ui, "assign");
    ui.anim = 1;
    shot(&ui, "assign-blink");
    ui.anim = 0;
    ui.wheel_pick = WT_FR;
    shot(&ui, "assign-replace");
    ui.assign_set = 1, ui.wheel_pick = WT_RR;
    shot(&ui, "assign-winter");

    /* Which spare slot */
    ui.assign_id = SPARE, ui.spare_pick = 0, ui.screen = WT_SCR_SPARE_PICK;
    shot(&ui, "spare-pick");
    ui.assign_id = C, ui.spare_pick = 3;
    put(&ui, WT_SLOT_SPARE0 + 3, 0xFFFFFFFF, 0, 0, 0, 0, 0);
    shot(&ui, "spare-pick-full");
    garage(&ui);

    /* Exit ship mode */
    ui.screen = WT_SCR_SHIP_INTRO;
    shot(&ui, "ship-intro");
    ui.ship_flow = true;
    ui.screen = WT_SCR_SCAN_RUN;
    ui.prog = (WtProgress){.phase = WT_PH_SCAN, .burst_no = 1, .burst_max = 7, .lf_on = true, .elapsed_ms = 400, .limit_ms = 84000};
    shot(&ui, "ship-read");
    ui.screen = WT_SCR_SCAN_FAIL;
    shot(&ui, "ship-read-fail");
    ui.reading = reading(C, 0xF6, 0xA7, 0x33, -46);
    ui.screen = WT_SCR_SHIP_CONFIRM;
    shot(&ui, "ship-confirm");
    ui.reading = reading(C, 0x37, 0xA7, 0x33, -46);
    shot(&ui, "ship-confirm-unknown");
    ui.reading = reading(C, 0x86, 0xA7, 0x33, -46);
    ui.screen = WT_SCR_SHIP_ALREADY;
    shot(&ui, "ship-already");
    ui.screen = WT_SCR_SHIP_RUN;
    ui.prog = (WtProgress){.phase = WT_PH_SEND, .burst_no = 1, .burst_max = 6, .lf_on = true, .elapsed_ms = 150, .limit_ms = 72000};
    ui.anim = 1;
    shot(&ui, "ship-sending");
    ui.prog = (WtProgress){.phase = WT_PH_SETTLE, .heard = true, .elapsed_ms = 3100, .limit_ms = 60000};
    shot(&ui, "ship-settle");
    ui.prog = (WtProgress){.phase = WT_PH_CHECK, .burst_no = 2, .burst_max = 7, .lf_on = true, .elapsed_ms = 21000, .limit_ms = 84000};
    ui.anim = 2;
    shot(&ui, "ship-checking");
    ui.prog.heard = true;
    shot(&ui, "ship-check-heard");
    ui.screen = WT_SCR_SHIP_RESULT;
    ui.ship = (WtShip){.before = reading(C, 0xF6, 0xA7, 0x33, -46), .ack = reading(C, 0x75, 0xA7, 0x33, -46),
                       .after = reading(C, 0x86, 0xA7, 0x33, -45), .have_ack = true, .have_after = true,
                       .cmd_bursts = 1, .cmd_frames = 5, .verdict = WT_V_DONE};
    shot(&ui, "ship-done");
    ui.ship.after = reading(C, 0xF6, 0xA7, 0x33, -45), ui.ship.verdict = WT_V_NO_CHANGE;
    shot(&ui, "ship-nochange");
    ui.ship.have_ack = false;
    shot(&ui, "ship-nochange-noack");
    ui.ship.have_after = false, ui.ship.verdict = WT_V_SILENT;
    shot(&ui, "ship-silent");
    ui.ship.have_ack = ui.ship.have_after = true;
    ui.ship.after = reading(C, 0x37, 0xA7, 0x33, -45), ui.ship.verdict = WT_V_OTHER;
    shot(&ui, "ship-other");
    ui.ship.before = ui.ship.after = reading(SPARE, 0x87, 0x02, 0x49, -55);
    ui.ship.ack = reading(SPARE, 0x77, 0x02, 0x49, -55), ui.ship.verdict = WT_V_NO_CHANGE;
    shot(&ui, "ship-still-normal");
    ui.ship_flow = false;
    ui.anim = 0;

    /* Monitor */
    ui.screen = WT_SCR_MONITOR;
    memset(ui.cfg.slot, 0, sizeof(ui.cfg.slot));
    ui.nsensors = 0, ui.now = 600000;
    shot(&ui, "monitor-empty");
    garage(&ui);
    shot(&ui, "monitor-waiting");
    ui.sensors[0] = sensor(A, 0x66, 0xAA, 0x35, 599000);
    ui.sensors[1] = sensor(B, 0x66, 0xAB, 0x36, 480000);
    ui.sensors[2] = sensor(C, 0x66, 0xB1, 0x38, 420000);
    ui.sensors[3] = sensor(D, 0x66, 0xB0, 0x37, 596000);
    ui.nsensors = 4;
    shot(&ui, "monitor");
    /* the phone link: connected, waiting (blinks), Bluetooth off */
    for(uint8_t l = WT_LINK_ON; l >= WT_LINK_WAIT && l <= WT_LINK_FAIL; l = l == WT_LINK_ON ? WT_LINK_WAIT : l == WT_LINK_WAIT ? WT_LINK_FAIL : 0xFF) {
        static const char* const name[] = {"", "monitor-link-wait", "monitor-link", "monitor-link-fail"};
        ui.link = l;
        shot(&ui, name[l]);
    }
    ui.anim = 2;
    ui.link = WT_LINK_WAIT;
    shot(&ui, "monitor-link-wait-blink");
    ui.anim = 0, ui.link = WT_LINK_OFF;
    ui.sensors[4] = sensor(0x1A2B3C4D, 0xB7, 0x90, 0x40, 590000);
    ui.sensors[5] = sensor(SPARE, 0x87, 0x02, 0x49, 100);
    ui.sensors[6] = sensor(0x0F0A66D3, 0x87, 0xC8, 0x40, 300000);
    ui.sensors[7] = sensor(0x0E5510A1, 0x87, 0x96, 0x44, 540000);
    ui.nsensors = 8;
    ui.sensors[2] = sensor(C, 0xF6, 0xA7, 0x33, 420000);
    shot(&ui, "monitor-extra");
    ui.cfg.punit = WT_P_PSI, ui.cfg.tunit = WT_T_F;
    for(int i = 0; i < 4; i++) ui.sensors[i] = sensor(ui.sensors[i].id, 0xF6, 0xFF, 0xFF, 0);
    shot(&ui, "monitor-extreme");
    ui.cfg.punit = WT_P_KPA;
    shot(&ui, "monitor-extreme-kpa");
    ui.cfg.punit = WT_P_BAR, ui.cfg.tunit = WT_T_C;
    ui.sensors[0] = sensor(A, 0x66, 0xAA, 0x35, 599000);
    ui.sensors[1] = sensor(B, 0x66, 0xAB, 0x36, 480000);
    ui.sensors[2] = sensor(C, 0x66, 0xB1, 0x38, 420000);
    ui.sensors[3] = sensor(D, 0x66, 0xB0, 0x37, 596000);
    ui.cfg.on_car = 1; /* the winter set on the car: two of its sensors not placed yet */
    shot(&ui, "monitor-winter");
    ui.cfg.on_car = 0;
    ui.screen = WT_SCR_MONITOR_LIST;
    shot(&ui, "monitor-list");
    ui.list_sel = 7;
    shot(&ui, "monitor-list-scrolled");
    ui.cfg.punit = WT_P_KPA;
    for(int i = 4; i < 8; i++) ui.sensors[i] = sensor(0xFFFFFFFF - (uint32_t)i, 0x66, 0xFF, 0xFF, 0);
    shot(&ui, "monitor-list-extreme");
    ui.cfg.punit = WT_P_BAR;
    ui.nsensors = 0, ui.list_sel = 0;
    shot(&ui, "monitor-list-empty");

    /* My wheels */
    ui.screen = WT_SCR_WHEELS;
    ui.wheels_page = 0, ui.wheels_ids = false;
    shot(&ui, "wheels");
    ui.wheels_ids = true;
    shot(&ui, "wheels-ids");
    ui.wheels_ids = false;
    ui.wheels_page = 1;
    shot(&ui, "wheels-winter");
    ui.wheels_page = 2;
    shot(&ui, "wheels-spares");
    /* ... at their widest: every place full, the largest values, a long month */
    for(uint8_t i = 0; i < WT_SLOTS; i++) put(&ui, i, 0xFFFFFFF0u + i, 0xF6, 0xFF, 0xFF, 9, 30);
    ui.cfg.punit = WT_P_PSI;
    ui.cfg.on_car = 1;
    for(uint8_t pg = 0; pg < WT_WHEELS_PAGES; pg++)
        for(int ids = 0; ids < 2; ids++) {
            char name[32];
            ui.wheels_page = pg, ui.wheels_ids = ids;
            snprintf(name, sizeof(name), "wheels-extreme-%u-%d", pg, ids);
            shot(&ui, name);
        }
    ui.cfg.punit = WT_P_BAR, ui.cfg.on_car = 0, ui.wheels_ids = false;
    memset(ui.cfg.slot, 0, sizeof(ui.cfg.slot));
    for(uint8_t pg = 0; pg < WT_WHEELS_PAGES; pg++) {
        char name[32];
        ui.wheels_page = pg;
        snprintf(name, sizeof(name), "wheels-empty-%u", pg);
        shot(&ui, name);
    }
    garage(&ui);

    /* Settings, fault */
    ui.screen = WT_SCR_SETTINGS;
    for(uint8_t r = 0; r < WT_SET_COUNT; r++) {
        char name[20];
        ui.set_row = r;
        snprintf(name, sizeof(name), r ? "settings-%u" : "settings", r);
        shot(&ui, name);
    }
    for(uint8_t a = 0; a < WT_ASK_COUNT; a++) {
        char name[20];
        ui.cfg.ask = a, ui.cfg.on_car = a & 1u, ui.set_row = WT_SET_ASK;
        snprintf(name, sizeof(name), "settings-ask-%u", a);
        shot(&ui, name);
    }
    ui.cfg.band = WT_BAND_315, ui.set_row = WT_SET_BAND;
    shot(&ui, "settings-band-315");
    ui.cfg.band = WT_BAND_433;
    /* "Which cars?": every make selected in the list, and every make's page at every scroll position */
    for(uint8_t m = 0; m < WT_CARS_COUNT; m++) {
        char name[20];
        ui.screen = WT_SCR_CARS, ui.car_sel = m;
        snprintf(name, sizeof(name), m ? "cars-%u" : "cars", m);
        shot(&ui, name);
        ui.screen = WT_SCR_CAR;
        for(uint8_t l = 0; l == 0 || l + WT_CAR_ROWS <= wt_cars[m].nlines; l++) {
            ui.car_line = l;
            snprintf(name, sizeof(name), "car-%u-%u", m, l);
            shot(&ui, name);
        }
    }
    /* Developer: the warning, the menu with every row selected, and a tool that is not on the card */
    ui.screen = WT_SCR_DRAGONS;
    shot(&ui, "dragons");
    ui.screen = WT_SCR_DEV;
    for(uint8_t r = 0; r < WT_DEV_COUNT; r++) {
        char name[20];
        ui.dev_row = r;
        snprintf(name, sizeof(name), r ? "developer-%u" : "developer", r);
        shot(&ui, name);
    }
    ui.cfg.phone = true;
    wt_ui_toast(&ui, "Not on the SD card");
    shot(&ui, "developer-missing");
    ui.cfg.phone = false, ui.toast_ticks = 0, ui.dev_row = 0;
    ui.screen = WT_SCR_SETTINGS, ui.car_sel = 0, ui.car_line = 0;
    ui.cfg.punit = WT_P_KPA, ui.cfg.tunit = WT_T_F, ui.set_row = 0;
    wt_ui_toast(&ui, "Places forgotten");
    shot(&ui, "settings-kpa-f");
    wt_ui_set_fault(&ui, "LF coil not available");
    shot(&ui, "fault");
    wt_ui_set_fault(&ui, "radio did not start");
    shot(&ui, "fault-radio");
    assert(!failed);
    printf("PASS: %d screens drawn with the device's fonts; nothing runs off the 128 x 64 screen\n", shots);
}

/* The app writes the settings when it finds this set, and clears it. */
static bool saved(WtUi* ui) {
    bool d = ui->dirty;
    ui->dirty = false;
    return d;
}

static void keys(void) {
    WtUi ui;
    wt_ui_init(&ui);
    ui.today = (WtDate){2026, 11, 13};
    WtReading r = reading(C, 0xF6, 0xA7, 0x33, -46);
    WtShip s = {0};

    /* Home: wraps both ways, Back leaves the app. */
    assert(wt_ui_input(&ui, WT_KEY_UP) == WT_CMD_NONE && ui.menu == WT_MENU_SETTINGS);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.menu == WT_MENU_SCAN);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_EXIT);

    /* Scan a sensor that has no place: with "Ask place: if new" (the default)
     * the app asks where it is. Back skips; the result is one key away. */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && ui.screen == WT_SCR_SCAN_RUN && !ui.ship_flow);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_WHERE && ui.assign_id == C && ui.reading.id == C && !saved(&ui));
    assert(wt_where_rows(&ui) == WT_WHERE_NONE && ui.where_sel == 0); /* no "No place" row; the set on the car first */
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && !saved(&ui));
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_DETAIL);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT);

    /* Give it a place: Summer set, rear left. What it just said is kept with the place. */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_WHERE);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_ASSIGN && ui.assign_set == 0 && ui.wheel_pick == WT_FL);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.wheel_pick == WT_RL);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.wheel_pick == WT_RR);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.wheel_pick == WT_RL && !saved(&ui));
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && saved(&ui));
    const WtSlot* rl = &ui.cfg.slot[wt_slot(0, WT_RL)];
    assert(rl->id == C && rl->seen && rl->b4 == 0xF6 && rl->b5 == 0xA7 && rl->date.day == 13 && rl->date.month == 11);
    assert(ui.toast_ticks && !strcmp(ui.toast, "Saved as S-RL"));
    for(int i = 0; i < 10; i++) wt_ui_tick(&ui);
    assert(!ui.toast_ticks);

    /* Scanned again, it has a place: no question, and the reading is refreshed. */
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN);
    r = reading(C, 0x86, 0xA9, 0x35, -45);
    ui.today = (WtDate){2026, 11, 14};
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_RESULT && saved(&ui) && rl->b4 == 0x86 && rl->b5 == 0xA9 && rl->date.day == 14);

    /* Move it: winter set, the wheel chosen there. One sensor, one place, and its reading goes with it. */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_WHERE);
    assert(ui.where_sel == 0 && wt_where_rows(&ui) == WT_WHERE_COUNT); /* on its own row; "No place" is offered now */
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.where_sel == 1);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_ASSIGN && ui.assign_set == 1 && ui.wheel_pick == WT_FL);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_WHERE); /* Back goes one step back */
    wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_input(&ui, WT_KEY_RIGHT);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && saved(&ui));
    assert(!rl->id && ui.cfg.slot[wt_slot(1, WT_FR)].id == C && ui.cfg.slot[wt_slot(1, WT_FR)].b5 == 0xA9);
    assert(!strcmp(ui.toast, "Saved as W-FR"));

    /* Spare tyre: one place, so it is saved at once. */
    wt_ui_input(&ui, WT_KEY_OK);
    assert(ui.where_sel == 1); /* it is in the winter set now */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.where_sel == WT_WHERE_TYRE);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && saved(&ui));
    assert(ui.cfg.slot[WT_SLOT_TYRE].id == C && !ui.cfg.slot[wt_slot(1, WT_FR)].id && !strcmp(ui.toast, "Saved as TYRE"));

    /* Spare sensor: the first free slot is offered; another can be chosen. */
    wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.where_sel == WT_WHERE_SPARE);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SPARE_PICK && ui.spare_pick == 0);
    assert(wt_ui_input(&ui, WT_KEY_UP) == WT_CMD_NONE && ui.spare_pick == 3);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.spare_pick == 0);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.spare_pick == 1);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_WHERE);
    wt_ui_input(&ui, WT_KEY_OK), wt_ui_input(&ui, WT_KEY_DOWN);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && saved(&ui));
    assert(ui.cfg.slot[WT_SLOT_SPARE0 + 1].id == C && !ui.cfg.slot[WT_SLOT_TYRE].id && !strcmp(ui.toast, "Saved as SP2"));

    /* No place: the sensor is forgotten, nothing else is. */
    ui.cfg.slot[wt_slot(0, WT_FL)] = (WtSlot){.id = A};
    wt_ui_input(&ui, WT_KEY_OK);
    assert(ui.where_sel == WT_WHERE_SPARE);
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.where_sel == WT_WHERE_NONE);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.where_sel == 0); /* wraps */
    wt_ui_input(&ui, WT_KEY_UP);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SCAN_RESULT && saved(&ui));
    assert(wt_config_slot_of(&ui.cfg, C) == WT_NO_SLOT && ui.cfg.slot[wt_slot(0, WT_FL)].id == A);
    wt_ui_input(&ui, WT_KEY_BACK);

    /* "Ask place: always" asks about a sensor that has a place too; "never" asks about none. */
    ui.cfg.ask = WT_ASK_ALWAYS;
    r = reading(A, 0x86, 0xA7, 0x34, -50);
    wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_WHERE && ui.assign_id == A && saved(&ui)); /* saved: its reading was refreshed */
    wt_ui_input(&ui, WT_KEY_BACK), wt_ui_input(&ui, WT_KEY_BACK);
    ui.cfg.ask = WT_ASK_NEVER;
    r = reading(D, 0x86, 0xAB, 0x34, -50);
    wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_RESULT && !saved(&ui));
    wt_ui_input(&ui, WT_KEY_BACK);
    ui.cfg.ask = WT_ASK_NEW;

    /* "Not it": a neighbour answered. Left scans again, ignoring it and every sensor with a place. */
    uint32_t skip[WT_SKIP_MAX];
    ui.cfg.slot[wt_slot(1, WT_FL)] = (WtSlot){.id = B};
    r = reading(D, 0x86, 0xAB, 0x34, -61);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && !ui.scan_new && !ui.nrejected);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_WHERE); /* a sensor with no place: asked, and skipped */
    wt_ui_input(&ui, WT_KEY_BACK);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_SCAN && ui.screen == WT_SCR_SCAN_RUN && ui.scan_new);
    assert(wt_ui_skip_list(&ui, skip) == 3); /* A (Summer FL), B (Winter FL) and D, just refused */
    assert(skip[0] == A && skip[1] == B && skip[2] == D);
    /* the next one is refused too; one that is both placed and refused is listed once */
    r = reading(B, 0x86, 0xA7, 0x34, -58);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_RESULT && saved(&ui)); /* it has a place: no question, reading refreshed */
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_SCAN && wt_ui_skip_list(&ui, skip) == 3 && ui.nrejected == 2);
    /* nothing new answers: said plainly, and "Try again" keeps ignoring them */
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_NO_REPLY, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_FAIL && ui.scan_new);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && ui.scan_new && wt_ui_skip_list(&ui, skip) == 3);
    /* a new sensor is found: with "Ask place: if new" the app asks where it is */
    r = reading(C, 0x86, 0xA9, 0x35, -44);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_WHERE && ui.assign_id == C);
    wt_ui_input(&ui, WT_KEY_BACK), wt_ui_input(&ui, WT_KEY_BACK);
    /* more refusals than the list holds: the oldest goes */
    ui.nrejected = WT_REJECT_MAX;
    for(uint8_t i = 0; i < WT_REJECT_MAX; i++) ui.rejected[i] = 0x0A000000u + i;
    ui.screen = WT_SCR_SCAN_RESULT, ui.reading = reading(0x0A0000FF, 0x86, 0xA7, 0x34, -60);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_SCAN && ui.nrejected == WT_REJECT_MAX);
    assert(ui.rejected[0] == 0x0A000001 && ui.rejected[WT_REJECT_MAX - 1] == 0x0A0000FF);
    assert(wt_ui_skip_list(&ui, skip) == 2 + WT_REJECT_MAX && 2 + WT_REJECT_MAX + (WT_SLOTS - 2) <= WT_SKIP_MAX);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL);
    /* a scan started from the menu ignores nobody */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && !ui.scan_new && !ui.nrejected);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL);
    ui.cfg.slot[wt_slot(1, WT_FL)] = (WtSlot){0};

    /* Scan: nothing heard, try again; a fault; Back cancels and a late result is dropped. */
    r = reading(C, 0xF6, 0xA7, 0x33, -46);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_NO_REPLY, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_FAIL);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && ui.screen == WT_SCR_SCAN_RUN);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_FAULT, &r, "LF coil not available");
    assert(ui.screen == WT_SCR_FAULT && !strcmp(ui.fault, "LF coil not available"));
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN);
    uint32_t seq = ui.op_seq;
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL && ui.screen == WT_SCR_HOME);
    wt_ui_scan_done(&ui, seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_HOME);
    /* ... also when the user has already started the next one. */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN);
    wt_ui_scan_done(&ui, seq, WT_R_NO_REPLY, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_RUN);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_CANCELLED, &r, NULL);
    assert(ui.screen == WT_SCR_HOME && !saved(&ui));

    /* Exit ship mode: intro, read, confirm, run, result. The read step never asks for a place. */
    ui.cfg.ask = WT_ASK_ALWAYS;
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SHIP_INTRO);
    ui.scan_new = true; /* left over from a "Not it" scan: the read step of this flow ignores nobody */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && ui.screen == WT_SCR_SCAN_RUN && ui.ship_flow && !ui.scan_new);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_SHIP_CONFIRM);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SHIP && ui.screen == WT_SCR_SHIP_RUN);
    /* The sensor has a place: what it says after the command is kept there. */
    wt_config_assign(&ui.cfg, wt_slot(0, WT_RL), C);
    s.before = r, s.have_after = true, s.after = reading(C, 0x86, 0xA7, 0x33, -45), s.verdict = WT_V_DONE;
    wt_ui_ship_done(&ui, ui.op_seq, WT_R_OK, &s, NULL);
    assert(ui.screen == WT_SCR_SHIP_RESULT && ui.ship.verdict == WT_V_DONE && saved(&ui) && rl->b4 == 0x86);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME);
    ui.cfg.ask = WT_ASK_NEW;
    /* The command is never sent without the confirm screen's OK. */
    wt_ui_input(&ui, WT_KEY_OK), wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &r, NULL);
    assert(ui.screen == WT_SCR_SHIP_CONFIRM);
    for(int k = WT_KEY_UP; k <= WT_KEY_RIGHT; k++) assert(wt_ui_input(&ui, (WtKey)k) == WT_CMD_NONE);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME);
    /* A sensor that is already working: nothing is sent unless the user asks. */
    wt_ui_input(&ui, WT_KEY_OK), wt_ui_input(&ui, WT_KEY_OK);
    WtReading normal = reading(A, 0x86, 0xA7, 0x34, -55);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &normal, NULL);
    assert(ui.screen == WT_SCR_SHIP_ALREADY);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME);
    wt_ui_input(&ui, WT_KEY_OK), wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_OK, &normal, NULL);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_SHIP_CONFIRM);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SHIP);
    seq = ui.op_seq;
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL && ui.screen == WT_SCR_HOME);
    wt_ui_ship_done(&ui, seq, WT_R_OK, &s, NULL);
    assert(ui.screen == WT_SCR_HOME);
    /* The read step finding nothing offers a retry that stays in the flow. */
    wt_ui_input(&ui, WT_KEY_OK), wt_ui_input(&ui, WT_KEY_OK);
    wt_ui_scan_done(&ui, ui.op_seq, WT_R_NO_REPLY, &r, NULL);
    assert(ui.screen == WT_SCR_SCAN_FAIL && wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_SCAN && ui.ship_flow);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL);
    saved(&ui);

    /* Monitor: what the sensors of the set on the car say is kept, and written when the monitor is left. */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.menu == WT_MENU_MONITOR && wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_MONITOR && ui.screen == WT_SCR_MONITOR);
    uint32_t pins[WT_PINS];
    ui.cfg.slot[WT_SLOT_TYRE] = (WtSlot){.id = SPARE};
    wt_config_car_ids(&ui.cfg, pins);
    assert(pins[WT_FL] == A && pins[WT_RL] == C && !pins[WT_FR] && pins[WT_WHEELS] == SPARE);
    WtEngine e;
    WtPort port = {0};
    wt_engine_init(&e, &port);
    e.prog.phase = WT_PH_MONITOR;
    e.sensors[0] = sensor(D, 0x66, 0xB0, 0x37, 1000);
    e.sensors[1] = sensor(B, 0x66, 0xAB, 0x36, 2000);
    e.sensors[2] = sensor(A, 0x66, 0xAA, 0x35, 2200);
    e.nsensors = 3, e.shown = 2500;
    ui.today = (WtDate){2026, 11, 15};
    wt_ui_progress(&ui, &e);
    assert(ui.nsensors == 3 && ui.now == 2500 && wt_ui_unassigned(&ui) == 2 && ui.mon_touched && !ui.dirty);
    assert(ui.cfg.slot[wt_slot(0, WT_FL)].b4 == 0x66 && ui.cfg.slot[wt_slot(0, WT_FL)].b5 == 0xAA &&
           ui.cfg.slot[wt_slot(0, WT_FL)].date.day == 15);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_MONITOR_LIST);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.list_sel == 1);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.list_sel == 2);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.list_sel == 2);
    wt_ui_input(&ui, WT_KEY_UP);
    /* give a sensor heard a place, with the monitor still running */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_WHERE && ui.assign_id == B && wt_ui_busy(&ui));
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_ASSIGN && ui.wheel_pick == WT_FR && wt_ui_busy(&ui));
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_MONITOR_LIST && saved(&ui));
    assert(ui.cfg.slot[wt_slot(0, WT_FR)].id == B && ui.cfg.slot[wt_slot(0, WT_FR)].b5 == 0xAB && wt_ui_unassigned(&ui) == 1);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.screen == WT_SCR_MONITOR);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL && ui.screen == WT_SCR_HOME && !wt_ui_busy(&ui) && saved(&ui));
    /* a monitor that heard nothing new for any place leaves the file alone */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_MONITOR);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_CANCEL && !saved(&ui));

    /* My wheels: a page per set, starting with the one on the car, then the spares; OK shows IDs. */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.menu == WT_MENU_WHEELS && wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_WHEELS);
    assert(ui.wheels_page == 0 && !ui.wheels_ids);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.wheels_page == 1);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.wheels_page == 2);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.wheels_page == 0);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.wheels_page == 2);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.wheels_ids);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME && !saved(&ui));

    /* Settings: units cycle both ways, the set on the car, when to ask, forget; Back saves. */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.menu == WT_MENU_SETTINGS && wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.punit == WT_P_PSI);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.cfg.punit == WT_P_BAR);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.cfg.punit == WT_P_KPA);
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.cfg.tunit == WT_T_F);
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_ON_CAR && wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.on_car == 1);
    wt_config_car_ids(&ui.cfg, pins);
    assert(!pins[WT_FL] && pins[WT_WHEELS] == SPARE); /* the monitor follows the set on the car */
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.cfg.on_car == 0);
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_ASK && wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.ask == WT_ASK_ALWAYS);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.ask == WT_ASK_NEVER);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.ask == WT_ASK_NEW);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.cfg.ask == WT_ASK_NEVER);
    assert(WT_SET_COUNT == 8 && !ui.cfg.phone); /* no phone row here: it is under Developer */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_BAND && ui.cfg.band == WT_BAND_433); /* 433.92 MHz until asked for */
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.band == WT_BAND_315);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.cfg.band == WT_BAND_433);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.cfg.band == WT_BAND_315);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.cfg.band == WT_BAND_433);
    /* "Which cars?": a list of makes, a page for each; nothing here starts anything */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_CARS);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_CARS && ui.car_sel == 0);
    assert(wt_ui_input(&ui, WT_KEY_UP) == WT_CMD_NONE && ui.car_sel == WT_CARS_COUNT - 1u); /* wraps */
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.car_sel == 0);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.car_sel == 1);
    assert(wt_cars[1].nlines > WT_CAR_ROWS); /* a make with more lines than fit, for the scrolling */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_CAR && ui.car_line == 0);
    assert(wt_ui_input(&ui, WT_KEY_UP) == WT_CMD_NONE && ui.car_line == 0);
    for(int i = 0; i < 20; i++) assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE);
    assert(ui.car_line == wt_cars[1].nlines - WT_CAR_ROWS); /* stops with the last line at the bottom */
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_CARS && ui.car_sel == 1);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_CAR && ui.car_line == 0);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.screen == WT_SCR_CARS);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_CARS);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS && ui.set_row == WT_SET_CARS);
    assert(!wt_ui_busy(&ui));
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_FORGET);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.slot[wt_slot(0, WT_FL)].id); /* only OK forgets */
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE);
    for(uint8_t i = 0; i < WT_SLOTS; i++) assert(!ui.cfg.slot[i].id);
    /* Developer: the last row, a warning with a way back, then the menu */
    wt_ui_input(&ui, WT_KEY_DOWN);
    assert(ui.set_row == WT_SET_DEV && WT_SET_DEV == WT_SET_COUNT - 1);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_DRAGONS);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_DRAGONS);
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS && ui.set_row == WT_SET_DEV);
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.screen == WT_SCR_DRAGONS);
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS);
    wt_ui_input(&ui, WT_KEY_OK);
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.screen == WT_SCR_DEV && ui.dev_row == 0);
    assert(WT_DEV_TOOLS == 3 && WT_DEV_COUNT == WT_DEV_TOOLS + WT_DEV_PHONE_ROWS);
#if WT_PHONE_LINK
    assert(!ui.cfg.phone && !saved(&ui)); /* the phone link: off until asked for; a change is saved at once */
    assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && ui.cfg.phone && saved(&ui));
    assert(wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE && !ui.cfg.phone && saved(&ui));
    assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_NONE && ui.cfg.phone && saved(&ui));
    wt_ui_input(&ui, WT_KEY_DOWN);
#endif
    /* a lab tool: only OK starts it, and the app is told which */
    for(uint8_t t = 0; t < WT_DEV_TOOLS; t++) {
        assert(ui.dev_row == WT_DEV_PHONE_ROWS + t);
        assert(wt_ui_input(&ui, WT_KEY_RIGHT) == WT_CMD_NONE && wt_ui_input(&ui, WT_KEY_LEFT) == WT_CMD_NONE);
        ui.launch = 0xFF;
        assert(wt_ui_input(&ui, WT_KEY_OK) == WT_CMD_LAUNCH && ui.launch == t && ui.screen == WT_SCR_DEV);
        wt_ui_input(&ui, WT_KEY_DOWN);
    }
    assert(ui.dev_row == 0); /* wraps */
    assert(wt_ui_input(&ui, WT_KEY_UP) == WT_CMD_NONE && ui.dev_row == WT_DEV_COUNT - 1u);
    assert(!wt_ui_busy(&ui) && !saved(&ui));
    assert(wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_SETTINGS && ui.set_row == WT_SET_DEV);
    assert(wt_ui_input(&ui, WT_KEY_DOWN) == WT_CMD_NONE && ui.set_row == 0); /* wraps */
    assert(!saved(&ui) && wt_ui_input(&ui, WT_KEY_BACK) == WT_CMD_NONE && ui.screen == WT_SCR_HOME && saved(&ui));
    printf("PASS: keys: scan, Not it, ask where, sets and spares, exit ship mode with its confirm step, monitor, My wheels, settings, Which cars, Developer\n");
}


/* Commands from a paired phone: each does what the same keys would. */
static void remote(void) {
    WtUi ui;
    uint8_t c[WT_LINK_REC_LEN], r[WT_LINK_REC_LEN], ack = 0xEE;
    wt_ui_init(&ui);
    garage(&ui);
#define CMD(type, tag) (memset(c, 0, sizeof(c)), c[0] = (type), c[1] = (tag))
    /* scan from home; a second scan while it runs is refused; cancel stops it */
    CMD(WT_RC_SCAN, 7);
    uint32_t seq = ui.op_seq;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_SCAN && ack == WT_ACK_OK && ui.screen == WT_SCR_SCAN_RUN && ui.op_seq == seq + 1);
    assert(!ui.scan_new && !ui.ship_flow);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BUSY);
    CMD(WT_RC_MONITOR, 8);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BUSY);
    CMD(WT_RC_CANCEL, 9);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_CANCEL && ack == WT_ACK_OK && ui.screen == WT_SCR_HOME && ui.op_seq == seq + 2);
    /* "Not it" needs a sensor read first; then it ignores it, once */
    ui.reading = reading(0, 0, 0, 0, 0);
    CMD(WT_RC_SCAN_OTHER, 10);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BAD);
    ui.reading = reading(0x0F999999, 0x86, 0xA7, 0x34, -45);
    ui.screen = WT_SCR_SCAN_RESULT;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_SCAN && ui.scan_new && ui.nrejected == 1 && ui.rejected[0] == 0x0F999999);
    CMD(WT_RC_CANCEL, 11);
    wt_remote(&ui, c, &ack);
    CMD(WT_RC_SCAN_OTHER, 12);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_SCAN && ui.nrejected == 1); /* the same sensor is not listed twice */
    uint32_t skip[WT_SKIP_MAX];
    wt_rec_status(r, 5, &ui, 87);
    assert(r[0] == WT_LINK_STATUS && r[1] == 5 && r[2] == WT_SCR_SCAN_RUN && (r[6] & 4) && r[12] == 87 &&
           r[13] == wt_ui_skip_list(&ui, skip) && r[13] == 1 + 9); /* the one rejected and the nine placed */
    CMD(WT_RC_CANCEL, 13);
    wt_remote(&ui, c, &ack);
    /* the monitor; places can be set while it runs; cancel keeps what it heard */
    CMD(WT_RC_MONITOR, 14);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_MONITOR && ui.screen == WT_SCR_MONITOR && ui.nsensors == 0);
    ui.dirty = false;
    CMD(WT_RC_PLACE, 15);
    c[2] = 0x0F, c[3] = 0x99, c[4] = 0x99, c[5] = 0x99, c[6] = WT_SLOT_SPARE0 + 1;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_OK && ui.dirty &&
           wt_config_slot_of(&ui.cfg, 0x0F999999) == WT_SLOT_SPARE0 + 1 && ui.screen == WT_SCR_MONITOR);
    ui.mon_touched = true, ui.dirty = false;
    CMD(WT_RC_CANCEL, 16);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_CANCEL && ui.screen == WT_SCR_HOME && ui.dirty && !ui.mon_touched);
    /* a sensor moved by the phone answers a "Where is it?" open for it on the Flipper */
    wt_ui_open_where(&ui, 0x0F999999, WT_SCR_SCAN_RESULT);
    assert(ui.screen == WT_SCR_WHERE);
    CMD(WT_RC_PLACE, 17);
    c[2] = 0x0F, c[3] = 0x99, c[4] = 0x99, c[5] = 0x99, c[6] = WT_NO_SLOT;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_OK && ui.screen == WT_SCR_SCAN_RESULT &&
           wt_config_slot_of(&ui.cfg, 0x0F999999) == WT_NO_SLOT);
    c[6] = WT_SLOTS;  /* no such place */
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BAD);
    c[2] = c[3] = c[4] = c[5] = 0, c[6] = 0;  /* no sensor */
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BAD);
    /* the set on the car; exit ship mode is for a later version; unknown commands */
    CMD(WT_RC_ON_CAR, 18);
    c[2] = 1;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_OK && ui.cfg.on_car == 1);
    c[2] = 2;
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_BAD && ui.cfg.on_car == 1);
    CMD(WT_RC_SHIP, 19);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_UNSUPPORTED && ui.screen == WT_SCR_SCAN_RESULT); /* nothing moves */
    CMD(0x99, 20);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_UNSUPPORTED);
    CMD(WT_RC_WHEELS, 21);
    assert(wt_remote(&ui, c, &ack) == WT_CMD_NONE && ack == WT_ACK_OK);
    /* the records: an ack echoes tag and type; a place carries its sensor and reading; a scan its place */
    wt_rec_ack(r, 40, c, WT_ACK_OK);
    assert(r[0] == WT_LINK_ACK && r[1] == 40 && r[2] == 21 && r[3] == WT_RC_WHEELS && r[4] == WT_ACK_OK);
    wt_rec_place(r, 41, &ui.cfg, wt_slot(0, WT_RL));
    assert(r[0] == WT_LINK_PLACE && r[2] == wt_slot(0, WT_RL) && r[3] == WT_SLOTS && wt_rc_id(r + 4) == C && r[8] == 1 &&
           r[9] == 0x86 && r[10] == 0xA7 && r[11] == 0x33 && r[12] == 26 && r[13] == 11 && r[14] == 12 && r[15] == 1);
    wt_rec_place(r, 42, &ui.cfg, WT_SLOT_SPARE0 + 3); /* an empty place */
    assert(r[2] == WT_SLOT_SPARE0 + 3 && wt_rc_id(r + 4) == 0 && r[8] == 0);
    ui.reading = reading(C, 0x86, 0xA7, 0x33, -44);
    ui.scan_new = false;
    wt_rec_scan(r, 43, &ui, WT_R_OK);
    assert(r[0] == WT_LINK_SCAN && r[2] == WT_R_OK && wt_rc_id(r + 3) == C && r[7] == 0x86 && (int8_t)r[12] == -44 &&
           r[13] == wt_slot(0, WT_RL) && r[14] == 6 && r[15] == 0 && r[16] == 3 && r[17] == 0);
    wt_rec_scan(r, 44, &ui, WT_R_NO_REPLY);
    assert(r[2] == WT_R_NO_REPLY && wt_rc_id(r + 3) == 0 && r[13] == WT_NO_SLOT);
#undef CMD
    printf("PASS: phone commands: scan, not it, cancel, monitor, places, set on the car; busy, bad and unsupported answers; records\n");
}

int main(int argc, char** argv) {
    assert(argc == 2);
    out_dir = argv[1];
    screens();
    keys();
    remote();
    return 0;
}
