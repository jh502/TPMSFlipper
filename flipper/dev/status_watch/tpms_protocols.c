// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// From flipper-zero-tpms, Copyright (c) 2026 Vishov Vladimir, MIT licence
// (licenses/MIT-flipper-zero-tpms.txt), with this project's changes.
#include "tpms_proto.h"
const TpmsProtocol tpms_protocols[] = {{.id="vdo_tg1c",.label="VDO TG1C",.chip_us=52,.modulation=TpmsModulationFsk,.slicer=TpmsSlicerNrz,.id_digits=8,.sync=0xAAAAA9,.sync_bits=24,.capture_bits=144,.decode=tpms_decode_abarth124}};
const uint8_t tpms_protocol_count=1;
const char* tpms_protocol_label(uint8_t i){return i?"?":"VDO TG1C";}
const char* tpms_protocol_id(uint8_t i){return i?"?":"vdo_tg1c";}
uint8_t tpms_protocol_count_for(TpmsModulation m){return m==TpmsModulationFsk;}
