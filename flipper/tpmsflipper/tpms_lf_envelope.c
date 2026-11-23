// SPDX-License-Identifier: GPL-2.0-or-later
#include "tpms_lf_envelope.h"
size_t tpms_lf_envelope(const uint8_t* slots, size_t length, uint16_t half_us,
    uint16_t duty, uint16_t* first, uint16_t* output, size_t capacity) {
    if(!slots || !first || !output || !length || half_us<112 || half_us>144 || half_us%8) return 0;
    size_t cycles=half_us/8;
    if(length>capacity/cycles) return 0;
    for(size_t i=0;i<length;i++) if(slots[i]>1) return 0;
    size_t count=length*cycles;
    *first=slots[0]?duty:0;
    for(size_t i=1;i<count;i++) output[i-1]=slots[i/cycles]?duty:0;
    output[count-1]=0;
    return count;
}

size_t tpms_lf_envelope_lead(const uint8_t* slots, size_t length, uint16_t half_us,
    uint16_t lead_us, uint16_t duty, uint16_t* first, uint16_t* output, size_t capacity) {
    if(!first || !output || lead_us>16000 || lead_us%8) return 0;
    size_t prefix=lead_us/8;
    if(prefix>capacity) return 0;
    uint16_t frame_first;
    size_t count=tpms_lf_envelope(slots,length,half_us,duty,&frame_first,output+prefix,capacity-prefix);
    if(!count) return 0;
    if(!prefix) {*first=frame_first;return count;}
    *first=duty;
    for(size_t i=0;i+1<prefix;i++) output[i]=duty;
    output[prefix-1]=frame_first;
    return prefix+count;
}
