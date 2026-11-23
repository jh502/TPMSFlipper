// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* TPMSFlipper: what a build may change.
 *
 *   WT_PHONE_LINK 1  Settings > Developer has "Phone link". Off until it is
 *                    switched on there; then the app runs a Bluetooth LE
 *                    service for a phone app (wt_link.h, wt_remote.h).
 *   WT_PHONE_LINK 0  no such row, the settings file's "phone=" line is
 *                    ignored and not written, and wt_link_ble.c compiles to
 *                    stubs, so the app contains no Bluetooth code at all
 *                    (then also take "bt" out of application.fam).
 *   WT_APP_NAME      the name on the home screen and in the settings file.
 *   WT_APP_ID        the app's id: its folder under apps_data on the SD card.
 */
#ifndef WT_PHONE_LINK
#define WT_PHONE_LINK 1
#endif
#ifndef WT_VERSION
#define WT_VERSION "1.0.0"
#endif
#ifndef WT_APP_NAME
#define WT_APP_NAME "TPMSFlipper"
#endif
#ifndef WT_APP_ID
#define WT_APP_ID "tpmsflipper"
#endif
