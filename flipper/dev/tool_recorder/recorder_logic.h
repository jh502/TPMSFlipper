// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { Intro, Prepare, Baseline, WaitOn, Active, Tail, Result, Done } Page;
typedef enum { NoAction, PromptOn, FinishIdle, FinishTail, TimeLimit } TimedAction;
static inline TimedAction recorder_due(Page page, unsigned step, uint32_t elapsed_ms, uint32_t phase_ms) {
    if(elapsed_ms>=120000) return TimeLimit;
    if(page==Baseline && phase_ms>=(step?5000U:30000U)) return step?PromptOn:FinishIdle;
    if(page==Tail && phase_ms>=5000) return FinishTail;
    return NoAction;
}
static inline bool recorder_integrity(unsigned step, bool stopped, bool disk_ok,
    uint32_t lost, uint32_t invalid, uint32_t count, bool on, bool off) {
    return !stopped && disk_ok && !lost && !invalid && (!step || (count && on && off));
}
