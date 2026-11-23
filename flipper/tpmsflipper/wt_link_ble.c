// SPDX-License-Identifier: GPL-2.0-or-later
/* TPMSFlipper — the phone link's Bluetooth LE profile.
 *
 * A Flipper app can bring its own BLE profile: a GapConfig (how it
 * advertises, pairing) and the GATT services it adds when started
 * (furi_ble/profile_interface.h). This follows the firmware's own serial and
 * HID profiles (lib/ble_profile, targets/f7/ble_glue/profiles) at 1.5.1-rc:
 *   - one primary service, one characteristic: read + notify, 20 bytes;
 *   - no pairing and no bonding, so any phone can connect and nothing is
 *     written to the Flipper's key store;
 *   - its own MAC address (the Flipper's, one byte changed, as the HID profile
 *     does), so a phone that has paired with the Flipper before does not mix
 *     this service up with the Flipper's own;
 *   - the advertising packet carries the 128-bit service UUID, which is what
 *     an iPhone app filters on, also in the background, and a short name.
 * Advertising data is 31 bytes at most. The firmware passes flags 3 + UUID 18
 * + name (2 + length), but the stack adds more of its own: with the 7-letter
 * name "TPMS WT" (30 bytes by that count) aci_gap_set_discoverable failed
 * with 0x92, invalid parameters, on the owner's Flipper (v0.3.0, 13 November
 * 2026; analysis/tpmsflipper-phone-link-20261113/). Hence 4 letters, which
 * leaves room for a 3-byte TX power entry. Note that BleGap reports the state
 * as advertising even when that call fails.
 */
#include "wt_link_ble.h"

#if WT_PHONE_LINK

#include <furi.h>
#include <furi_hal_version.h>
#include <storage/storage.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <furi_ble/profile_interface.h>
#include <ble/core/ble_defs.h>

#define TAG "TPMSFlipperLink"
#define WT_LINK_ADV_NAME "TPMS" /* 4 characters at most, see above */
#define WT_LINK_ATTRS 8u /* service; readings: declaration, value, CCCD; commands: declaration, value; room */
#define WT_LINK_KEYS EXT_PATH("apps_data/tpmsflipper/.bt.keys") /* our pairings, not the Flipper's */
#define WT_LINK_FAIL_MS 3000u /* not advertising this long after starting: Bluetooth is off */
#define WT_LINK_SUB_WAIT_MS 1500u /* connected, no subscription seen: send anyway after this */

/* Who is connected. The bt service's status cannot say: at 1.5.1-rc gap.c
 * raises GapEventTypeConnected only when pairing completes (gap.c,
 * ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE), and this profile has no pairing, so
 * the status stays "advertising" with a phone connected (seen on the owner's
 * Flipper with v0.3.1). So the profile reads the BLE events itself, through
 * the event dispatcher, as the firmware's serial service does.
 *
 * The stack's event packet, as the pinned stm32wb_copro (133182d) defines it
 * in tl.h (hci_uart_pckt, hci_event_pckt, evt_blecore_aci), headers the SDK
 * does not ship: byte 0 packet type, 1 event code, 2 length, 3.. the event.
 *   0x3E LE meta: 3 subevent (0x01 connection complete, 0x0A enhanced), 4 status
 *   0x05 disconnection complete
 *   0xFF vendor: 3..4 ecode; 0x0C01 attribute modified (ble_vs_codes.h), then
 *        the SDK's aci_gatt_attribute_modified_event_rp0 from byte 5 */
#define WT_EVT_LE_META 0x3Eu
#define WT_EVT_LE_CONN 0x01u
#define WT_EVT_LE_CONN_ENH 0x0Au
#define WT_EVT_DISCONN 0x05u
#define WT_EVT_VENDOR 0xFFu
#define WT_ECODE_ATTR_MODIFIED 0x0C01u

typedef struct {
    FuriHalBleProfileBase base;
    uint16_t svc;
    BleGattCharacteristicInstance chr;
    BleGattCharacteristicInstance cmd; /* commands from a paired phone */
    WtLinkCommand on_command;
    void* command_ctx;
    GapSvcEventHandler* events;
    volatile bool connected, subscribed;
    volatile uint32_t connected_at;
} WtLinkProfile;
_Static_assert(offsetof(WtLinkProfile, base) == 0, "profile must start with its base");

struct WtLinkBle {
    Bt* bt;
    WtLinkProfile* profile;
    uint32_t started;
};

static const uint8_t zero_rec[WT_LINK_REC_LEN] = {0};
static const FuriHalBleProfileTemplate* const wt_link_profile;

static const BleGattCharacteristicParams chr_params = {
    .name = "TPMS readings",
    .data_prop_type = FlipperGattCharacteristicDataFixed,
    .data.fixed.length = WT_LINK_REC_LEN,
    .data.fixed.ptr = zero_rec,
    .uuid.Char_UUID_128 = WT_LINK_CHR_UUID,
    .uuid_type = UUID_TYPE_128,
    .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
    .is_variable = CHAR_VALUE_LEN_CONSTANT,
};

/* Commands: written by the phone, and only over an encrypted, authenticated
 * (PIN-paired) link. The stack refuses a write from an unpaired phone. */
static const BleGattCharacteristicParams cmd_params = {
    .name = "TPMS commands",
    .data_prop_type = FlipperGattCharacteristicDataFixed,
    .data.fixed.length = WT_LINK_REC_LEN,
    .data.fixed.ptr = zero_rec,
    .uuid.Char_UUID_128 = WT_LINK_CMD_UUID,
    .uuid_type = UUID_TYPE_128,
    .char_properties = CHAR_PROP_WRITE,
    .security_permissions = ATTR_PERMISSION_ENCRY_WRITE | ATTR_PERMISSION_AUTHEN_WRITE,
    .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
    .is_variable = CHAR_VALUE_LEN_CONSTANT,
};

typedef struct {
    WtLinkCommand on_command;
    void* ctx;
} WtLinkParams;

/* Called on the BLE event thread for every event. Only the subscription
 * write to our own descriptor is ours to acknowledge; GAP must still see the
 * connection events. */
static BleEventAckStatus on_ble_event(void* event, void* ctx) {
    WtLinkProfile* p = ctx;
    const uint8_t* e = event;
    if(e[1] == WT_EVT_LE_META && (e[3] == WT_EVT_LE_CONN || e[3] == WT_EVT_LE_CONN_ENH) && e[4] == 0) {
        p->subscribed = false;
        p->connected_at = furi_get_tick();
        p->connected = true;
    } else if(e[1] == WT_EVT_DISCONN) {
        p->connected = p->subscribed = false;
    } else if(e[1] == WT_EVT_VENDOR && (uint16_t)(e[3] | e[4] << 8) == WT_ECODE_ATTR_MODIFIED) {
        const aci_gatt_attribute_modified_event_rp0* m = (const void*)(e + 5);
        if(m->Attr_Handle == p->chr.handle + 2) { /* the CCCD follows the value handle */
            p->subscribed = m->Attr_Data_Length >= 1 && (m->Attr_Data[0] & 0x01u);
            return BleEventAckFlowEnable;
        }
        if(m->Attr_Handle == p->cmd.handle + 1) { /* a command's value */
            if(m->Attr_Data_Length == WT_LINK_REC_LEN && p->on_command) p->on_command(m->Attr_Data, p->command_ctx);
            return BleEventAckFlowEnable;
        }
    }
    return BleEventNotAck;
}

static FuriHalBleProfileBase* profile_start(FuriHalBleProfileParams params) {
    const WtLinkParams* lp = params;
    WtLinkProfile* p = malloc(sizeof(WtLinkProfile));
    memset(p, 0, sizeof(*p));
    p->base.config = wt_link_profile;
    if(lp) p->on_command = lp->on_command, p->command_ctx = lp->ctx;
    p->events = ble_event_dispatcher_register_svc_handler(on_ble_event, p);
    const Service_UUID_t uuid = {.Service_UUID_128 = WT_LINK_SVC_UUID};
    if(!ble_gatt_service_add(UUID_TYPE_128, &uuid, PRIMARY_SERVICE, WT_LINK_ATTRS, &p->svc)) {
        FURI_LOG_E(TAG, "service not added");
        p->svc = 0;
        return &p->base;
    }
    ble_gatt_characteristic_init(p->svc, &chr_params, &p->chr);
    ble_gatt_characteristic_init(p->svc, &cmd_params, &p->cmd);
    return &p->base;
}

static void profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == wt_link_profile);
    WtLinkProfile* p = (WtLinkProfile*)base;
    if(p->events) ble_event_dispatcher_unregister_svc_handler(p->events);
    if(p->svc) {
        ble_gatt_characteristic_delete(p->svc, &p->cmd);
        ble_gatt_characteristic_delete(p->svc, &p->chr);
        ble_gatt_service_delete(p->svc);
    }
    /* furi_hal_bt drops its pointer after this and restarts the stack; the
     * instance is ours to free (the firmware's own profiles leak theirs). */
    free(p);
}

static void profile_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    static const uint8_t svc_uuid[16] = WT_LINK_SVC_UUID;
    furi_check(config);
    memset(config, 0, sizeof(*config));
    config->adv_service.UUID_Type = UUID_TYPE_128;
    memcpy(config->adv_service.Service_UUID_128, svc_uuid, sizeof(svc_uuid));
    config->appearance_char = 0x0000; /* unknown */
    /* Pair once with a PIN the Flipper shows, and keep the bond. GAP asks for
     * it as soon as a phone connects (gap.c sends a security request) and
     * drops a phone that fails. Before 0.4.0 there was no pairing; that was
     * fine for readings only, not for commands. */
    config->bonding_mode = true;
    config->pairing_method = GapPairingPinCodeShow;
    config->conn_param.conn_int_min = 0x18; /* 30 ms: four tyres a minute need no speed */
    config->conn_param.conn_int_max = 0x30; /* 60 ms */
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 3; /* the HID profile uses +1 */
    config->adv_name[0] = 0x09; /* AD type: complete local name; gap.c skips it for the GAP name */
    strlcpy(config->adv_name + 1, WT_LINK_ADV_NAME, sizeof(config->adv_name) - 1);
}

static const FuriHalBleProfileTemplate profile_template = {
    .start = profile_start,
    .stop = profile_stop,
    .get_gap_config = profile_gap_config,
};
static const FuriHalBleProfileTemplate* const wt_link_profile = &profile_template;

WtLinkBle* wt_link_ble_start(WtLinkCommand on_command, void* ctx) {
    static WtLinkParams params; /* read by profile_start during the call below */
    WtLinkBle* link = malloc(sizeof(WtLinkBle));
    memset(link, 0, sizeof(*link));
    link->bt = furi_record_open(RECORD_BT);
    bt_disconnect(link->bt); /* the Flipper phone app, if it was connected */
    furi_delay_ms(200);
    /* Our bonds in our own file, as the HID app keeps its own: pairing a phone
     * here leaves the Flipper's own pairings alone. */
    bt_keys_storage_set_storage_path(link->bt, WT_LINK_KEYS);
    params = (WtLinkParams){on_command, ctx};
    link->profile = (WtLinkProfile*)bt_profile_start(link->bt, wt_link_profile, &params);
    if(!link->profile || !link->profile->svc) {
        FURI_LOG_E(TAG, "profile did not start");
        wt_link_ble_stop(link);
        return NULL;
    }
    link->started = furi_get_tick();
    return link;
}

void wt_link_ble_stop(WtLinkBle* link) {
    if(!link) return;
    if(link->profile) bt_disconnect(link->bt);
    bt_keys_storage_set_default_path(link->bt);
    /* Always: bt_profile_start may have switched before it failed. */
    if(!bt_profile_restore_default(link->bt)) FURI_LOG_E(TAG, "default profile not restored");
    furi_record_close(RECORD_BT);
    free(link);
}

WtLinkState wt_link_ble_state(WtLinkBle* link) {
    if(!link) return WT_LINK_FAIL;
    WtLinkProfile* p = link->profile;
    /* A phone that has subscribed; or one connected a while without the
     * subscription being seen, in case a stack reports it differently. */
    if(p->connected && (p->subscribed || furi_get_tick() - p->connected_at >= WT_LINK_SUB_WAIT_MS))
        return WT_LINK_ON;
    if(furi_hal_bt_is_active()) return WT_LINK_WAIT; /* advertising, or connected and not yet subscribed */
    return furi_get_tick() - link->started < WT_LINK_FAIL_MS ? WT_LINK_WAIT : WT_LINK_FAIL;
}

bool wt_link_ble_send(WtLinkBle* link, const uint8_t rec[WT_LINK_REC_LEN]) {
    if(!link || !link->profile || !link->profile->svc || !link->profile->connected) return false;
    /* At 1.5.1-rc this returns true on FAILURE (furi_ble/gatt.c ends with
     * "return result != BLE_STATUS_SUCCESS"), after retrying a full stack for
     * up to a second. Callers keep to a few records per call. */
    return !ble_gatt_characteristic_update(link->profile->svc, &link->profile->chr, rec);
}

#else /* !WT_PHONE_LINK: the public build has no phone link and no Bluetooth code */

WtLinkBle* wt_link_ble_start(WtLinkCommand on_command, void* ctx) {
    (void)on_command;
    (void)ctx;
    return NULL;
}
void wt_link_ble_stop(WtLinkBle* link) {
    (void)link;
}
WtLinkState wt_link_ble_state(WtLinkBle* link) {
    (void)link;
    return WT_LINK_OFF;
}
bool wt_link_ble_send(WtLinkBle* link, const uint8_t rec[WT_LINK_REC_LEN]) {
    (void)link;
    (void)rec;
    return false;
}

#endif
