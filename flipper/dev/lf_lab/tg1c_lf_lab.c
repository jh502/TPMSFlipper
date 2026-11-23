// SPDX-License-Identifier: GPL-2.0-or-later
/* Continental TG1C LF Lab v0.3.6 — bench-first CONNECT probe with on-device
 * reply decode (plan v3), from v0.3.0 a single-bit sweep of the wake frame, from
 * v0.3.1 a dilution test of it, and from v0.3.2 an offset test and a scan of
 * candidate frames.
 *
 * A separate app (appid continental_tg1c_lf) alongside tpms_bridge. It sends a
 * selected LF frame as a burst train with the receiver live, decodes TG1C
 * telemetry (the control signal) and looks for the NXP bootloader's NRZ reply.
 *
 * This file is only the Flipper side: the option list, the result screen, the
 * hardware port (receive session, LF envelope engine) and the SD writes. The
 * session itself — preflight, burst trains, waits, stop conditions, log — is
 * tg1c_probe.h, which the host tests run unchanged against a simulated port.
 *
 * SCOPE. Up to v0.2.4 the app was read-only (plan v3 §2). On 11 November 2026
 * the owner removed that limit for the bench spare 0F61C9F5 and the Bad sensor
 * 0F3A6CB8 (see tg1c_frame.h). v0.3.0 adds the Bit sweep, which sends the wake
 * frame with one data bit flipped; what such a frame does to a sensor is not
 * known. v0.3.1 adds the Dilution test, which mixes the stock wake with one of
 * those one-bit variants in a fixed ratio. v0.3.2 adds the Offset test (the
 * same mix, the stock frame's place in the cycle moved per trial) and the Scan,
 * which sends wake-shaped frames that differ from the wake in several bits;
 * what those do to a sensor is not known either. v0.3.3 adds the Park command:
 * payload E, the frame a workshop tool sends to take a Continental sensor out
 * of shipping mode (read from that tool's firmware), between two wakes, and a
 * Target option that makes the Bad sensor the control. The Good sensors on the car are not
 * covered: keep them out of LF range. Still enforced by tg1c_seq.h, which
 * decides every transmission:
 *  - No builder or path for bootloader commands 0x02-0x06 exists in the app.
 *  - Only the target's telemetry counts as a control: the spare 0F61C9F5
 *    unless the Target option is set to the Bad sensor 0F3A6CB8.
 *  - CONNECT is never auto-sent. The default payload is the stock wake; a
 *    CONNECT needs an explicit payload selection and Start. In a Sequence it
 *    is sent only after the spare answered the wake in the same session.
 *  - The first CRC-valid bootloader reply stops all LF transmission at once.
 *  - Any telemetry status pair outside {87/01, 87/02} stops all LF
 *    transmission at once; the app then only receives, until 10 s pass with
 *    no frame (v0.3.6; 120 s at most, and 120 s after a bootloader reply).
 *
 * Memory (plan v3 §3): the session refuses to start unless the heap holds the
 * largest envelope, the 4 KiB evidence ring and a 16 KiB reserve. Nothing is
 * written to SD while the receiver is live.
 */
#include "tg1c_probe.h"
#include "tg1c_lf_dma.h"
#include "tpms_session.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view.h>
#include <storage/storage.h>

#define DATA_DIR EXT_PATH("apps_data/continental_tg1c_lf")

_Static_assert(sizeof(TG1cRawRecord) == sizeof(TpmsCaptureRecord), "TPRAW02 record layout");
_Static_assert(TG1C_MAX_SLOTS == TG1C_MAX_FRAME_SLOTS, "LF engine bound must equal largest frame");

typedef enum {
    ViewConfig = 0,
    ViewResult,
} ViewId;

/* Result screen model — filled by the worker, drawn on the GUI thread. */
typedef struct {
    char title[28];
    char line[6][32]; /* v0.3.6: six rows; row 2 is the latest frame decoded */
} ResultModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    VariableItemList* config;
    /* Rows added to the option list so far, and the index of the Start row.
     * Counted as the list is built (v0.3.4): v0.3.3 added the Target row and
     * left a hand-kept row number behind, so Start did nothing and OK on the
     * row above it began a session. */
    uint32_t cfg_rows, cfg_start_row;
    View* result;

    /* Operator options. */
    uint8_t sel; /* TG1cSelection */
    uint8_t burst_idx, lead_idx, gap_idx;
    uint8_t rxf; /* TG1cRxFreqSel */
    uint8_t sweep_range; /* 0 = bits 0-11 .. 3 = bits 36-47 */
    uint8_t dilute_fill; /* 0 = bit 3, 1 = bit 40 */
    uint8_t scan_set; /* TG1cScanSet */
    uint8_t scan_group; /* choice 0..3 = 1, 2, 4, 8 candidates per burst */
    uint8_t scan_from; /* choice 0..15 = first candidate index 0x00, 0x10, .. 0xF0 */
    uint8_t target; /* TG1cTarget: the spare (default) or the Bad sensor */

    FuriThread* worker;
    volatile bool cancel;

    /* Worker-only state. */
    TpmsSession* session;
    TG1cPort port;
    TG1cProbe probe; /* about 27 KiB: 124 log rows, the telemetry log and the reply ring, part of this heap block */
} App;

static const char* const kSelName[TG1C_SEL_COUNT] = {"A wake", "B 5A42", "C 615E?", "D 615E~?", "Sequence", "Bit sweep", "Dilution", "Offset", "Scan", "Park cmd"};
static const char* const kTargetName[TG1C_TARGETS] = {"Spare", "Bad"};
static const char* const kFillName[TG1C_DIL_FILLS] = {"bit 3", "bit 40"};
static const char* const kScanSetName[TG1C_SCAN_SETS] = {"Byte", "Nibble"};
static const char* const kScanGroupName[TG1C_SCAN_GROUP_OPTIONS] = {"1", "2", "4", "8"};
static const char* const kScanFromName[TG1C_SCAN_FROM_OPTIONS] = {"00", "10", "20", "30", "40", "50", "60", "70",
                                                                  "80", "90", "A0", "B0", "C0", "D0", "E0", "F0"};
static const char* const kSweepName[TG1C_SWEEP_RANGES] = {"0-11", "12-23", "24-35", "36-47"};
static const char* const kBurstName[TG1C_BURST_OPTIONS] = {"1 s", "5 s", "10 s"};
static const char* const kLeadName[TG1C_LEAD_OPTIONS] = {"0", "2.048ms", "6 ms", "12 ms"};
static const char* const kGapName[TG1C_GAP_OPTIONS] = {"5 ms", "8 ms", "20 ms", "40 ms"};
static const char* const kRxfName[TG1C_RXF_COUNT] = {"Alt", "434.00", "433.92"};

/* ----- result screen -------------------------------------------------------- */

static void port_show(void* ctx, const TG1cProbe* p, const char* title, const char* detail, const char* hint) {
    App* app = ctx;
    with_view_model(
        app->result,
        ResultModel * m,
        {
            if(title) strlcpy(m->title, title, sizeof(m->title));
            if(detail) strlcpy(m->line[0], detail, sizeof(m->line[0]));
            if(p->have_tele)
                snprintf(
                    m->line[1],
                    sizeof(m->line[1]),
                    "T %08lX %02X/%02X x%lu",
                    (unsigned long)p->tele_id,
                    p->tele_b4,
                    p->tele_b7,
                    (unsigned long)p->tele_total);
            else
                strlcpy(m->line[1], "T none", sizeof(m->line[1]));
            /* The latest frame read out: "231kPa 2C bat ok ship?". Battery and
             * class are candidate readings of B4 (tg1c_seq.h). */
            if(p->have_tele)
                tg1c_decode_text(m->line[2], sizeof(m->line[2]), p->tele_b4, p->tele_b5, p->tele_b6);
            else
                m->line[2][0] = 0;
            if(p->rx.reply_valid)
                snprintf(
                    m->line[3],
                    sizeof(m->line[3]),
                    "BL CMD %02X  %08lX",
                    tg1c_btldr_reply_cmd(&p->rx),
                    (unsigned long)tg1c_btldr_reply_data4(&p->rx));
            else if(p->sel == TG1C_SEL_DILUTION) {
                /* Diluted bursts the spare answered, per ratio, of 3 trials
                 * each: "2:3 4:3 8:1 16:0 32:0 /3" (24 characters). */
                char woke[48];
                snprintf(
                    woke,
                    sizeof(woke),
                    "2:%u 4:%u 8:%u 16:%u 32:%u /%u",
                    tg1c_seq_dil_count(&p->seq, 0, TG1C_BIT_WOKE),
                    tg1c_seq_dil_count(&p->seq, 1, TG1C_BIT_WOKE),
                    tg1c_seq_dil_count(&p->seq, 2, TG1C_BIT_WOKE),
                    tg1c_seq_dil_count(&p->seq, 3, TG1C_BIT_WOKE),
                    tg1c_seq_dil_count(&p->seq, 4, TG1C_BIT_WOKE),
                    (unsigned)TG1C_DIL_TRIALS);
                strlcpy(m->line[3], woke, sizeof(m->line[3]));
            }
            else if(p->sel == TG1C_SEL_OFFSET) {
                /* One character per offset 0-15: W woke, . silent, ? unverified,
                 * - not run. "pos W..............." is 20 characters. */
                char map[TG1C_OFF_ITEMS + 1u];
                tg1c_seq_off_map(&p->seq, map);
                snprintf(m->line[3], sizeof(m->line[3]), "pos %s", map);
            }
            else if(p->sel == TG1C_SEL_SCAN)
                /* "woke: none" or "woke 38 3C": first index of each group that woke the spare. */
                tg1c_seq_scan_woke_text(&p->seq, m->line[3], 24);
            else if(p->sel == TG1C_SEL_PARK) {
                /* "E3 ack 77/01 was 87/01" (22 characters): frame E bursts sent,
                 * the target's pair in answer to them, and its pair before. */
                /* One declaration per line: a comma here would split the
                 * with_view_model() macro's argument. */
                char was[8] = "-";
                char ack[8] = "-";
                if(p->seq.pk_have_before)
                    snprintf(was, sizeof(was), "%02X/%02X", p->seq.pk_before_b4, p->seq.pk_before_b7);
                if(p->seq.pk_have_ack)
                    snprintf(ack, sizeof(ack), "%02X/%02X", p->seq.pk_ack_b4, p->seq.pk_ack_b7);
                snprintf(m->line[3], sizeof(m->line[3]), "E%u ack %s was %s", p->seq.pk_cmd_sent, ack, was);
            }
            else if(p->sel == TG1C_SEL_SWEEP)
                snprintf(
                    m->line[3],
                    sizeof(m->line[3]),
                    "woke %u silent %u unver %u",
                    tg1c_seq_sweep_count(&p->seq, TG1C_BIT_WOKE),
                    tg1c_seq_sweep_count(&p->seq, TG1C_BIT_SILENT),
                    tg1c_seq_sweep_count(&p->seq, TG1C_BIT_UNVERIFIED));
            else
                snprintf(
                    m->line[3],
                    sizeof(m->line[3]),
                    "BL sync %lu like %lu CRC -",
                    (unsigned long)p->rx.sync_hits,
                    (unsigned long)p->rx.sync_like_hits);
            if(p->fault)
                strlcpy(m->line[4], p->fault, sizeof(m->line[4]));
            else if(p->seq.status != TG1C_ST_RUNNING)
                tg1c_seq_status_text(&p->seq, m->line[4], sizeof(m->line[4]));
            else if(p->rx.reply_valid)
                strlcpy(m->line[4], "REPLY - TX stopped", sizeof(m->line[4]));
            else if(p->unknown_seen && p->sel == TG1C_SEL_PARK)
                snprintf(m->line[4], sizeof(m->line[4]), "NEW %02X/%02X - TX off", p->unk_b4, p->unk_b7);
            else if(p->unknown_seen)
                strlcpy(m->line[4], "UNKNOWN STATUS - TX off", sizeof(m->line[4]));
            else
                strlcpy(m->line[4], "running, Back cancels", sizeof(m->line[4]));
            if(hint) strlcpy(m->line[5], hint, sizeof(m->line[5]));
        },
        true);
}

static void result_draw(Canvas* canvas, void* model) {
    ResultModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 8, m->title);
    canvas_set_font(canvas, FontSecondary);
    /* Six rows 9 px apart under the title (v0.3.6; five rows 10 px apart before). */
    for(int i = 0; i < 6; i++) canvas_draw_str(canvas, 2, 18 + 9 * i, m->line[i]);
}

/* ----- hardware port -------------------------------------------------------- */

/* Session callbacks run inside tpms_session_pump, on the worker thread. */
static void on_frame(const TpmsFrame* frame, float rssi_dbm, void* ctx) {
    App* app = ctx;
    if(frame->raw_len < 9) return;
    tg1c_probe_on_frame(
        &app->probe, frame->id, frame->raw, (int)(rssi_dbm < 0 ? rssi_dbm - 0.5f : rssi_dbm + 0.5f));
}
static void on_archive(const TpmsCaptureRecord* record, void* ctx) {
    App* app = ctx;
    TG1cRawRecord raw = {record->tick, record->sequence, record->rx_lost, record->pulse};
    tg1c_probe_on_record(&app->probe, &raw);
}

static uint32_t port_tick_ms(void* ctx) {
    UNUSED(ctx);
    return furi_get_tick(); /* 1 kHz kernel tick */
}
static void port_delay_tick(void* ctx) {
    UNUSED(ctx);
    furi_delay_tick(1);
}
static size_t port_free_heap(void* ctx) {
    UNUSED(ctx);
    return memmgr_get_free_heap();
}
static bool port_cancelled(void* ctx) {
    return ((App*)ctx)->cancel;
}
static bool port_rx_start(void* ctx, uint32_t hz) {
    App* app = ctx;
    /* Same 325 kHz FSK preset and 512-interval stream as the bench session. */
    app->session = tpms_session_alloc_bench();
    tpms_session_set_frame_callback(app->session, on_frame, app);
    tpms_session_set_archive_callback(app->session, on_archive, app);
    if(!tpms_session_start(app->session, hz, TpmsModulationFsk)) return false;
    tpms_session_set_vdo_only(app->session, true);
    return true;
}
static bool port_rx_retune(void* ctx, uint32_t hz) {
    return tpms_session_retune(((App*)ctx)->session, hz, TpmsModulationFsk);
}
static void port_rx_pump(void* ctx, uint32_t timeout_ms) {
    tpms_session_pump(((App*)ctx)->session, timeout_ms);
}
static uint32_t port_rx_overruns(void* ctx) {
    return tpms_session_get_overruns(((App*)ctx)->session);
}
static int port_rx_rssi(void* ctx) {
    float dbm = tpms_session_get_rssi(((App*)ctx)->session);
    return (int)(dbm < 0 ? dbm - 0.5f : dbm + 0.5f);
}
static void port_rx_stop(void* ctx) {
    tpms_session_stop(((App*)ctx)->session);
}
static bool port_lf_prepare(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us) {
    UNUSED(ctx);
    return tg1c_lf_dma_prepare(slots, nslots, half_us, lead_us);
}
static const char* port_lf_failure(void* ctx) {
    UNUSED(ctx);
    return tg1c_lf_dma_failure();
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
static bool port_lf_reload(void* ctx, const uint8_t* slots, size_t nslots, uint16_t half_us, uint16_t lead_us) {
    UNUSED(ctx);
    return tg1c_lf_dma_reload(slots, nslots, half_us, lead_us);
}

/* ----- SD output (only after the receiver has stopped) ---------------------- */

static bool file_sink(void* ctx, const void* data, size_t len) {
    return storage_file_write(ctx, data, len) == len;
}

typedef enum { FileCsv, FileRaw, FileTele, FileReply } FileKind;

static bool save_file(App* app, Storage* storage, FileKind kind, uint32_t stack_min) {
    static const char* const name[] = {"session", "probe", "telemetry", "reply"};
    static const char* const ext[] = {"csv", "tpraw", "csv", "tpraw"};
    char path[96];
    snprintf(
        path,
        sizeof(path),
        DATA_DIR "/%s_%08lx.%s",
        name[kind],
        (unsigned long)app->probe.session_tick,
        ext[kind]);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok)
        ok = kind == FileRaw   ? tg1c_probe_write_raw(&app->probe, file_sink, f) :
             kind == FileReply ? tg1c_probe_write_reply_raw(&app->probe, file_sink, f) :
             kind == FileTele  ? tg1c_probe_write_tele(&app->probe, file_sink, f) :
                                 tg1c_probe_write_csv(&app->probe, stack_min, file_sink, f);
    if(ok && kind == FileCsv) {
        /* Stack mark again, now that the deepest SD call path has run. */
        char line[40];
        int n = snprintf(
            line,
            sizeof(line),
            "#stack_free_min_end,%lu\n",
            (unsigned long)furi_thread_get_stack_space(furi_thread_get_current_id()));
        ok = file_sink(f, line, (size_t)n);
    }
    if(ok) ok = storage_file_sync(f);
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

/* ----- worker --------------------------------------------------------------- */

static int32_t worker(void* ctx) {
    App* app = ctx;
    TG1cProbe* p = &app->probe;
    tg1c_probe_init(p, &app->port, app->sel, app->burst_idx, app->lead_idx, app->gap_idx, app->rxf);
    tg1c_probe_set_sweep_range(p, app->sweep_range);
    tg1c_probe_set_dilution_fill(p, app->dilute_fill);
    tg1c_probe_set_scan(p, app->scan_set, app->scan_group, app->scan_from);
    tg1c_probe_set_target(p, app->target);

    tg1c_probe_run(p); /* returns with the carrier off and the receiver stopped */
    if(app->session) {
        tpms_session_free(app->session);
        app->session = NULL;
    }
    port_show(app, p, "Saving", "receiver stopped", "");

    uint32_t stack_min = furi_thread_get_stack_space(furi_thread_get_current_id());
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, DATA_DIR);
    bool csv = save_file(app, storage, FileCsv, stack_min);
    bool want_raw = tg1c_probe_has_raw(p);
    bool raw = want_raw && save_file(app, storage, FileRaw, stack_min);
    bool want_tele = tg1c_probe_has_tele(p);
    bool tele = want_tele && save_file(app, storage, FileTele, stack_min);
    bool want_reply = tg1c_probe_has_reply_raw(p);
    bool reply = want_reply && save_file(app, storage, FileReply, stack_min);
    furi_record_close(RECORD_STORAGE);

    const char* hint = (!csv || (want_raw && !raw) || (want_tele && !tele) || (want_reply && !reply)) ?
                           "SD write failed" :
                       p->seq.status == TG1C_ST_BOOTLOADER_REPLY ? "Then run one A wake" :
                       raw ? "CSV + raw saved" :
                             "CSV saved";
    char detail[32];
    if(p->sel == TG1C_SEL_DILUTION)
        snprintf(
            detail,
            sizeof(detail),
            "bursts %u  trials %u/%u",
            p->seq.total_bursts,
            (unsigned)(TG1C_DIL_ITEMS - tg1c_seq_dil_count(&p->seq, -1, TG1C_BIT_NOT_RUN)),
            (unsigned)TG1C_DIL_ITEMS);
    else if(p->sel == TG1C_SEL_OFFSET || p->sel == TG1C_SEL_SCAN)
        snprintf(
            detail,
            sizeof(detail),
            "bursts %u  %s %u/%u",
            p->seq.total_bursts,
            p->sel == TG1C_SEL_SCAN ? "groups" : "trials",
            (unsigned)(p->seq.sw_items - tg1c_seq_item_count(&p->seq, TG1C_BIT_NOT_RUN)),
            (unsigned)p->seq.sw_items);
    else if(p->sel == TG1C_SEL_SWEEP)
        snprintf(
            detail,
            sizeof(detail),
            "bursts %u  bits %u/%u",
            p->seq.total_bursts,
            (unsigned)(TG1C_SWEEP_BITS - tg1c_seq_sweep_count(&p->seq, TG1C_BIT_NOT_RUN)),
            (unsigned)TG1C_SWEEP_BITS);
    else
        snprintf(detail, sizeof(detail), "bursts %u  CONNECT %u", p->seq.total_bursts, p->seq.connect_bursts);
    port_show(app, p, "Done", detail, hint);
    tg1c_probe_free(p); /* the evidence ring; the result fields stay readable */
    return 0;
}

static void worker_join(App* app) {
    if(app->worker) {
        app->cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
    app->cancel = false;
}

static void worker_start(App* app) {
    worker_join(app);
    app->worker = furi_thread_alloc_ex("Tg1cProbe", TG1C_WORKER_STACK, worker, app);
    furi_thread_start(app->worker);
}

/* ----- config view ---------------------------------------------------------- */

static void cfg_payload_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->sel = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kSelName[app->sel]);
}
static void cfg_burst_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->burst_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kBurstName[app->burst_idx]);
}
static void cfg_lead_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->lead_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kLeadName[app->lead_idx]);
}
static void cfg_gap_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->gap_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kGapName[app->gap_idx]);
}
static void cfg_rxf_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->rxf = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kRxfName[app->rxf]);
}
static void cfg_sweep_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->sweep_range = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kSweepName[app->sweep_range]);
}
static void cfg_fill_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->dilute_fill = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kFillName[app->dilute_fill]);
}
static void cfg_target_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->target = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kTargetName[app->target]);
}
static void cfg_scan_set_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->scan_set = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kScanSetName[app->scan_set]);
}
static void cfg_scan_group_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->scan_group = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kScanGroupName[app->scan_group]);
}
static void cfg_scan_from_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->scan_from = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, kScanFromName[app->scan_from]);
}
static void cfg_enter(void* ctx, uint32_t index) {
    App* app = ctx;
    if(index != app->cfg_start_row) return; /* only the Start row begins a session */
    view_dispatcher_switch_to_view(app->vd, ViewResult);
    worker_start(app);
}

static bool result_input(InputEvent* event, void* ctx) {
    App* app = ctx;
    /* Any Back event cancels, from the press itself: a held Back produces no
     * Short event, and the dispatcher leaves this view on a Long one, so
     * waiting for Short could leave a burst running behind the option list. */
    if(event->key == InputKeyBack)
        app->cancel = true; /* the engine stops TX and leaves its loop within ~10 ms */
    return false; /* the previous-view callback returns to the option list */
}
static uint32_t result_prev(void* ctx) {
    UNUSED(ctx);
    return ViewConfig;
}
static uint32_t config_prev(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

/* ----- app lifecycle -------------------------------------------------------- */

static void cfg_add(
    App* app,
    const char* label,
    uint8_t count,
    uint8_t index,
    const char* text,
    VariableItemChangeCallback changed) {
    VariableItem* it = variable_item_list_add(app->config, label, count, changed, app);
    variable_item_set_current_value_index(it, index);
    variable_item_set_current_value_text(it, text);
    app->cfg_rows++;
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->sel = TG1C_SEL_DEFAULT; /* stock wake: CONNECT is never the default */
    app->burst_idx = TG1C_BURST_DEFAULT;
    app->lead_idx = TG1C_LEAD_DEFAULT;
    app->gap_idx = TG1C_GAP_DEFAULT;
    app->rxf = TG1C_RXF_DEFAULT;
    app->dilute_fill = TG1C_DIL_FILL_DEFAULT;
    app->scan_set = TG1C_SCAN_BYTE;
    app->scan_group = TG1C_SCAN_GROUP_DEFAULT;
    app->port = (TG1cPort){
        .ctx = app,
        .tick_ms = port_tick_ms,
        .delay_tick = port_delay_tick,
        .free_heap = port_free_heap,
        .cancelled = port_cancelled,
        .rx_start = port_rx_start,
        .rx_retune = port_rx_retune,
        .rx_pump = port_rx_pump,
        .rx_overruns = port_rx_overruns,
        .rx_rssi = port_rx_rssi,
        .rx_stop = port_rx_stop,
        .lf_prepare = port_lf_prepare,
        .lf_failure = port_lf_failure,
        .lf_start = port_lf_start,
        .lf_done = port_lf_done,
        .lf_error = port_lf_error,
        .lf_stop = port_lf_stop,
        .lf_free = port_lf_free,
        .show = port_show,
        .lf_reload = port_lf_reload,
    };

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->config = variable_item_list_alloc();
    cfg_add(app, "Payload", TG1C_SEL_COUNT, app->sel, kSelName[app->sel], cfg_payload_changed);
    cfg_add(app, "Target", TG1C_TARGETS, app->target, kTargetName[app->target], cfg_target_changed);
    cfg_add(app, "Burst", TG1C_BURST_OPTIONS, app->burst_idx, kBurstName[app->burst_idx], cfg_burst_changed);
    cfg_add(app, "Lead-in", TG1C_LEAD_OPTIONS, app->lead_idx, kLeadName[app->lead_idx], cfg_lead_changed);
    cfg_add(app, "Gap", TG1C_GAP_OPTIONS, app->gap_idx, kGapName[app->gap_idx], cfg_gap_changed);
    cfg_add(app, "CONN RX", TG1C_RXF_COUNT, app->rxf, kRxfName[app->rxf], cfg_rxf_changed);
    cfg_add(app, "Sweep bits", TG1C_SWEEP_RANGES, app->sweep_range, kSweepName[app->sweep_range], cfg_sweep_changed);
    cfg_add(app, "Dilute fill", TG1C_DIL_FILLS, app->dilute_fill, kFillName[app->dilute_fill], cfg_fill_changed);
    cfg_add(app, "Scan set", TG1C_SCAN_SETS, app->scan_set, kScanSetName[app->scan_set], cfg_scan_set_changed);
    cfg_add(app, "Scan group", TG1C_SCAN_GROUP_OPTIONS, app->scan_group, kScanGroupName[app->scan_group], cfg_scan_group_changed);
    cfg_add(app, "Scan from", TG1C_SCAN_FROM_OPTIONS, app->scan_from, kScanFromName[app->scan_from], cfg_scan_from_changed);
    app->cfg_start_row = app->cfg_rows; /* Start is the row after every option row */
    variable_item_list_add(app->config, "Start", 0, NULL, app);
    variable_item_list_set_enter_callback(app->config, cfg_enter, app);
    View* cfg_view = variable_item_list_get_view(app->config);
    view_set_previous_callback(cfg_view, config_prev);
    view_dispatcher_add_view(app->vd, ViewConfig, cfg_view);

    app->result = view_alloc();
    view_allocate_model(app->result, ViewModelTypeLocking, sizeof(ResultModel));
    view_set_context(app->result, app);
    view_set_draw_callback(app->result, result_draw);
    view_set_input_callback(app->result, result_input);
    view_set_previous_callback(app->result, result_prev);
    view_dispatcher_add_view(app->vd, ViewResult, app->result);

    return app;
}

static void app_free(App* app) {
    worker_join(app);
    view_dispatcher_remove_view(app->vd, ViewConfig);
    view_dispatcher_remove_view(app->vd, ViewResult);
    variable_item_list_free(app->config);
    view_free(app->result);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t tg1c_lf_lab_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->vd, ViewConfig);
    view_dispatcher_run(app->vd);
    app_free(app);
    return 0;
}
