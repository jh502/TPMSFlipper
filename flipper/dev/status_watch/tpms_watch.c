// SPDX-License-Identifier: GPL-2.0-or-later
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <stdlib.h>
#include "tpms_decoder.h"
#include "tpms_preset.h"
#include "watch_state.h"
#include "watch_live.h"

typedef struct {uint32_t seq,duration,tick;bool level;} Pulse;
typedef struct {uint32_t id,count,last;TpmsFrame frame;WatchState state;WatchLive live;} Sensor;
typedef struct {
    FuriStreamBuffer* pulses;FuriMessageQueue* keys;FuriMutex* mutex;
    volatile uint32_t seq,loss;
    uint32_t tick,start;Sensor sensors[8];uint8_t count,selected,page;
    bool alert,log_ok;File* log;NotificationApp* notifications;
} App;
static void capture(bool level,uint32_t duration,void* ctx) {
    App* a=ctx;Pulse p={.seq=++a->seq,.duration=duration,.tick=furi_get_tick(),.level=level};
    if(furi_stream_buffer_spaces_available(a->pulses)<sizeof(p) ||
       furi_stream_buffer_send(a->pulses,&p,sizeof(p),0)!=sizeof(p))a->loss++;
}
static void write_log(App* a,const char* line) {
    if(a->log_ok && storage_file_write(a->log,line,strlen(line))!=strlen(line))a->log_ok=false;
}
static void frame(const TpmsFrame* f,void* ctx) {
    App* a=ctx;if(f->raw_len!=9)return;
    furi_mutex_acquire(a->mutex,FuriWaitForever);
    unsigned i=0;while(i<a->count && a->sensors[i].id!=f->id)i++;
    if(i==a->count) {if(a->count==8){furi_mutex_release(a->mutex);return;}a->sensors[i].id=f->id;a->count++;}
    Sensor* s=&a->sensors[i];s->frame=*f;s->count++;s->last=a->tick;
    watch_live_update(&s->live,f->raw[4],f->raw[7],a->tick);
    uint8_t changed=watch_observe(&s->state,f->raw[4],f->raw[7],a->tick);
    if(changed && i==a->selected)a->alert=true;
    char raw[19];for(unsigned b=0;b<9;b++)snprintf(raw+b*2,3,"%02X",f->raw[b]);
    char line[160];snprintf(line,sizeof(line),"rx,%lu,%08lX,%s,%u,%u,%lu\n",
        (unsigned long)(a->tick-a->start),(unsigned long)f->id,raw,s->state.armed,changed,(unsigned long)a->loss);
    write_log(a,line);
    furi_mutex_release(a->mutex);
}
static void input(InputEvent* e,void* ctx) {
    App* a=ctx;
    if(e->type==InputTypeShort || e->type==InputTypeLong)furi_message_queue_put(a->keys,e,0);
}
static void binary(uint8_t v,char* out) {for(unsigned i=0;i<8;i++)out[i]=(v&(0x80>>i))?'1':'0';out[8]=0;}
static void draw(Canvas* c,void* ctx) {
    App* a=ctx;char line[64];furi_mutex_acquire(a->mutex,FuriWaitForever);
    canvas_clear(c);canvas_set_font(c,FontSecondary);
    if(!a->count) {
        canvas_draw_str(c,0,10,"TPMS Watch 433.92 FSK");canvas_draw_str(c,0,25,"Waiting for TG1C...");
        canvas_draw_str(c,0,38,"Wake sensor with a tool");
    } else {
        Sensor* s=&a->sensors[a->selected];WatchState* w=&s->state;
        WatchLive* live=&s->live;
        const char* pages[]={"LIVE","MEAN","BITS","HIST","RAW","INFO"};
        snprintf(line,sizeof(line),"%08lX %c %s%s",(unsigned long)s->id,w->armed?'A':'L',pages[a->page],(!a->log_ok||a->loss)?" !":"");
        canvas_draw_str(c,0,9,line);
        switch(a->page) {
        case 0:
            snprintf(line,sizeof(line),"NOW B4:%02X B7:%02X",s->frame.raw[4],s->frame.raw[7]);canvas_draw_str(c,0,20,line);
            canvas_draw_str(c,0,31,watch_live_reference(live->b4,live->b7));
            snprintf(line,sizeof(line),"%ld.%02ld kPa  %dC",(long)(s->frame.pressure_kpa_x100/100),(long)(s->frame.pressure_kpa_x100%100),s->frame.temperature_c);canvas_draw_str(c,0,42,line);
            snprintf(line,sizeof(line),"RX:%lu age:%lus",(unsigned long)s->count,(unsigned long)((furi_get_tick()-s->last)/furi_kernel_get_tick_frequency()));canvas_draw_str(c,0,53,line);
            break;
        case 1:
            canvas_draw_str(c,0,20,watch_live_status(live->b7));
            canvas_draw_str(c,0,31,live->b7==2?"Wake phase? HYPOTHESIS":"Mode meaning UNKNOWN");
            canvas_draw_str(c,0,42,live->b4==0x87?"B4 87: Good reference":live->b4==0xf7?"B4 F7: Bad reference":"B4: meaning unknown");
            canvas_draw_str(c,0,53,"Parking not decoded");
            break;
        case 2: {
            char bits[9];binary(live->b4,bits);snprintf(line,sizeof(line),"B4 %02X %s",live->b4,bits);canvas_draw_str(c,0,20,line);
            binary(live->b7,bits);snprintf(line,sizeof(line),"B7 %02X %s",live->b7,bits);canvas_draw_str(c,0,31,line);
            snprintf(line,sizeof(line),"Last XOR B4:%02X B7:%02X",live->delta4,live->delta7);canvas_draw_str(c,0,42,line);
            canvas_draw_str(c,0,53,"Bit meanings UNKNOWN");break;
        }
        case 3:
            for(unsigned h=0;h<3;h++) {
                if(h<live->history_count) {
                    WatchTransition* t=&live->history[h];
                    snprintf(line,sizeof(line),"%lus %02X/%02X>%02X/%02X",(unsigned long)((t->tick-a->start)/furi_kernel_get_tick_frequency()),t->old4,t->old7,t->b4,t->b7);
                } else snprintf(line,sizeof(line),"--");
                canvas_draw_str(c,0,20+11*h,line);
            }
            snprintf(line,sizeof(line),"Transitions: %lu",(unsigned long)live->transitions);canvas_draw_str(c,0,53,line);break;
        case 4:
            snprintf(line,sizeof(line),"ID %02X %02X %02X %02X",s->frame.raw[0],s->frame.raw[1],s->frame.raw[2],s->frame.raw[3]);canvas_draw_str(c,0,20,line);
            snprintf(line,sizeof(line),"%02X %02X %02X %02X %02X",s->frame.raw[4],s->frame.raw[5],s->frame.raw[6],s->frame.raw[7],s->frame.raw[8]);canvas_draw_str(c,0,31,line);
            canvas_draw_str(c,0,42,"B4 P T B7 XOR: valid");
            snprintf(line,sizeof(line),"RX gap:%lums",(unsigned long)((uint64_t)live->interval*1000/furi_kernel_get_tick_frequency()));canvas_draw_str(c,0,53,line);break;
        default:
            snprintf(line,sizeof(line),"Base:%u %s",w->count,w->overflow?"FULL!":w->armed?"armed":"learning");canvas_draw_str(c,0,20,line);
            if(w->changed)snprintf(line,sizeof(line),"First hit %02X/%02X",w->trigger4,w->trigger7);
            else snprintf(line,sizeof(line),"No latched change");
            canvas_draw_str(c,0,31,line);
            snprintf(line,sizeof(line),"Good seen:%s",w->good_reached?"YES":"no");canvas_draw_str(c,0,42,line);
            snprintf(line,sizeof(line),"Loss:%lu SD:%s",(unsigned long)a->loss,a->log_ok?"OK":"ERR");canvas_draw_str(c,0,53,line);break;
        }
    }
    canvas_draw_str(c,0,63,"U/D:page OK:arm/reset");furi_mutex_release(a->mutex);
}

int32_t tpms_watch_app(void* p) {
    UNUSED(p);App* a=calloc(1,sizeof(App));a->start=furi_get_tick();
    a->mutex=furi_mutex_alloc(FuriMutexTypeNormal);a->pulses=furi_stream_buffer_alloc(16384,sizeof(Pulse));
    a->keys=furi_message_queue_alloc(8,sizeof(InputEvent));
    Storage* storage=furi_record_open(RECORD_STORAGE);a->log=storage_file_alloc(storage);
    storage_common_mkdir(storage,"/ext/apps_data/tpms_watch");
    char path[100];unsigned n=0;
    do{snprintf(path,sizeof(path),"/ext/apps_data/tpms_watch/watch_%04u.csv",n++);}while(storage_file_exists(storage,path)&&n<100000);
    a->log_ok=storage_file_open(a->log,path,FSAM_WRITE,FSOM_CREATE_NEW);
    char header[128];snprintf(header,sizeof(header),"# TG1C 433920000; tick_hz=%lu\nevent,tick,id,raw,armed,event_bits,rx_loss\n",(unsigned long)furi_kernel_get_tick_frequency());write_log(a,header);
    a->notifications=furi_record_open(RECORD_NOTIFICATION);
    Gui* gui=furi_record_open(RECORD_GUI);ViewPort* view=view_port_alloc();view_port_draw_callback_set(view,draw,a);view_port_input_callback_set(view,input,a);gui_add_view_port(gui,view,GuiLayerFullscreen);
    TpmsDecoder* decoder=tpms_decoder_alloc(frame,a);tpms_decoder_set_vdo_only(decoder,true);
    furi_hal_subghz_reset();furi_hal_subghz_load_custom_preset(tpms_fsk_preset);
    furi_hal_subghz_set_frequency_and_path(433920000);furi_hal_subghz_start_async_rx(capture,a);
    bool running=true;uint32_t redraw=0;
    while(running) {
        Pulse pulse;
        for(unsigned nread=0;nread<256;nread++) {
            if(furi_stream_buffer_receive(a->pulses,&pulse,sizeof(pulse),nread?0:1)!=sizeof(pulse))break;
            a->tick=pulse.tick;tpms_decoder_feed_sequenced(decoder,pulse.seq,pulse.level,pulse.duration);
        }
        InputEvent key;
        if(furi_message_queue_get(a->keys,&key,0)==FuriStatusOk) {
            furi_mutex_acquire(a->mutex,FuriWaitForever);
            if(key.key==InputKeyBack)running=false;
            else if(key.key==InputKeyDown)a->page=(a->page+1)%6;
            else if(key.key==InputKeyUp)a->page=(a->page+5)%6;
            else if(a->count) {
                Sensor* s=&a->sensors[a->selected];
                if(key.key==InputKeyOk) {
                    if(key.type==InputTypeLong)memset(&s->state,0,sizeof(s->state));
                    else watch_arm(&s->state);
                    char event[120];snprintf(event,sizeof(event),"%s,%lu,%08lX,,%u,0,%lu\n",key.type==InputTypeLong?"reset":"arm",(unsigned long)(furi_get_tick()-a->start),(unsigned long)s->id,s->state.armed,(unsigned long)a->loss);write_log(a,event);
                } else if(!s->state.armed && (key.key==InputKeyLeft||key.key==InputKeyRight))a->selected=(a->selected+(key.key==InputKeyRight?1:a->count-1))%a->count;
            }
            furi_mutex_release(a->mutex);
        }
        if(a->alert){notification_message(a->notifications,&sequence_success);a->alert=false;}
        if(furi_get_tick()-redraw>furi_ms_to_ticks(100)){view_port_update(view);redraw=furi_get_tick();}
    }
    furi_hal_subghz_stop_async_rx();furi_hal_subghz_sleep();
    Pulse tail;
    while(furi_stream_buffer_receive(a->pulses,&tail,sizeof(tail),0)==sizeof(tail)) {
        a->tick=tail.tick;tpms_decoder_feed_sequenced(decoder,tail.seq,tail.level,tail.duration);
    }
    snprintf(header,sizeof(header),"stop,%lu,,,,,%lu\n",(unsigned long)(furi_get_tick()-a->start),(unsigned long)a->loss);write_log(a,header);
    storage_file_close(a->log);storage_file_free(a->log);furi_record_close(RECORD_STORAGE);
    view_port_enabled_set(view,false);gui_remove_view_port(gui,view);view_port_free(view);furi_record_close(RECORD_GUI);
    tpms_decoder_free(decoder);furi_record_close(RECORD_NOTIFICATION);furi_stream_buffer_free(a->pulses);furi_message_queue_free(a->keys);furi_mutex_free(a->mutex);free(a);return 0;
}
