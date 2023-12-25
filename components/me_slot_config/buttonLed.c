#include "buttonLed.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "driver/ledc.h"

#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

extern const uint8_t gamma_8[256];

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "BUTTONS";

enum animation{
	NONE,
	FLASH,
	GLITCH
};

static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void button_task(void *arg){
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][0];

	me_state.interrupt_queue[slot_num] = xQueueCreate(5, sizeof(uint8_t));

	gpio_reset_pin(pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
    gpio_config_t in_conf = {};
   	in_conf.intr_type = GPIO_INTR_ANYEDGE;
    in_conf.pin_bit_mask = (1ULL<<pin_num);
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(pin_num, gpio_isr_handler, (void*)slot_num);
	
	ESP_LOGD(TAG,"SETUP BUTTON_pin_%d Slot:%d", pin_num, slot_num );
	
	uint8_t button_state=0;
	int prev_state=0;
	char str[255];

	int button_inverse=0;
	if (strstr(me_config.slot_options[slot_num], "button_inverse")!=NULL){
		button_inverse=1;
	}


	int debounce_delay = 0;
	if (strstr(me_config.slot_options[slot_num], "button_debounce_delay") != NULL) {
		debounce_delay = get_option_int_val(slot_num, "button_debounce_delay");
		ESP_LOGD(TAG, "Set debounce_delay:%d for slot:%d",debounce_delay, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "button_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "button_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/button_0")+3];
		sprintf(t_str, "%s/button_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

	uint32_t tick=xTaskGetTickCount();
    for(;;) {
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			if(gpio_get_level(pin_num)){
				button_state=button_inverse ? 0 : 1;
			}else{
				button_state=button_inverse ? 1 : 0;
			}

			if(debounce_delay!=0){
				if((xTaskGetTickCount()-tick)<debounce_delay){
					ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
			
			if(button_state != prev_state){
				prev_state = button_state;

				memset(str, 0, strlen(str));
				sprintf(str, "%d", button_state);

				report(str, slot_num);
				//ESP_LOGD(TAG,"String:%s", str);
				tick = xTaskGetTickCount();
			}

			exit:
		}
    }

}

void start_button_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_button_%d", slot_num);
	xTaskCreate(button_task, tmpString, 1024*4, &t_slot_num,12, NULL);

	ESP_LOGD(TAG,"Button task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


void checkBright(uint8_t *currentBright, uint8_t targetBright, uint8_t fade_increment){
	if(*currentBright!=targetBright){
        if(*currentBright < targetBright){
            if((targetBright - *currentBright) < fade_increment){
              	*currentBright = targetBright;
            }else{
                *currentBright += fade_increment;
            }
        }else if(*currentBright > targetBright){
            if((*currentBright - targetBright) < fade_increment){
               	*currentBright = targetBright;
            }else{
                *currentBright -= fade_increment;
            }
        }
    }
}

void led_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    uint8_t state = 0;

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    //vQueueAddToRegistry( xQueue, "AMeaningfulName" );
    
    uint8_t inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "led_inverse")!=NULL){
		inverse=1;
	}

    uint8_t fade_increment = 255;
    if (strstr(me_config.slot_options[slot_num], "fade_increment") != NULL) {
		fade_increment = get_option_int_val(slot_num, "fade_increment");
		ESP_LOGD(TAG, "Set fade_increment:%d for slot:%d",fade_increment, slot_num);
	}

    uint16_t max_bright = 255;
    if (strstr(me_config.slot_options[slot_num], "max_bright") != NULL) {
		max_bright = get_option_int_val(slot_num, "max_bright");
        if(max_bright>255)max_bright=255;
		ESP_LOGD(TAG, "Set max_bright:%d for slot:%d",max_bright, slot_num);
	}

    uint16_t min_bright = 0;
    if (strstr(me_config.slot_options[slot_num], "min_bright") != NULL) {
		min_bright = get_option_int_val(slot_num, "min_bright");
        if(min_bright>255)min_bright=255;
		ESP_LOGD(TAG, "Set min_bright:%d for slot:%d",min_bright, slot_num);
	}

    uint16_t refreshRate_ms = 30;
    if (strstr(me_config.slot_options[slot_num], "refreshRate_ms") != NULL) {
		refreshRate_ms = get_option_int_val(slot_num, "refreshRate_ms");
		ESP_LOGD(TAG, "Set refreshRate_ms:%d for slot:%d",refreshRate_ms, slot_num);
	}

    int animate;
    if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "ledMode");
		if(!memcmp(tmp, "flash", 5)){
			animate = FLASH;
		}else if(!memcmp(tmp, "glitch", 5)){
			animate = GLITCH;
		}
		ESP_LOGD(TAG, "Custom animate:%s", tmp);
    }else{
        animate=NONE;
    }

	if (strstr(me_config.slot_options[slot_num], "led_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "led_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/led_0")+3];
		sprintf(t_str, "%s/led_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

	// Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 4000,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = slot_num,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = pin_num,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_LOGD(TAG, "Led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));


    uint8_t currentBright=0;
    uint8_t targetBright=inverse ? max_bright : min_bright;

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    while (1) {
        startTick = xTaskGetTickCount();

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
			int val = atoi(msg.str+strlen(me_state.action_topic_list[slot_num])+1);
			if(val==1){
				targetBright = max_bright;
			}else{
				targetBright = min_bright;
			}
        }

        if (animate == NONE){
            
        }
		checkBright(&currentBright, targetBright, fade_increment);
		ledc_set_duty(LEDC_LOW_SPEED_MODE, slot_num, currentBright);
		ledc_update_duty(LEDC_LOW_SPEED_MODE, slot_num);

        uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d", delay); 
        vTaskDelay(pdMS_TO_TICKS(delay));
    }

}


void start_led_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(led_task, "smartLed_task", 1024*4, &slot_num,12, NULL);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

