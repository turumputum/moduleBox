#include "sensor_2ch.h"
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

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SENSOR_2CH";


static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void sensors_task(void *arg){
	int slot_num = *(int*) arg;
	
	me_state.interrupt_queue[slot_num] = xQueueCreate(5, sizeof(uint8_t));

    uint8_t pin_num_1 = SLOTS_PIN_MAP[slot_num][0];
	gpio_reset_pin(pin_num_1);
	esp_rom_gpio_pad_select_gpio(pin_num_1);
    gpio_config_t in_conf = {};
   	in_conf.intr_type = GPIO_INTR_ANYEDGE;
    in_conf.pin_bit_mask = (1ULL<<pin_num_1);
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num_1, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(pin_num_1, gpio_isr_handler, (void*)slot_num);

    uint8_t pin_num_2 = SLOTS_PIN_MAP[slot_num][1];
    gpio_reset_pin(pin_num_2);
	esp_rom_gpio_pad_select_gpio(pin_num_2);
	in_conf.pin_bit_mask = (1ULL<<pin_num_2);
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num_2, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(pin_num_2, gpio_isr_handler, (void*)slot_num);

	ESP_LOGD(TAG,"SETUP sens  pin_%d and pin_%d Slot:%d",pin_num_1, pin_num_2, slot_num);
	
	uint8_t sens_1_state=0;
    uint8_t sens_2_state=0;
	uint8_t sens_1_prevState=0;
    uint8_t sens_2_prevState=0;
	char str[255];

	int ch_1_inverse=0;
	if (strstr(me_config.slot_options[slot_num], "ch_1_inverse")!=NULL){
		ch_1_inverse=1;
	}

    int ch_2_inverse=0;
	if (strstr(me_config.slot_options[slot_num], "ch_2_inverse")!=NULL){
		ch_2_inverse=1;
	}

	int debounce_gap = 0;
	if (strstr(me_config.slot_options[slot_num], "sens_debounce_gap") != NULL) {
		debounce_gap = get_option_int_val(slot_num, "sens_debounce_gap");
		ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",debounce_gap, slot_num);
	}

    int mode = INDEPENDENT_MODE;
	if (strstr(me_config.slot_options[slot_num], "mode") != NULL) {
        char* mode_str=NULL;
		mode_str = get_option_string_val(slot_num, "mode");
        if(strstr(mode_str,"OR_logic")!=NULL){
            mode= OR_LOGIC_MODE;
        }else if(strstr(mode_str,"AND_logic")!=NULL){
            mode= AND_LOGIC_MODE;
        }
		ESP_LOGD(TAG, "Set sens mode:%d for slot:%d",mode, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "sens_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "sens_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/sens_0")+3];
		sprintf(t_str, "%s/sens_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	uint32_t tick=xTaskGetTickCount();
    for(;;) {
		//vTaskDelay(pdMS_TO_TICKS(10));
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			if(gpio_get_level(pin_num_1)){
				sens_1_state=ch_1_inverse ? 0 : 1;
			}else{
				sens_1_state=ch_1_inverse ? 1 : 0;
			}
            if(gpio_get_level(pin_num_2)){
				sens_2_state=ch_2_inverse ? 0 : 1;
			}else{
				sens_2_state=ch_2_inverse ? 1 : 0;
			}

			if(debounce_gap!=0){
				if((xTaskGetTickCount()-tick)<debounce_gap){
					ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
            
			
            switch(mode){
                case INDEPENDENT_MODE:
                    if(sens_1_state != sens_1_prevState){
                        sens_1_prevState = sens_1_state;
                        memset(str, 0, strlen(str));
                        sprintf(str, "/ch_1:%d", sens_1_state);
                        report(str, slot_num);
                        tick = xTaskGetTickCount();
                    }
                    if(sens_2_state != sens_2_prevState){
                        sens_2_prevState = sens_2_state;
                        memset(str, 0, strlen(str));
                        sprintf(str, "/ch_2:%d", sens_2_state);
                        report(str, slot_num);
                        tick = xTaskGetTickCount();
                    }
                    break;
                
                case OR_LOGIC_MODE:
                    memset(str, 0, strlen(str));
                    if((sens_1_prevState==0)&&(sens_2_prevState==0)){
                        if((sens_1_state)||(sens_2_state)){
                            sprintf(str, "%d", 1);
                            tick = xTaskGetTickCount();
                            report(str, slot_num);
                            sens_1_prevState = sens_1_state;
                            sens_2_prevState = sens_2_state;
                        }
                    }else{
                        if((sens_1_state==0)&&(sens_2_state==0)){
                            sprintf(str, "%d", 0);
                            tick = xTaskGetTickCount();
                            report(str, slot_num);
                            sens_1_prevState = sens_1_state;
                            sens_2_prevState = sens_2_state;
                        }
                    }
                    
                    break;

                case AND_LOGIC_MODE:
                    memset(str, 0, strlen(str));
                    if((sens_1_prevState==0)||(sens_2_prevState==0)){
                        if((sens_1_state)&&(sens_2_state)){
                            sprintf(str, "%d", 1);
                            report(str, slot_num);
                            sens_1_prevState = sens_1_state;
                            sens_2_prevState = sens_2_state;
                        }
                    }else{
                        if((sens_1_state==0)&&(sens_2_state==0)){
                            sprintf(str, "%d", 0);
                            report(str, slot_num);
                            sens_1_prevState = sens_1_state;
                            sens_2_prevState = sens_2_state;
                        }
                    }
                    break;
            }
			
			exit:
		}
    }

}

void start_sensors_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_in_%d", slot_num);
	xTaskCreatePinnedToCore(sensors_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"Sensors task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
