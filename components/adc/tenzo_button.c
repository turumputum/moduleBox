#include "tenzo_button.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "driver/adc.h"
#include "esp_adc_cal.h"

#include <generated_files/gen_tenzo_button.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern adc_channel_t SLOT_ADC_MAP[6];
extern configuration me_config;
extern stateStruct me_state;


#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "TENZO_BUTTON";

typedef struct {
    uint32_t    detThres;
    int         integrMult;
    int         oversample;
    int         boolean;
    int         valReport;
} tenzo_ctx_t;

/*
    Тензо-кнопка - нажатие определяется по силе на тензодатчике через ADC
    slots: 0-5
*/
void configure_tenzoButton(tenzo_ctx_t *c, int slot_num)
{
    /* Порог срабатывания нажатия, По умолчанию 100
    */
    c->detThres = get_option_int_val(slot_num, "threshold", "", 100, 1, 4096) * 1024;
    ESP_LOGD(TAG, "Set threshold:%ld for slot:%d", c->detThres / 1024, slot_num);

    /* Инерция - больше значение, медленнее реакция, По умолчанию 50
    */
    c->integrMult = get_option_int_val(slot_num, "inertia", "", 50, 1, 4096);
    ESP_LOGD(TAG, "Set inertia:%d for slot:%d", c->integrMult, slot_num);

    /* Усреднение выборок ADC, По умолчанию 8
    */
    c->oversample = get_option_int_val(slot_num, "oversample", "", 8, 1, 64);
    ESP_LOGD(TAG, "Set oversample:%d for slot:%d", c->oversample, slot_num);

    /* Флаг - слать 1-0 вместо величины силы
    */
    c->boolean = get_option_flag_val(slot_num, "boolean");
    ESP_LOGD(TAG, "Set boolean:%d for slot:%d", c->boolean, slot_num);

    {
        char t_str[strlen(me_config.deviceName) + strlen("/tenzoButton_0") + 3];
        sprintf(t_str, "%s/tenzoButton_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* Сила нажатия, 0 при отпускании
    */
    c->valReport = stdreport_register(RPTT_int, slot_num, "", "event/val");
}

void tenzo_button_task(void *arg){
    int slot_num = (int)(intptr_t)arg;

    tenzo_ctx_t c = {0};
    configure_tenzoButton(&c, slot_num);

    ChanCfg adc_channel;
    adc_channel.chan = SLOT_ADC_MAP[slot_num];
    gpio_reset_pin(SLOTS_PIN_MAP[slot_num][0]);
	gpio_set_direction(SLOTS_PIN_MAP[slot_num][0], GPIO_MODE_INPUT);

    gpio_num_t ledPin = SLOTS_PIN_MAP[slot_num][1];
    esp_rom_gpio_pad_select_gpio(ledPin);
	gpio_set_direction(ledPin, GPIO_MODE_OUTPUT);
    gpio_set_level(ledPin, 0);

     //ADC1 config
    if(slot_num==2){
        //ADC2 config
        adc_channel.adc = ADC_UNIT_2;
        adc2_config_channel_atten(adc_channel.chan, ADC_ATTEN_DB_11);
    }else if(slot_num==1){
        ESP_LOGW(TAG, "no adc channel on this slot(((");
        goto EXIT;
    }else{
       adc_channel.adc = ADC_UNIT_1;
       adc1_config_width(ADC_ATTEN_DB_11);
       adc1_config_channel_atten(adc_channel.chan, ADC_ATTEN_DB_11);
    }

    TickType_t lastWakeTime;
    lastWakeTime = xTaskGetTickCount();

    adc_channel.trig = 0;
    adc_channel._val = 0;
    adc_channel.integrated = 0;
    adc_channel.integrated2 = 0;

    waitForWorkPermit(slot_num);

    while(1){
        vTaskDelayUntil(&lastWakeTime, 10);

        int hpfUp = 1; //high pass filter coeff
        int hpfDown = 200; //high pass filter coeff when signal goes down

        int integrThres = 20 *1024; // dead-band, values above this are ignored (1...50 *1024)

        adc_channel.val = 0;
        adc_channel.samples = 0;
        for (uint8_t j=0; j < c.oversample; j++){
            int val=-1;
            if(adc_channel.adc == ADC_UNIT_1){
                val = adc1_get_raw(adc_channel.chan);
            }else{
               adc2_get_raw(adc_channel.chan,ADC_ATTEN_DB_11, &val);
            }
            adc_channel.val += val;
            adc_channel.samples++;
        }

        adc_channel.val = adc_channel.val / adc_channel.samples;

        //--- dimonMagic math ---
        adc_channel.val = adc_channel.val*1024;
        if (adc_channel._val == 0) adc_channel._val = adc_channel.val;

        //assymetric HPF value
        int hpfVal = adc_channel._val > adc_channel.val ? hpfDown : hpfUp;
        int currentVal = (adc_channel._val + (((adc_channel.val-adc_channel._val)*hpfVal)/1024));
        int dcCorrected = adc_channel.val - currentVal;

        adc_channel.integrated = dcCorrected > integrThres ? (adc_channel.integrated + (dcCorrected * c.integrMult)/1024) : 0;
        adc_channel.integrated2 = dcCorrected > integrThres ? (adc_channel.integrated2 + adc_channel.integrated) : 0;

        if (adc_channel.integrated2 > c.detThres){
            if (adc_channel.trig == 0){
                adc_channel.trig = 1;
                int force = c.boolean ? 1 : ((adc_channel.integrated/1024) * (adc_channel.integrated/1024)) + 1;
                stdreport_i(c.valReport, force);
                gpio_set_level(ledPin, 1);
            }
        }else{
            if (adc_channel.trig == 1){
                adc_channel.trig = 0;
                stdreport_i(c.valReport, 0);
                gpio_set_level(ledPin, 0);
            }
        }

        adc_channel._val=currentVal;
    }

    EXIT:
    vTaskDelete(NULL);
}


void start_tenzo_button_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	char tmpString[60];
	sprintf(tmpString, "task_tenzo_button_%d", slot_num);
	xTaskCreatePinnedToCore(tenzo_button_task, tmpString, 1024*4, (void*)(intptr_t)slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"tenzo_button_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_tenzo_button()
{
	return manifesto;
}
