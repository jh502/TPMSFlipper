// SPDX-License-Identifier: GPL-2.0-or-later
// Passive receiver derived from TPMS Lab's tpms_session.c.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stm32wbxx_ll_tim.h>
#include <stdio.h>
#include <string.h>
#include "recorder_logic.h"
#define ROOT EXT_PATH("apps_data/tpms_tool_recorder")
typedef struct { uint32_t tick, sequence, lost; int32_t pulse; } Record;
_Static_assert(sizeof(Record)==16,"TPRAW02 layout");
typedef struct {
    FuriStreamBuffer* stream; FuriMessageQueue* keys; FuriMutex* mutex;
    Storage* storage; File *raw, *log;
    Page page; unsigned step, attempt, run, spacing, original_spacing;
    uint32_t start, phase, written, on_tick, off_tick;
    volatile uint32_t sequence, lost, high, fast, invalid;
    volatile bool have_high, back, exit_requested; volatile unsigned fault;
    bool receiving, disk_ok, passed, on, off, raw_open, blocked;
    char directory[100], reason[32]; Record batch[64];
} App;
static void disable_rx(App* a,unsigned fault) {
    a->fault=fault; LL_TIM_DisableIT_CC3(TIM2); LL_TIM_DisableIT_CC4(TIM2);
}
static void emit(App* a,int32_t pulse) {
    Record r={furi_get_tick(),++a->sequence,a->lost,pulse};
    if(furi_stream_buffer_spaces_available(a->stream)<sizeof(r) ||
       furi_stream_buffer_send(a->stream,&r,sizeof(r),0)!=sizeof(r)) a->lost++;
}
static void receive(bool high,uint32_t duration,void* ctx) {
    App* a=ctx; if(a->fault) return;
    if(LL_TIM_IsActiveFlag_CC3OVR(TIM2)||LL_TIM_IsActiveFlag_CC4OVR(TIM2)) {
        LL_TIM_ClearFlag_CC3OVR(TIM2); LL_TIM_ClearFlag_CC4OVR(TIM2); disable_rx(a,1); return;
    }
    if(high) {a->high=duration;a->have_high=true;return;}
    a->fast=duration<32?a->fast+1:0;
    if(a->fast>=256) {disable_rx(a,2);return;}
    if(a->have_high && a->high && duration>a->high && duration<=INT32_MAX) {
        emit(a,(int32_t)a->high); emit(a,-(int32_t)(duration-a->high));
    } else if(a->have_high) a->invalid++;
    a->have_high=false;
}
static void input(InputEvent* e,void* ctx) {
    App* a=ctx; if(e->type!=InputTypeShort) return;
    if(e->key==InputKeyBack) {a->back=true;return;}
    furi_message_queue_put(a->keys,&e->key,0);
}
static bool signal_exit(uint32_t signal, void* arg, void* ctx) {
    UNUSED(arg);
    if(signal!=FuriSignalExit) return false;
    ((App*)ctx)->exit_requested=true;return true;
}
static void line(Canvas* c,unsigned y,const char* s) {canvas_draw_str(c,2,y,s);}
static void draw(Canvas* c,void* ctx) {
    App* a=ctx; furi_mutex_acquire(a->mutex,FuriWaitForever);
    canvas_clear(c);canvas_set_font(c,FontSecondary);
    char b[48]; snprintf(b,sizeof(b),"Tool Recorder   %u/4",a->step<4?a->step+1:4);if(a->receiving) snprintf(b,sizeof(b),"Step %u/4   %lu pulses",a->step+1,(unsigned long)a->written);
    line(c,9,b);
    switch(a->page) {
    case Intro:
        line(c,22,"Tool capture guide");line(c,33,"1 idle + 3 activations");
        line(c,44,"Receive only. RF off.");line(c,60,"OK: begin   Back: exit");break;
    case Prepare:
        line(c,22,"Tool OFF. Keep tools flat.");
        if(a->step==0 || a->step==3) {
            line(c,33,a->step==0?"Set gap between tools:":"Change gap; same direction.");
            snprintf(b,sizeof(b),"Left/Right: %u cm gap",a->spacing);line(c,44,b);
        } else {
            snprintf(b,sizeof(b),"Keep same %u cm gap.",a->spacing);line(c,33,b);
            line(c,44,"Keep the same direction.");
        }
        line(c,60,a->step==3 && a->spacing==a->original_spacing?"Choose a different gap.":"OK: listen (keep tool OFF)");break;
    case Baseline: {
        line(c,23,"Listening. Tool OFF.");
        uint32_t elapsed=furi_get_tick()-a->phase, limit=furi_ms_to_ticks(a->step?5000:30000);
        snprintf(b,sizeof(b),"%lus left",(unsigned long)(elapsed<limit?(limit-elapsed)/furi_ms_to_ticks(1000):0));line(c,37,b);
        line(c,60,"Back: save partial");break;}
    case WaitOn:
        line(c,23,"Press OK here, then");line(c,35,"activate tool once.");line(c,47,"Do not move either tool.");line(c,60,"OK: mark tool ON");break;
    case Active:
        line(c,22,"Let tool finish its cycle.");line(c,34,"When tool has stopped:");line(c,46,"press OK to mark OFF.");line(c,60,"OK: OFF   Back: partial");break;
    case Tail:
        line(c,24,"Tool OFF. Keep still.");line(c,38,"Recording 5 seconds after.");line(c,60,"Will save automatically.");break;
    case Result:
        line(c,22,a->passed?"Saved for analysis":a->reason);
        snprintf(b,sizeof(b),"%lu intervals, %lu lost",(unsigned long)a->written,(unsigned long)a->lost);line(c,34,b);
        snprintf(b,sizeof(b),"Save run_%04u for analysis",a->run);line(c,46,b);
        line(c,60,a->blocked?"Back: exit; send this run":a->passed?"OK: next   Left: retry":"OK: retry   Back: exit");break;
    case Done:
        line(c,22,"Four recordings saved.");line(c,34,"Exit app; use qFlipper.");snprintf(b,sizeof(b),"Copy run_%04u folder.",a->run);line(c,46,b);line(c,60,"Back: exit");break;
    }
    furi_mutex_release(a->mutex);
}
static bool write_text(File* f,const char* text) {return storage_file_write(f,text,strlen(text))==strlen(text);}
static void drain(App* a) {
    size_t n=furi_stream_buffer_receive(a->stream,a->batch,sizeof(a->batch),0);
    if(n && a->disk_ok) {
        if(storage_file_write(a->raw,a->batch,n)!=n) a->disk_ok=false;
        else a->written+=n/sizeof(Record);
    }
}
static void finish(App* a,const char* reason) {
    if(a->receiving) {furi_hal_rfid_tim_read_capture_stop();furi_hal_rfid_pins_reset();furi_hal_power_insomnia_exit();a->receiving=false;}
    while(furi_stream_buffer_bytes_available(a->stream)) drain(a);
    if(!reason && a->fault) reason=a->fault==1?"Timer overcapture":"LF carrier too dense";
    a->passed=recorder_integrity(a->step,reason!=NULL,a->disk_ok,a->lost,a->invalid,a->written,a->on,a->off);
    snprintf(a->reason,sizeof(a->reason),"%s",reason?reason:!a->disk_ok?"SD write failed":a->lost?"Capture lost intervals":a->invalid?"Invalid pulse pairs":!a->passed?"No LF data captured":"Saved");
    if(a->raw_open) {
        if(!storage_file_sync(a->raw)) {a->disk_ok=false;a->passed=false;snprintf(a->reason,sizeof(a->reason),"SD sync failed");}
        storage_file_close(a->raw);a->raw_open=false;
    }
    char b[600];
    snprintf(b,sizeof(b),"{\"event\":\"capture_end\",\"step\":%u,\"attempt\":%u,\"tick\":%lu,\"records\":%lu,\"sequence\":%lu,\"lost\":%lu,\"invalid_pairs\":%lu,\"on_tick\":%lu,\"off_tick\":%lu,\"on_marked\":%s,\"off_marked\":%s,\"integrity_pass\":%s,\"reason\":\"%s\"}\n",a->step,a->attempt,(unsigned long)furi_get_tick(),(unsigned long)a->written,(unsigned long)a->sequence,(unsigned long)a->lost,(unsigned long)a->invalid,(unsigned long)a->on_tick,(unsigned long)a->off_tick,a->on?"true":"false",a->off?"true":"false",a->passed?"true":"false",a->reason);
    if(!write_text(a->log,b)||!storage_file_sync(a->log)) {a->disk_ok=false;a->passed=false;snprintf(a->reason,sizeof(a->reason),"Metadata save failed");}
    a->blocked=a->fault || a->lost || a->invalid || !a->disk_ok;
    a->page=Result;
}
static void start(App* a) {
    if(a->step==0) a->original_spacing=a->spacing;
    a->blocked=false;a->attempt++; a->written=a->sequence=a->lost=a->invalid=a->fast=a->fault=0;
    a->have_high=a->on=a->off=false;a->on_tick=a->off_tick=0;
    furi_stream_buffer_reset(a->stream);
    char path[150],b[400];snprintf(path,sizeof(path),"%s/step_%u_attempt_%u.tpraw",a->directory,a->step,a->attempt);
    a->raw_open=storage_file_open(a->raw,path,FSAM_WRITE,FSOM_CREATE_NEW);a->disk_ok=a->raw_open;
    if(a->disk_ok) a->disk_ok=storage_file_write(a->raw,"TPRAW02\n",8)==8;
    a->start=a->phase=furi_get_tick();
    snprintf(b,sizeof(b),"{\"event\":\"capture_start\",\"step\":%u,\"attempt\":%u,\"tick\":%lu,\"file\":\"step_%u_attempt_%u.tpraw\",\"source\":\"passive_lf\",\"placement\":\"%s\",\"gap_cm\":%u}\n",a->step,a->attempt,(unsigned long)a->start,a->step,a->attempt,a->step==3?"changed_separation":"fixed",a->spacing);
    if(!write_text(a->log,b)||!storage_file_sync(a->log)) a->disk_ok=false;
    if(!a->disk_ok) {finish(a,"Cannot open/save files");return;}
    furi_hal_rfid_pins_reset();furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull,false);
    LL_TIM_ClearFlag_CC3OVR(TIM2);LL_TIM_ClearFlag_CC4OVR(TIM2);
    a->start=a->phase=furi_get_tick();
    furi_hal_power_insomnia_enter();a->receiving=true;furi_hal_rfid_tim_read_capture_start(receive,a);a->page=Baseline;
}
int32_t tpms_tool_recorder_app(void* p) {
    UNUSED(p);App* a=malloc(sizeof(App));memset(a,0,sizeof(App));a->spacing=5;
    a->stream=furi_stream_buffer_alloc(16384,sizeof(Record));
    a->keys=furi_message_queue_alloc(8,sizeof(InputKey));a->mutex=furi_mutex_alloc(FuriMutexTypeNormal);
    a->storage=furi_record_open(RECORD_STORAGE);a->raw=storage_file_alloc(a->storage);a->log=storage_file_alloc(a->storage);
    storage_common_mkdir(a->storage,ROOT);
    bool ready=false;
    for(unsigned i=1;i<10000;i++) {
        snprintf(a->directory,sizeof(a->directory),ROOT "/run_%04u",i);
        if(storage_common_stat(a->storage,a->directory,NULL)==FSE_NOT_EXIST) {
            ready=storage_common_mkdir(a->storage,a->directory)==FSE_OK;a->run=i;break;
        }
    }
    char path[140];snprintf(path,sizeof(path),"%s/events.jsonl",a->directory);
    ready=ready && storage_file_open(a->log,path,FSAM_WRITE,FSOM_CREATE_NEW);
    if(ready) ready=write_text(a->log,"{\"event\":\"session\",\"app\":\"TPMS Tool Recorder\",\"version\":\"0.1.0\",\"tool\":\"trigger tool\",\"raw_format\":\"TPRAW02\",\"rf_active\":false,\"lf_transmit\":false,\"tick_hz\":1000}\n");
    if(!ready) {a->blocked=true;a->page=Result;snprintf(a->reason,sizeof(a->reason),"SD unavailable. Back: exit");}
    Gui* gui=furi_record_open(RECORD_GUI);ViewPort* view=view_port_alloc();
    view_port_draw_callback_set(view,draw,a);view_port_input_callback_set(view,input,a);gui_add_view_port(gui,view,GuiLayerFullscreen);
    furi_thread_set_signal_callback(furi_thread_get_current(),signal_exit,a);
    bool quit=false;uint32_t last_draw=0;
    while(!quit) {
        InputKey key;bool got=furi_message_queue_get(a->keys,&key,5)==FuriStatusOk;
        furi_mutex_acquire(a->mutex,FuriWaitForever);
        if(a->receiving) {
            drain(a);uint32_t now=furi_get_tick();
            TimedAction due=recorder_due(a->page,a->step,(now-a->start)*1000/furi_kernel_get_tick_frequency(),(now-a->phase)*1000/furi_kernel_get_tick_frequency());
            if(a->back || a->exit_requested) {a->back=false;finish(a,"Stopped early; partial");got=false;}
            else if(a->fault) {finish(a,a->fault==1?"Timer overcapture":"LF carrier too dense");got=false;}
            else if(!a->disk_ok) {finish(a,"SD write failed");got=false;}
            else if(a->lost) {finish(a,"Capture buffer overflow");got=false;}
            else if(due==TimeLimit) {finish(a,"120s limit; partial");got=false;}
            else if((due==PromptOn || due==FinishIdle)) {
                if(a->step) a->page=WaitOn;else finish(a,NULL);got=false;
            } else if(due==FinishTail) {finish(a,NULL);got=false;}
            else if(got && key==InputKeyOk && a->page==WaitOn) {a->on=true;a->on_tick=now;a->page=Active;got=false;}
            else if(got && key==InputKeyOk && a->page==Active) {a->off=true;a->off_tick=now;a->phase=now;a->page=Tail;got=false;}
        } else if(a->back) {a->back=false;quit=true;}
        if(a->exit_requested) quit=true;
        if(got && !quit && !a->receiving && ready) {
            if(a->page==Prepare && (a->step==0 || a->step==3) && (key==InputKeyLeft || key==InputKeyRight)) {
                if(key==InputKeyLeft && a->spacing>1) a->spacing--;
                if(key==InputKeyRight && a->spacing<30) a->spacing++;
            }
            else if(key==InputKeyOk && a->page==Intro) a->page=Prepare;
            else if(key==InputKeyOk && a->page==Prepare && (a->step!=3 || a->spacing!=a->original_spacing)) start(a);
            else if(a->page==Result && (key==InputKeyOk||key==InputKeyLeft)) {
                if(a->blocked) {got=false;}
                else if(a->passed && key==InputKeyOk) {a->step++;if(a->step==3) a->spacing=a->original_spacing==10?5:10;}
                if(!a->blocked) a->page=a->step==4?Done:Prepare;
            }
        }
        furi_mutex_release(a->mutex);
        if(furi_get_tick()-last_draw>=furi_ms_to_ticks(50)) {view_port_update(view);last_draw=furi_get_tick();}
    }
    furi_thread_set_signal_callback(furi_thread_get_current(),NULL,NULL);
    view_port_enabled_set(view,false);gui_remove_view_port(gui,view);view_port_free(view);
    storage_file_close(a->log);storage_file_free(a->log);storage_file_free(a->raw);
    furi_record_close(RECORD_STORAGE);furi_record_close(RECORD_GUI);
    furi_stream_buffer_free(a->stream);furi_message_queue_free(a->keys);furi_mutex_free(a->mutex);free(a);return 0;
}
