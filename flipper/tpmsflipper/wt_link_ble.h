// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper — the phone link's Bluetooth side (Flipper only). While it
 * runs, the Flipper's own Bluetooth profile (the one the Flipper phone app
 * uses) is replaced by TPMSFlipper's service; stopping puts it back. What
 * is sent is wt_link.h. */
#include "wt_link.h"

typedef struct WtLinkBle WtLinkBle;
/* A command from a paired phone: 20 bytes, on the BLE event thread. Copy it
 * and return; act on it elsewhere. */
typedef void (*WtLinkCommand)(const uint8_t cmd[WT_LINK_REC_LEN], void* ctx);

/* Switch the Flipper's Bluetooth to TPMSFlipper service and advertise it.
 * A phone pairs once, with a PIN the Flipper shows; then it may send
 * commands, delivered to on_command. Restarts the radio co-processor, which
 * takes about a second. NULL if the service could not be started. */
WtLinkBle* wt_link_ble_start(WtLinkCommand on_command, void* ctx);
/* Put the Flipper's own Bluetooth profile back, and free. NULL is allowed. */
void wt_link_ble_stop(WtLinkBle* link);
/* WT_LINK_WAIT, WT_LINK_ON, or WT_LINK_FAIL when Bluetooth is not running
 * (switched off in the Flipper's settings). */
WtLinkState wt_link_ble_state(WtLinkBle* link);
/* One record to the phone: the characteristic's new value, notified to a
 * phone that has subscribed. False if the stack refused it (its buffers are
 * full); send it again later. */
bool wt_link_ble_send(WtLinkBle* link, const uint8_t rec[WT_LINK_REC_LEN]);
