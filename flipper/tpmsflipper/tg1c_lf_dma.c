// SPDX-License-Identifier: GPL-2.0-or-later
/* TIM1 is the RFID carrier, TIM2 remains owned by Sub-GHz RX.
 * Official 1.5.1-rc reserves DMA2 ch1/ch2 for RFID/Sub-GHz *TX*; RX uses
 * TIM2 interrupts, and storage SPI uses ch6/ch7. Never use with RF TX.
 * No per-half-bit ISR: one DMA transfer at each 8us carrier period.
 * Physical envelope still requires user validation on the real device.
 *
 * Forked from tpms_bridge/tpms_lf_dma.c; the functional changes are the
 * explicit nslots parameter and, from v0.3.1, tg1c_lf_dma_reload (see
 * tg1c_lf_dma.h). */
#include "tg1c_lf_dma.h"
#include "tpms_lf_envelope.h"
#include "tpms_lf.h"
#include <furi.h>
#include <furi_hal.h>
#include <stm32wbxx_ll_dma.h>
#include <stm32wbxx_ll_tim.h>
static uint16_t* samples;
static uint32_t count;
static uint16_t first;
static bool active;
static const char* failure;
const char* tg1c_lf_dma_failure(void) {return failure?failure:"LF engine unavailable";}
size_t tg1c_lf_dma_count(void) {return count;}

bool tg1c_lf_dma_prepare(const uint8_t* slots,size_t nslots,uint16_t half_us,uint16_t lead_us) {
    failure=NULL;
    if(active || !slots || !nslots || nslots>TG1C_MAX_SLOTS ||
       half_us<112 || half_us>144 || half_us%8 || lead_us>16000 || lead_us%8) {
        failure="LF engine busy or invalid timing";return false;
    }
    free(samples);samples=NULL;
    /* Computed for the actual slot count, not the 184-slot LAB_SLOTS. The app's
     * session preflight (tg1c_seq.h) has already reserved ring + envelope. */
    count=(uint32_t)nslots*(half_us/8)+lead_us/8;
    if(memmgr_get_free_heap()<count*2+16384) {failure="Insufficient memory for LF envelope and reserve";return false;}
    samples=malloc(count*sizeof(uint16_t));
    if(!samples) {failure="LF envelope allocation failed";return false;}
    uint16_t duty=(SystemCoreClock/125000-1)/2;
    count=tpms_lf_envelope_lead(slots,nslots,half_us,lead_us,duty,&first,samples,count);
    if(!count)failure="Invalid LF envelope";
    return count!=0;
}
/* v0.3.1, dilution test: the next frame of the burst is a different one of the
 * same size. `count` is both the prepared transfer count and the size of the
 * allocation, so an envelope of exactly `count` samples fits. The slots are
 * checked before anything is written (tpms_lf_envelope validates first), so a
 * refusal leaves the previous envelope in place. */
bool tg1c_lf_dma_reload(const uint8_t* slots,size_t nslots,uint16_t half_us,uint16_t lead_us) {
    if(active || !samples || !count || !slots || !nslots || nslots>TG1C_MAX_SLOTS ||
       half_us<112 || half_us>144 || half_us%8 || lead_us>16000 || lead_us%8 ||
       (uint32_t)nslots*(half_us/8)+lead_us/8!=count) {
        failure="LF reload refused";return false;
    }
    uint16_t duty=(SystemCoreClock/125000-1)/2;
    uint16_t new_first=0;
    if(tpms_lf_envelope_lead(slots,nslots,half_us,lead_us,duty,&new_first,samples,count)!=count) {
        failure="Invalid LF envelope";return false;
    }
    first=new_first;
    return true;
}
bool tg1c_lf_dma_start(void) {
    if(active || !samples || furi_hal_bus_is_enabled(FuriHalBusTIM1) ||
       LL_DMA_IsEnabledChannel(DMA2,LL_DMA_CHANNEL_1)) return false;
    furi_hal_rfid_tim_read_start(125000,0);
    LL_TIM_DisableCounter(TIM1);
    LL_TIM_DisableAllOutputs(TIM1);
    LL_TIM_OC_DisablePreload(TIM1,LL_TIM_CHANNEL_CH1);
    LL_TIM_OC_SetCompareCH1(TIM1,first);
    LL_TIM_SetCounter(TIM1,0);
    LL_TIM_ClearFlag_UPDATE(TIM1);
    LL_DMA_DeInit(DMA2,LL_DMA_CHANNEL_1);
    LL_DMA_ClearFlag_GI1(DMA2);
    LL_DMA_InitTypeDef config={0};
    config.PeriphOrM2MSrcAddress=(uint32_t)&TIM1->CCR1;
    config.MemoryOrM2MDstAddress=(uint32_t)samples;
    config.Direction=LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    config.Mode=LL_DMA_MODE_NORMAL;
    config.PeriphOrM2MSrcIncMode=LL_DMA_PERIPH_NOINCREMENT;
    config.MemoryOrM2MDstIncMode=LL_DMA_MEMORY_INCREMENT;
    config.PeriphOrM2MSrcDataSize=LL_DMA_PDATAALIGN_HALFWORD;
    config.MemoryOrM2MDstDataSize=LL_DMA_MDATAALIGN_HALFWORD;
    config.NbData=count;
    config.PeriphRequest=LL_DMAMUX_REQ_TIM1_UP;
    config.Priority=LL_DMA_PRIORITY_HIGH;
    if(LL_DMA_Init(DMA2,LL_DMA_CHANNEL_1,&config)!=SUCCESS) {
        LL_DMA_DeInit(DMA2,LL_DMA_CHANNEL_1);
        tpms_lf_field_stop();return false;
    }
    LL_DMA_EnableChannel(DMA2,LL_DMA_CHANNEL_1);
    LL_TIM_EnableDMAReq_UPDATE(TIM1);
    LL_TIM_EnableAllOutputs(TIM1);
    active=true;
    LL_TIM_EnableCounter(TIM1);
    return true;
}
bool tg1c_lf_dma_done(void) {return active && LL_DMA_GetDataLength(DMA2,LL_DMA_CHANNEL_1)==0;}
bool tg1c_lf_dma_error(void) {return active && LL_DMA_IsActiveFlag_TE1(DMA2);}
void tg1c_lf_dma_stop(void) {
    if(!active)return;
    LL_TIM_DisableDMAReq_UPDATE(TIM1);
    LL_DMA_DisableChannel(DMA2,LL_DMA_CHANNEL_1);
    LL_TIM_OC_SetCompareCH1(TIM1,0);
    LL_DMA_DeInit(DMA2,LL_DMA_CHANNEL_1);
    tpms_lf_field_stop();active=false;
}
void tg1c_lf_dma_free(void) {tg1c_lf_dma_stop();free(samples);samples=NULL;count=0;}
