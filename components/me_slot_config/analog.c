#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>
#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"

#include <generated_files/gen_analog.h>


extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ANALOG";

#define MODE_3V3 0
#define MODE_5V 1
#define MODE_10V 2


typedef struct __tag_ANALOG_CONFIG
{
	bool 					used;
	int 					pattern_num;
	TaskHandle_t			s_task_handle;
	int 					slot_num;

	int 					averagingCount;
	uint64_t				averagingSum;

	uint16_t 				result;
	uint16_t 				prev_result;

    uint16_t 				MIN_VAL;
    uint16_t 				MAX_VAL;
    uint8_t 				flag_float_output;
	uint8_t 				inverse;
	float 					k;
	uint16_t 				dead_band;
	uint16_t 				periodic;

	char *					custom_topic;
	int 					flag_custom_topic;
	int 					divider;

} ANALOG_CONFIG, * PANALOG_CONFIG; 

/* 
    Старая версия поддержки ADC
*/
void configure_analog(PANALOG_CONFIG	ch, int slot_num)
{
    if (strstr(me_config.slot_options[slot_num], "floatOutput")!=NULL){
        /* Флаг определяет формат воводящего значения, 
          если указан - будет выводиться значение с плавающей точкой,
          иначе - целочисленное
        */
		ch->flag_float_output = get_option_flag_val(slot_num, "floatOutput");
		ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "maxVal")!=NULL){
		/* Определяет верхний порог значений */
		ch->MAX_VAL = get_option_int_val(slot_num, "maxVal", "", 4096, 1, 4096);
		ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", ch->MAX_VAL, slot_num);
	}
    if (strstr(me_config.slot_options[slot_num], "minVal")!=NULL){
		/* Определяет нижний порог значений */
		ch->MIN_VAL = get_option_int_val(slot_num, "minVal", "", 0, 1, 4096);
		ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", ch->MIN_VAL, slot_num);
	}

	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		/* Флаг задаёт инвертирование значений */
		ch->inverse = get_option_flag_val(slot_num, "inverse");
	}

    if (strstr(me_config.slot_options[slot_num], "filterK")!=NULL){
		/* Коэфициент фильтрации */
        ch->k = get_option_float_val(slot_num, "filterK", 1);
		ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", ch->k, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "deadBand")!=NULL){
		/* Фильтрация "дребезга" - определяет порог срабатывания */
        ch->dead_band = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set dead_band:%d. Slot:%d", ch->dead_band, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "periodic")!=NULL){
		/* Задаёт периодичночть отсчётов в миллисекундах */
        ch->periodic = get_option_int_val(slot_num, "periodic", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set periodic:%d. Slot:%d", ch->periodic, slot_num);
	}

	if (strstr(me_config.slot_options[slot_num], "topic")!=NULL){
		/* Определяет топик для MQTT сообщений */
		ch->custom_topic = get_option_string_val(slot_num,"topic");
		ESP_LOGD(TAG, "Custom topic:%s", ch->custom_topic);
		ch->flag_custom_topic=1;
	}


    if (strstr(me_config.slot_options[slot_num], "dividerMode")!=NULL){
		/* Задаёт режим делителя */
        if ((ch->divider = get_option_enum_val(slot_num, "dividerMode", "5V", "3V3", "10V", NULL)) < 0)
		{
			ESP_LOGE(TAG, "dividerMode: unricognized value");
		}
	}
}

void analog_task(void *arg)
{
	ANALOG_CONFIG		c = {0};
    c.k=1;
	uint16_t raw_val;
    uint16_t resault=0, prev_resault=0xFFFF;

	char tmpString[255];

    int slot_num = *(int *)arg;
	uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];

    if(slot_num==1){
        char errorString[300];
        sprintf(errorString, "no ADC on SLOT_1, use another slot");
        ESP_LOGE(TAG, "%s", errorString);
        writeErrorTxt(errorString);
        vTaskDelay(200);
        vTaskDelete(NULL);
    }

	static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
	static const adc_atten_t atten = ADC_ATTEN_DB_11; // ADC_ATTEN_DB_11

	gpio_reset_pin(sens_pin_num);
	gpio_set_direction(sens_pin_num, GPIO_MODE_INPUT);
    adc_channel_t ADC_chan = slot_num;

	if(slot_num==2){
		ADC_chan = ADC2_CHANNEL_6;
		//adc2_pad_get_io_num(ADC_chan, &sens_pin_num);
		adc2_config_channel_atten( ADC_chan, atten );
	}else{
		switch (slot_num){
		case 0:
			ADC_chan = ADC1_CHANNEL_3;
			break;
		case 3:
			ADC_chan = ADC1_CHANNEL_2;
			break;
		case 4:
			ADC_chan = ADC1_CHANNEL_1;
			break;
		case 5:
			ADC_chan = ADC1_CHANNEL_6;
			break;
		}
		adc1_config_width(width);
    	adc1_config_channel_atten(ADC_chan, atten);
	}

	configure_analog(&c, slot_num);

	uint8_t divPin_1 = SLOTS_PIN_MAP[slot_num][2];
	esp_rom_gpio_pad_select_gpio(divPin_1);
	gpio_set_direction(divPin_1, GPIO_MODE_OUTPUT);
	uint8_t divPin_2 = SLOTS_PIN_MAP[slot_num][1];
	esp_rom_gpio_pad_select_gpio(divPin_2);
	gpio_set_direction(divPin_2, GPIO_MODE_OUTPUT);

	switch (c.divider)
	{
		case 1:
			gpio_set_level(divPin_1, 0);
			gpio_set_level(divPin_2, 0);
			ESP_LOGD(TAG, "Set dividerMode:3V3. Slot:%d", slot_num);
			break;

		case 2:
			gpio_set_level(divPin_1, 0);
			gpio_set_level(divPin_2, 1);
			ESP_LOGD(TAG, "Set dividerMode:10V. Slot:%d", slot_num);
			break;
		
		default:
			gpio_set_level(divPin_1, 1);
			gpio_set_level(divPin_2, 0);
			ESP_LOGD(TAG, "Set dividerMode:5V. Slot:%d", slot_num);
			break;
	}

    if(c.flag_custom_topic==0){
		char *str = calloc(strlen(me_config.deviceName)+strlen("/analog_")+4, sizeof(char));
		sprintf(str, "%s/analog_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.trigger_topic_list[slot_num]=c.custom_topic;
	}


	uint8_t oversumple = 150;

	uint32_t tmp = 0;

	waitForWorkPermit(slot_num);

	TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {

		tmp = 0;
		for(int i=0;i<oversumple;i++){
			if(c.inverse){
				if(slot_num==2){
					adc2_get_raw(ADC_chan, width, &raw_val);
					raw_val = 4096-raw_val;
				}else{
					raw_val = 4096-adc1_get_raw(ADC_chan);
				}
			}else{
				if(slot_num==2){
					adc2_get_raw(ADC_chan, width, &raw_val);
				}else{
					raw_val = adc1_get_raw(ADC_chan);
				}
			}
			tmp =tmp+raw_val;
			//vTaskDelay(1);
		}
		tmp = tmp/oversumple;
		resault =resault*(1-c.k)+tmp*c.k;
		//printf(">val:%d\n",resault);
		//ESP_LOGD(TAG, "raw_val:%d time:%ld", raw_val, xTaskGetTickCount()-startTick);

        if((abs(resault - prev_resault)>c.dead_band)||(c.periodic!=0)){
            prev_resault = resault;
            //ESP_LOGD(TAG, "analog val:%d , allow_delta:%d", resault, MAX_VAL-MIN_VAL);

			int str_len;//=strlen(me_config.deviceName)+strlen("/tachometer_")+8;
			char *str;// = (char*)malloc(str_len * sizeof(char));

            float f_res = 0;
            if(c.flag_float_output){
				f_res = resault;
				if(f_res>c.MAX_VAL)f_res=c.MAX_VAL;
				if(f_res<c.MIN_VAL)f_res=c.MIN_VAL;
				f_res-=c.MIN_VAL;
				f_res = (float)f_res/(c.MAX_VAL-c.MIN_VAL);
            }

			memset(tmpString, 0, strlen(tmpString));

            if(c.flag_float_output){
				//sprintf(str,"%s/analog_%d:%f", me_config.deviceName, slot_num, f_res);
				sprintf(tmpString,"%f", f_res);
			}else{
				//sprintf(str,"%s/analog_%d:%d", me_config.deviceName, slot_num, resault);
				sprintf(tmpString,"%d", resault);
			}

			//ESP_LOGD(TAG, "result = %s", tmpString);

			report(tmpString, slot_num);
			//free(str); 

        }
        //ESP_LOGD(TAG, "analog val:%d", resault);
        if(c.periodic!=0){
			vTaskDelayUntil(&lastWakeTime, c.periodic);
		}else{
			vTaskDelayUntil(&lastWakeTime, 32);
		}
    }
    
}
void start_analog_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	xTaskCreatePinnedToCore(analog_task, "analog_task", 1024 * 4, &t_slot_num, 12, NULL,1);
	//xTaskCreate(analog_task, "analog_task", 1024 * 4, &t_slot_num, 12, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "analog_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
const char * get_manifest_analog()
{
	return manifesto;
}
