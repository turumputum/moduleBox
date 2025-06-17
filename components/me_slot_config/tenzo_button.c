#include "tenzo_button.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"

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

extern uint8_t SLOTS_PIN_MAP[10][4];
extern adc_channel_t SLOT_ADC_MAP[6];
extern configuration me_config;
extern stateStruct me_state;


#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "TENZO_BUTTON";

void tenzo_button_task(void *arg){
    int slot_num = *(int*) arg;

    ChanCfg adc_channel;
    adc_channel.chan= SLOT_ADC_MAP[slot_num];
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

    //---options block---
    uint32_t detThres = 100*1024; // distance for mass to go
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
		detThres = get_option_int_val(slot_num, "threshold", "", 10, 1, 4096)*1024;
		ESP_LOGD(TAG, "Set threshold :%ld for slot:%d",detThres/1024, slot_num);
	}
    
    int integrMult = 50; // 1/moving_mass (20...100) inertia
    if (strstr(me_config.slot_options[slot_num], "inertia") != NULL) {
		integrMult = get_option_int_val(slot_num, "inertia", "", 10, 1, 4096)*1024;
		ESP_LOGD(TAG, "Set inertia :%d for slot:%d", integrMult, slot_num);
	}

    int oversample = 8; //
    if (strstr(me_config.slot_options[slot_num], "oversample") != NULL) {
		oversample = get_option_int_val(slot_num, "oversample", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set oversample:%d for slot:%d",oversample, slot_num);
	}

    int boolean = 0; // 
    if (strstr(me_config.slot_options[slot_num], "boolean") != NULL) {
		boolean = 1; 
		ESP_LOGD(TAG, "Set boolean output for slot:%d", slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/tenzoButton_0")+3];
		sprintf(t_str, "%s/tenzoButton_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
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
        for (uint8_t j=0; j < oversample; j++){
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
        //printf("%d,",adc_channel.val);
        //ESP_LOGD(TAG, "ADC Channel[%d] Raw Data: %d", adc_channel.chan,  adc_channel.val);
        
        //--- dimonMagic math ---
        adc_channel.val = adc_channel.val*1024;
        if (adc_channel._val == 0) adc_channel._val = adc_channel.val;

        //assymetric HPF value
        int hpfVal = adc_channel._val > adc_channel.val ? hpfDown : hpfUp;
        int currentVal = (adc_channel._val + (((adc_channel.val-adc_channel._val)*hpfVal)/1024));
        int dcCorrected = adc_channel.val - currentVal;

        adc_channel.integrated = dcCorrected > integrThres ? (adc_channel.integrated + (dcCorrected * integrMult)/1024) : 0;
        adc_channel.integrated2 = dcCorrected > integrThres ? (adc_channel.integrated2 + adc_channel.integrated) : 0;

        //printf("%d,%d,%d\n",currentVal, adc_channel.integrated, adc_channel.integrated2);
        // if((adc_channel.integrated>0)||(adc_channel.integrated2>0)){
        //     ESP_LOGD(TAG, "dcCorrected:%d Iint_1:%d Int_2:%d detThres:%d",dcCorrected,adc_channel.integrated, adc_channel.integrated2, detThres);
        // }

        char str[255];

        if (adc_channel.integrated2 > detThres){
            if (adc_channel.trig == 0){
                adc_channel.trig = 1;
                memset(str, 0, strlen(str));
                if(boolean){
                    sprintf(str, "1");
                }else{
                    sprintf(str, "%d",((adc_channel.integrated/1024) * (adc_channel.integrated/1024)) + 1);
                }
                report(str, slot_num);
                gpio_set_level(ledPin, 1);
            }
        }else{ 
            if (adc_channel.trig == 1){
            adc_channel.trig = 0;
                memset(str, 0, strlen(str));
                sprintf(str, "0");
                report(str, slot_num);
                gpio_set_level(ledPin, 0);
            }
        }

        adc_channel._val=currentVal;
    }

    EXIT:
}


void start_tenzo_button_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_tenzo_button_%d", slot_num);
	xTaskCreatePinnedToCore(tenzo_button_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"tenzo_button_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
