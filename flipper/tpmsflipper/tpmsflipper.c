// SPDX-License-Identifier: GPL-2.0-or-later
/* TPMSFlipper v1.0.0 — read a Continental/VDO TG1C tyre pressure sensor at
 * the wheel, take a new one out of shipping mode, and watch the four wheels
 * live. For a car owner, not a bench: three functions, plain words, no options
 * that change what is transmitted.
 *
 * This file is only the Flipper side: the view, the keys, the worker thread,
 * the hardware port (receive session, LF coil) and the SD card. What is sent
 * and when is wt_engine.h; what is drawn and what a key does is wt_ui.h. Both
 * run unchanged in the host tests (tests/tpmsflipper_*.c).
 *
 * The receiver, the LF engine and the frame builders are the Continental TG1C
 * LF Lab's own files, linked into this folder, so this app sends exactly what
 * that app sent to real sensors. See docs/ for what was found and
 * what is not known.
 *
 * It also remembers where sensors are: two sets of wheels (Summer, Winter),
 * the spare tyre and four loose spares, each with what the sensor last said
 * and the date. That is wheels.cfg on the SD card.
 *
 * WHAT IT TRANSMITS. 125 kHz from the RFID coil, reaching a few centimetres:
 *   - the stock wake frame, in Scan and around the command;
 *   - the exit-ship-mode frame, only from the "Exit ship mode" flow, only
 *     after a sensor has been read and the user has pressed OK on the confirm
 *     screen, and only until that sensor's first answer.
 * The Live monitor only receives: on 433.92 MHz, or on 315 MHz if Settings >
 * Radio band says so (for North American sensors; untested).
 *
 * THE PHONE LINK. With "Phone link" on in Settings, the app runs a Bluetooth
 * LE service for as long as it is open (wt_link_ble.c): the monitor's
 * readings go to the phone (wt_link.h), and a phone paired with a PIN can
 * start what the keys start, scan, the monitor and places (wt_remote.h).
 * A phone's command goes through the same screen logic as a key press, so
 * the gates on what is transmitted are the same.
 */
#include "wt_ui.h"
#include "wt_log.h"
#include "wt_link_ble.h"
#include "wt_remote.h"
#include "tg1c_lf_dma.h"
#include "tpms_session.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <loader/loader.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#define DATA_DIR EXT_PATH("apps_data/" WT_APP_ID)
#define DEV_DIR DATA_DIR "/dev" /* the lab tools, kept out of the Apps menu */
#define CONFIG_PATH DATA_DIR "/wheels.cfg"
#define LOG_PATH DATA_DIR "/log.csv"
#define WORKER_STACK (5 * 1024)
#define TICK_MS 250u
#define LINK_PER_TICK 4u /* records sent each 250 ms: the stack takes them without waiting */
#define LINK_STACK (2 * 1024)
#define LINK_STATUS_MS 2000u /* the status again at least this often, changed or not */
#define EVT_REMOTE 1u /* view dispatcher custom event: commands are waiting */

_Static_assert(TG1C_MAX_SLOTS >= TG1C_SLOTS_A, "LF engine holds a wake-shaped frame");

typedef enum { OpNone = 0, OpScan, OpShip, OpMonitor } Op;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* view;
    NotificationApp* notif;
    FuriTimer* timer;
    FuriMutex* save_mutex; /* the app thread and the worker both save settings */

    FuriThread* worker;
    volatile bool cancel;
    /* Set by the app thread before the worker starts; the worker only reads them. */
    Op op;
    uint32_t op_seq;
    WtReading op_before;
    uint32_t op_skip[WT_SKIP_MAX]; /* a "Not it" scan ignores these */
    uint8_t op_nskip;

    /* The phone link. link and link_thread are changed on the app thread only;
     * the link thread does all the sending. */
    WtLinkBle* link;
    FuriThread* link_thread;
    volatile bool link_stop;
    FuriMessageQueue* cmds; /* commands from the phone, BLE thread -> app thread */
    FuriMessageQueue* acks; /* acks, app thread -> link thread */
    volatile bool places_pending; /* send the thirteen places */
    volatile int8_t scan_result; /* a scan's WtResult to report, or -1 */
    /* Link thread only. */
    WtLinkTrack track;
    uint8_t link_state, place_next;
    uint8_t status_last[WT_LINK_REC_LEN];
    uint32_t status_at;
    uint8_t held[WT_LINK_REC_LEN]; /* a record the stack refused, sent first next time */
    bool has_held;

    /* Worker-only. */
    TpmsSession* session;
    WtPort port;
    WtEngine engine;
} App;

/* ----- hardware port -------------------------------------------------------- */

static void on_frame(const TpmsFrame* frame, float rssi_dbm, void* ctx) {
    App* app = ctx;
    if(frame->raw_len < WT_FRAME_LEN) return;
    wt_engine_on_frame(&app->engine, frame->id, frame->raw, (int)(rssi_dbm < 0 ? rssi_dbm - 0.5f : rssi_dbm + 0.5f));
}
static uint32_t port_tick_ms(void* ctx) {
    UNUSED(ctx);
    return furi_get_tick(); /* 1 kHz kernel tick */
}
static void port_delay_tick(void* ctx) {
    UNUSED(ctx);
    furi_delay_tick(1);
}
static bool port_cancelled(void* ctx) {
    return ((App*)ctx)->cancel;
}
static bool port_rx_start(void* ctx, bool wide) {
    App* app = ctx;
    /* The lab's receiver: 325 kHz FSK preset, on 433.92 MHz unless Settings >
     * Radio band says 315. With the coil in use, its 512-interval stream;
     * receive-only, the full-size one. */
    uint8_t band = WT_BAND_433;
    with_view_model(app->view, WtUi * ui, { band = ui->cfg.band; }, false);
    app->session = wide ? tpms_session_alloc() : tpms_session_alloc_bench();
    tpms_session_set_frame_callback(app->session, on_frame, app);
    if(!tpms_session_start(app->session, wt_band_hz(band), TpmsModulationFsk)) return false;
    tpms_session_set_vdo_only(app->session, true);
    return true;
}
static void port_rx_pump(void* ctx, uint32_t timeout_ms) {
    tpms_session_pump(((App*)ctx)->session, timeout_ms);
}
static int port_rx_rssi(void* ctx) {
    float dbm = tpms_session_get_rssi(((App*)ctx)->session);
    return (int)(dbm < 0 ? dbm - 0.5f : dbm + 0.5f);
}
static void port_rx_stop(void* ctx) {
    App* app = ctx;
    if(!app->session) return;
    tpms_session_free(app->session); /* stops the radio if it is running */
    app->session = NULL;
}
static bool port_lf_prepare(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us) {
    UNUSED(ctx);
    return tg1c_lf_dma_prepare(slots, nslots, half_us, lead_us);
}
static bool port_lf_start(void* ctx) {
    UNUSED(ctx);
    return tg1c_lf_dma_start();
}
static bool port_lf_done(void* ctx) {
    UNUSED(ctx);
    return tg1c_lf_dma_done();
}
static bool port_lf_error(void* ctx) {
    UNUSED(ctx);
    return tg1c_lf_dma_error();
}
static void port_lf_stop(void* ctx) {
    UNUSED(ctx);
    tg1c_lf_dma_stop();
}
static void port_lf_free(void* ctx) {
    UNUSED(ctx);
    tg1c_lf_dma_free();
}
static WtDate today(void) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    return (WtDate){dt.year, dt.month, dt.day};
}
/* The engine's progress, four times a second: copy it to the screen, and tell
 * the monitor which sensors must stay in its table (the set on the car and
 * the spare tyre). */
static void port_progress(void* ctx, WtEngine* e) {
    App* app = ctx;
    uint32_t pins[WT_PINS];
    WtDate now = today();
    with_view_model(
        app->view,
        WtUi * ui,
        {
            ui->today = now;
            wt_ui_progress(ui, e);
            wt_config_car_ids(&ui->cfg, pins);
        },
        true);
    wt_engine_pin(e, pins);
}

/* ----- SD card -------------------------------------------------------------- */

static void config_load(WtConfig* cfg) {
    char text[WT_CONFIG_TEXT_MAX + 64];
    size_t n = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, CONFIG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) n = storage_file_read(f, text, sizeof(text));
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    wt_config_parse(cfg, text, n);
}
static void config_save(const WtConfig* cfg) {
    char text[WT_CONFIG_TEXT_MAX];
    size_t n = wt_config_write(cfg, text, sizeof(text));
    if(!n) return;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, DATA_DIR);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, CONFIG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) storage_file_write(f, text, n);
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}
/* One line onto the end of log.csv, the column names first if the file is new.
 * Only called with the receiver stopped. */
static void log_append(const char* line, size_t n) {
    if(!n) return;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, DATA_DIR);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(storage_file_size(f) == 0) storage_file_write(f, WT_LOG_HEADER, strlen(WT_LOG_HEADER));
        storage_file_write(f, line, n);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}
static void now_text(char* out, size_t cap) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

/* ----- worker --------------------------------------------------------------- */

/* If the settings have changed, write them. Never called with the receiver
 * and the coil both in use: from the app thread, or from the worker once its
 * operation has ended. */
static void save_if_dirty(App* app) {
    WtConfig cfg = {0};
    bool dirty = false;
    furi_mutex_acquire(app->save_mutex, FuriWaitForever);
    with_view_model(
        app->view,
        WtUi * ui,
        {
            dirty = ui->dirty;
            ui->dirty = false;
            cfg = ui->cfg;
        },
        false);
    if(dirty) {
        config_save(&cfg);
        app->places_pending = true; /* the phone's copy of the places is out of date */
    }
    furi_mutex_release(app->save_mutex);
}
/* The place a sensor has, for the log: "S-RL", "TYRE", or nothing. */
static void place_text(App* app, uint32_t id, char* out, size_t cap) {
    uint8_t slot = WT_NO_SLOT;
    with_view_model(app->view, WtUi * ui, { slot = wt_config_slot_of(&ui->cfg, id); }, false);
    if(slot == WT_NO_SLOT)
        out[0] = 0;
    else
        wt_slot_short(out, cap, slot);
}

static int32_t worker(void* ctx) {
    App* app = ctx;
    WtEngine* e = &app->engine;
    char when[32], line[WT_LOG_LINE_MAX], place[8];
    now_text(when, sizeof(when));
    WtDate date = today();

    if(app->op == OpScan) {
        WtReading r;
        WtResult res = wt_engine_scan(e, 0, app->op_skip, app->op_nskip, &r);
        place_text(app, r.id, place, sizeof(place));
        if(res != WT_R_CANCELLED)
            log_append(line, wt_log_scan(line, sizeof(line), when, res, &r, place, app->op_nskip != 0));
        with_view_model(
            app->view,
            WtUi * ui,
            {
                ui->today = date;
                wt_ui_scan_done(ui, app->op_seq, res, &r, e->fault);
            },
            true);
        save_if_dirty(app);
        if(res != WT_R_CANCELLED) app->scan_result = (int8_t)res;
        if(res == WT_R_OK)
            notification_message(app->notif, &sequence_single_vibro);
        else if(res != WT_R_CANCELLED)
            notification_message(app->notif, &sequence_error);
    } else if(app->op == OpShip) {
        WtShip s;
        WtResult res = wt_engine_ship(e, &app->op_before, &s);
        place_text(app, s.before.id, place, sizeof(place));
        /* Logged even when cancelled: the command may have been sent. */
        if(s.cmd_frames) log_append(line, wt_log_ship(line, sizeof(line), when, res, &s, place));
        with_view_model(
            app->view,
            WtUi * ui,
            {
                ui->today = date;
                wt_ui_ship_done(ui, app->op_seq, res, &s, e->fault);
            },
            true);
        save_if_dirty(app);
        if(res == WT_R_OK && s.verdict == WT_V_DONE)
            notification_message(app->notif, &sequence_success);
        else if(res != WT_R_CANCELLED)
            notification_message(app->notif, &sequence_error);
    } else if(app->op == OpMonitor) {
        WtResult res = wt_engine_monitor(e);
        with_view_model(app->view, WtUi * ui, { wt_ui_monitor_done(ui, app->op_seq, res, e->fault); }, true);
    }
    return 0;
}
/* Stop whatever is running and wait until the carrier and the radio are off. */
static void worker_join(App* app) {
    if(app->worker) {
        app->cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
    app->cancel = false;
}
static void worker_start(
    App* app, Op op, uint32_t seq, const WtReading* before, const uint32_t* skip, uint8_t nskip) {
    worker_join(app);
    app->op = op;
    app->op_seq = seq;
    if(before) app->op_before = *before;
    app->op_nskip = nskip;
    if(nskip) memcpy(app->op_skip, skip, nskip * sizeof(uint32_t));
    app->worker = furi_thread_alloc_ex("TPMSFlipper", WORKER_STACK, worker, app);
    furi_thread_start(app->worker);
}

/* ----- phone link ------------------------------------------------------------- */

/* One record to the phone, numbered as it goes. False if the stack refused it. */
static bool link_send(App* app, uint8_t rec[WT_LINK_REC_LEN]) {
    rec[1] = app->track.seq;
    if(!wt_link_ble_send(app->link, rec)) return false;
    app->track.seq++;
    return true;
}
static bool link_send_or_hold(App* app, uint8_t rec[WT_LINK_REC_LEN]) {
    if(link_send(app, rec)) return true;
    memcpy(app->held, rec, WT_LINK_REC_LEN);
    app->has_held = true;
    return false;
}

/* Every 250 ms: what is owed to the phone, most urgent first, at most
 * LINK_PER_TICK records: a refused record, acks, a scan result, the status
 * when it changes, the places when they change, then the monitor's records. */
static void link_tick(App* app) {
    WtSensor sensors[WT_MAX_SENSORS];
    uint8_t places[WT_MAX_SENSORS] = {0}, status[WT_LINK_REC_LEN], scan[WT_LINK_REC_LEN], n = 0;
    WtConfig cfg;
    bool running = false;
    int8_t result = app->scan_result;
    uint8_t state = (uint8_t)wt_link_ble_state(app->link);
    uint8_t battery = furi_hal_power_get_pct();
    with_view_model(
        app->view,
        WtUi * ui,
        {
            ui->link = state;
            n = ui->nsensors;
            memcpy(sensors, ui->sensors, sizeof(sensors));
            for(uint8_t i = 0; i < n; i++) places[i] = wt_config_slot_of(&ui->cfg, sensors[i].id);
            cfg = ui->cfg;
            running = wt_rc_running(ui);
            wt_rec_status(status, 0, ui, battery);
            if(result >= 0) wt_rec_scan(scan, 0, ui, (uint8_t)result);
        },
        false);
    bool on = state == WT_LINK_ON;
    if(on && app->link_state != WT_LINK_ON) { /* a phone has just come: everything */
        app->places_pending = true;
        app->place_next = 0;
        memset(app->status_last, 0, sizeof(app->status_last));
    }
    app->link_state = state;
    if(!on) {
        app->has_held = false;
        wt_link_due(&app->track, false, 0, NULL, 0, NULL, &cfg, NULL, 0, NULL);
        return;
    }
    uint8_t budget = LINK_PER_TICK, rec[WT_LINK_REC_LEN];
    if(app->has_held) {
        if(!link_send(app, app->held)) return;
        app->has_held = false;
        budget--;
    }
    while(budget && furi_message_queue_get(app->acks, rec, 0) == FuriStatusOk) {
        budget--;
        if(!link_send_or_hold(app, rec)) return;
    }
    if(budget && result >= 0) {
        app->scan_result = -1;
        budget--;
        if(!link_send_or_hold(app, scan)) return;
    }
    uint32_t now = furi_get_tick();
    if(budget && (memcmp(status + 2, app->status_last + 2, WT_LINK_REC_LEN - 2) || now - app->status_at >= LINK_STATUS_MS)) {
        memcpy(app->status_last, status, sizeof(status));
        app->status_at = now;
        budget--;
        if(!link_send_or_hold(app, status)) return;
    }
    if(app->places_pending && budget) {
        if(app->place_next >= WT_SLOTS) app->place_next = 0;
        while(budget && app->place_next < WT_SLOTS) {
            wt_rec_place(rec, 0, &cfg, app->place_next);
            budget--;
            if(!link_send_or_hold(app, rec)) return;
            app->place_next++;
        }
        if(app->place_next >= WT_SLOTS) app->places_pending = false, app->place_next = 0;
    }
    if(!budget) return;
    uint8_t out[LINK_PER_TICK][WT_LINK_REC_LEN], rows[LINK_PER_TICK];
    app->track.battery = battery;
    app->track.flags = running ? 1u : 0u;
    uint8_t k = wt_link_due(&app->track, true, now, sensors, n, places, &cfg, out, budget, rows);
    for(uint8_t i = 0; i < k; i++)
        if(!wt_link_ble_send(app->link, out[i])) {
            for(uint8_t j = i; j < k; j++) wt_link_unsent(&app->track, rows[j]);
            break;
        }
}
static int32_t link_thread(void* ctx) {
    App* app = ctx;
    while(!app->link_stop) {
        link_tick(app);
        furi_delay_ms(TICK_MS);
    }
    return 0;
}
/* From the BLE event thread: keep the command, wake the app thread. */
static void on_remote(const uint8_t cmd[WT_LINK_REC_LEN], void* ctx) {
    App* app = ctx;
    if(furi_message_queue_put(app->cmds, cmd, 0) == FuriStatusOk)
        view_dispatcher_send_custom_event(app->vd, EVT_REMOTE);
}
/* Start or stop the link. App thread only. Starting restarts the radio
 * co-processor (about a second). */
static void link_set(App* app, bool on) {
    if(on && !app->link) {
        wt_link_track_init(&app->track);
        app->link_state = WT_LINK_OFF;
        app->has_held = false;
        app->scan_result = -1;
        app->link = wt_link_ble_start(on_remote, app);
        if(app->link) {
            app->link_stop = false;
            app->link_thread = furi_thread_alloc_ex("TPMSFlipperLink", LINK_STACK, link_thread, app);
            furi_thread_start(app->link_thread);
        }
        with_view_model(app->view, WtUi * ui, { ui->link = app->link ? WT_LINK_WAIT : WT_LINK_FAIL; }, true);
    } else if(!on && app->link) {
        app->link_stop = true;
        furi_thread_join(app->link_thread);
        furi_thread_free(app->link_thread);
        app->link_thread = NULL;
        wt_link_ble_stop(app->link); /* the Flipper's own Bluetooth back */
        app->link = NULL;
        with_view_model(app->view, WtUi * ui, { ui->link = WT_LINK_OFF; }, true);
    } else if(!on) {
        with_view_model(app->view, WtUi * ui, { ui->link = WT_LINK_OFF; }, true);
    }
}

/* Developer: start one of the lab tools and leave. A tool is an app of its
 * own; the Flipper's loader starts it once this app has closed. Looked for in
 * this app's dev folder, then where a tool installed by itself would be. */
static void dev_launch(App* app) {
    static const char* const dirs[] = {DEV_DIR, EXT_PATH("apps/Sub-GHz")};
    uint8_t tool = 0;
    bool found = false;
    with_view_model(app->view, WtUi * ui, { tool = ui->launch; }, false);
    if(tool >= WT_DEV_TOOLS) return;
    FuriString* path = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    for(size_t i = 0; i < COUNT_OF(dirs) && !found; i++) {
        furi_string_printf(path, "%s/%s", dirs[i], wt_dev_tools[tool].fap);
        found = storage_file_exists(storage, furi_string_get_cstr(path));
    }
    furi_record_close(RECORD_STORAGE);
    if(found) {
        Loader* loader = furi_record_open(RECORD_LOADER);
        loader_enqueue_launch(loader, furi_string_get_cstr(path), NULL, LoaderDeferredLaunchFlagGui);
        furi_record_close(RECORD_LOADER);
        view_dispatcher_stop(app->vd);
    } else {
        with_view_model(app->view, WtUi * ui, { wt_ui_toast(ui, "Not on the SD card"); }, true);
    }
    furi_string_free(path);
}

/* What a key or a phone's command asks the app to do. */
static void run_cmd(App* app, WtCmd cmd, uint32_t seq, const WtReading* before, const uint32_t* skip, uint8_t nskip) {
    switch(cmd) {
    case WT_CMD_SCAN:
        worker_start(app, OpScan, seq, NULL, skip, nskip);
        break;
    case WT_CMD_SHIP:
        worker_start(app, OpShip, seq, before, NULL, 0);
        break;
    case WT_CMD_MONITOR:
        worker_start(app, OpMonitor, seq, NULL, NULL, 0);
        break;
    case WT_CMD_CANCEL:
        app->cancel = true; /* the engine has the carrier off within a few ms */
        break;
    case WT_CMD_EXIT:
        view_dispatcher_stop(app->vd);
        break;
    case WT_CMD_LAUNCH:
        dev_launch(app);
        break;
    default:
        break;
    }
}

/* Commands from the phone, on the app thread, one after another. */
static bool on_custom_event(void* ctx, uint32_t event) {
    App* app = ctx;
    if(event != EVT_REMOTE) return false;
    uint8_t c[WT_LINK_REC_LEN], rec[WT_LINK_REC_LEN];
    while(furi_message_queue_get(app->cmds, c, 0) == FuriStatusOk) {
        uint8_t ack = WT_ACK_OK, nskip = 0;
        WtCmd cmd = WT_CMD_NONE;
        uint32_t seq = 0, skip[WT_SKIP_MAX];
        WtReading before;
        WtDate date = today();
        with_view_model(
            app->view,
            WtUi * ui,
            {
                ui->today = date;
                cmd = wt_remote(ui, c, &ack);
                seq = ui->op_seq;
                before = ui->reading;
                if(cmd == WT_CMD_SCAN && ui->scan_new && !ui->ship_flow) nskip = wt_ui_skip_list(ui, skip);
            },
            true);
        wt_rec_ack(rec, 0, c, ack);
        furi_message_queue_put(app->acks, rec, 0);
        if(c[0] == WT_RC_WHEELS) app->places_pending = true;
        run_cmd(app, cmd, seq, &before, skip, nskip);
    }
    save_if_dirty(app);
    return true;
}

/* ----- view ----------------------------------------------------------------- */

static void view_draw(Canvas* canvas, void* model) {
    wt_ui_draw(canvas, model);
}
/* Keys arrive on the app thread. The screen logic answers with a command. */
static bool view_input(InputEvent* event, void* ctx) {
    App* app = ctx;
    bool back = event->key == InputKeyBack;
    bool step = event->key == InputKeyUp || event->key == InputKeyDown;
    if(!(event->type == InputTypeShort || (event->type == InputTypeLong && back) ||
         (event->type == InputTypeRepeat && step)))
        return true;
    WtKey key = event->key == InputKeyUp    ? WT_KEY_UP :
                event->key == InputKeyDown  ? WT_KEY_DOWN :
                event->key == InputKeyLeft  ? WT_KEY_LEFT :
                event->key == InputKeyRight ? WT_KEY_RIGHT :
                event->key == InputKeyOk    ? WT_KEY_OK :
                                              WT_KEY_BACK;
    WtCmd cmd = WT_CMD_NONE;
    uint32_t seq = 0;
    WtReading before;
    uint32_t skip[WT_SKIP_MAX];
    uint8_t nskip = 0;
    bool phone_was = false, phone = false;
    WtDate date = today();
    with_view_model(
        app->view,
        WtUi * ui,
        {
            ui->today = date;
            phone_was = ui->cfg.phone;
            cmd = wt_ui_input(ui, key);
            phone = ui->cfg.phone;
            seq = ui->op_seq;
            before = ui->reading;
            if(cmd == WT_CMD_SCAN && ui->scan_new && !ui->ship_flow) nskip = wt_ui_skip_list(ui, skip);
        },
        true);
    run_cmd(app, cmd, seq, &before, skip, nskip);
    if(phone != phone_was) link_set(app, WT_PHONE_LINK && phone); /* Settings > Developer > Phone link */
    save_if_dirty(app); /* a place given, a setting changed, the monitor left */
    return true; /* every key is handled here, Back included */
}
/* Four times a second: animation and the two-second notes. While an operation
 * runs its own progress redraws the screen, so this does not add a second
 * redraw on top: the display is then updated as often as in the lab app. */
static void tick(void* ctx) {
    App* app = ctx;
    bool redraw = false;
    with_view_model(
        app->view,
        WtUi * ui,
        {
            wt_ui_tick(ui);
            redraw = !wt_ui_busy(ui);
        },
        redraw);
}

/* ----- app lifecycle -------------------------------------------------------- */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->port = (WtPort){
        .ctx = app,
        .tick_ms = port_tick_ms,
        .delay_tick = port_delay_tick,
        .cancelled = port_cancelled,
        .rx_start = port_rx_start,
        .rx_pump = port_rx_pump,
        .rx_rssi = port_rx_rssi,
        .rx_stop = port_rx_stop,
        .lf_prepare = port_lf_prepare,
        .lf_start = port_lf_start,
        .lf_done = port_lf_done,
        .lf_error = port_lf_error,
        .lf_stop = port_lf_stop,
        .lf_free = port_lf_free,
        .progress = port_progress,
    };
    wt_engine_init(&app->engine, &app->port);

    app->gui = furi_record_open(RECORD_GUI);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(WtUi));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, view_draw);
    view_set_input_callback(app->view, view_input);
    with_view_model(
        app->view,
        WtUi * ui,
        {
            wt_ui_init(ui);
            config_load(&ui->cfg);
        },
        false);
    view_dispatcher_add_view(app->vd, 0, app->view);
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, on_custom_event);
    app->cmds = furi_message_queue_alloc(8, WT_LINK_REC_LEN);
    app->acks = furi_message_queue_alloc(8, WT_LINK_REC_LEN);
    app->scan_result = -1;

    app->save_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->timer = furi_timer_alloc(tick, FuriTimerTypePeriodic, app);
    return app;
}

static void app_free(App* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    worker_join(app);
    link_set(app, false); /* before the app's code goes: the profile's callbacks live here */
    furi_message_queue_free(app->cmds);
    furi_message_queue_free(app->acks);
    furi_mutex_free(app->save_mutex);
    view_dispatcher_remove_view(app->vd, 0);
    view_free(app->view);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t tpmsflipper_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    notification_message(app->notif, &sequence_display_backlight_enforce_on);
    furi_timer_start(app->timer, furi_ms_to_ticks(TICK_MS));
    bool phone = false;
    with_view_model(app->view, WtUi * ui, { phone = ui->cfg.phone; }, false);
    link_set(app, WT_PHONE_LINK && phone); /* never, in a build without the phone link */
    view_dispatcher_switch_to_view(app->vd, 0);
    view_dispatcher_run(app->vd);
    notification_message(app->notif, &sequence_display_backlight_enforce_auto);
    app_free(app);
    return 0;
}
