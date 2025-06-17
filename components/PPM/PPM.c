#include <stdio.h>
#include "PPM.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "math.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

#include "soc/gpio_struct.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "PPM";


#define PPM_PULSE_LENGTH_US 400

static bool IRAM_ATTR timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data) {
    ppm_generator_t *ppm = (ppm_generator_t *)user_data;
    //printf(".");
    if (ppm->pulse_state) {
        // End of pulse
        gpio_set_level(ppm->pin, 1);
        //GPIO.out_w1ts = ((uint32_t)1 << ppm->pin);
        
        gptimer_set_raw_count(timer, 0);
        gptimer_alarm_config_t run_timer_config = {
            .reload_count = 0,
            .alarm_count = ppm->current_value - PPM_PULSE_LENGTH_US, //
            .flags.auto_reload_on_alarm = true,
        };
        gptimer_set_alarm_action(ppm->timer, &run_timer_config);
        ppm->pulse_state = false;
        
    } else {
        // Start of pulse
        gpio_set_level(ppm->pin, 0);
        //GPIO.out_w1tc = ((uint32_t)1 << ppm->pin);
        gptimer_set_raw_count(timer, 0);
        gptimer_alarm_config_t run_timer_config = {
            .reload_count = 0,
            .alarm_count = PPM_PULSE_LENGTH_US, //
            .flags.auto_reload_on_alarm = true,
        };
        gptimer_set_alarm_action(ppm->timer, &run_timer_config);
        ppm->pulse_state = true;
    }
    
    return true;
}

esp_err_t ppm_generator_init(ppm_generator_t *ppm, uint8_t gpio_pin) {
    ppm->pin = gpio_pin;
    ppm->current_value = 1500; // Default center position
    ppm->pulse_state = false;
    
    // Configure GPIO
    // gpio_config_t gpio_conf = {
    //     .pin_bit_mask = (1ULL << gpio_pin),
    //     .mode = GPIO_MODE_OUTPUT,
    //     .pull_up_en = GPIO_PULLUP_DISABLE,
    //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
    //     .intr_type = GPIO_INTR_DISABLE,
    // };
    // gpio_config(&gpio_conf);

    esp_rom_gpio_pad_select_gpio(gpio_pin);
    ESP_ERROR_CHECK(gpio_set_direction(gpio_pin, GPIO_MODE_OUTPUT));
    
    
    // Configure timer
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz = 1us resolution
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &ppm->timer));
    
    // Configure callback
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = timer_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(ppm->timer, &callbacks, ppm));

    // Start timer
    ESP_ERROR_CHECK(gptimer_enable(ppm->timer));
    gptimer_alarm_config_t run_timer_config = {
        .reload_count = 0,
        .alarm_count = PPM_PULSE_LENGTH_US, // period = 10us
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(ppm->timer, &run_timer_config);
    ESP_ERROR_CHECK(gptimer_start(ppm->timer));
    
    return ESP_OK;
}

void ppm_generator_set_value(ppm_generator_t *ppm, uint16_t value) {
    // Limit value between 1000us and 2000us
    if (value < 1000) value = 1000;
    if (value > 2000) value = 2000;
    ppm->current_value = value;
}

void ppm_generator_stop(ppm_generator_t *ppm) {
    gptimer_stop(ppm->timer);
    gptimer_disable(ppm->timer);
    gptimer_del_timer(ppm->timer);
}

void ppm_generator_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint16_t inputMaxVal = 255;
	if (strstr(me_config.slot_options[slot_num], "inputMaxVal") != NULL) {
		inputMaxVal = get_option_int_val(slot_num, "inputMaxVal", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set inputMaxVal:%d for slot:%d",inputMaxVal, slot_num);
	}

    int inverse[2]={0,0};
	if (strstr(me_config.slot_options[slot_num], "inverse_0")!=NULL){
		inverse[0]=1;
        ESP_LOGD(TAG, "Set inverse_0 for slot:%d", slot_num);
	}
    if (strstr(me_config.slot_options[slot_num], "inverse_1")!=NULL){
		inverse[1]=1;
        ESP_LOGD(TAG, "Set inverse_1 for slot:%d", slot_num);
	}


	uint16_t minPulseWidth = 1000;
	if (strstr(me_config.slot_options[slot_num], "minPulseWidth") != NULL) {
		minPulseWidth = get_option_int_val(slot_num, "minPulseWidth", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set minPulseWidth:%d for slot:%d",minPulseWidth, slot_num);
	}
    uint16_t maxPulseWidth = 2000;
	if (strstr(me_config.slot_options[slot_num], "maxPulseWidth") != NULL) {
		maxPulseWidth = get_option_int_val(slot_num, "maxPulseWidth", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set maxPulseWidth:%d for slot:%d",minPulseWidth, slot_num);
	}


    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
        me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/PPM_0")+3];
		sprintf(t_str, "%s/PPM_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}

    ppm_generator_t ppm_ch0 = {
        .pin = SLOTS_PIN_MAP[slot_num][0],
    };
    ppm_generator_init(&ppm_ch0, SLOTS_PIN_MAP[slot_num][0]);
    ppm_generator_set_value(&ppm_ch0, ((maxPulseWidth-minPulseWidth)/2)+minPulseWidth);

    ppm_generator_t ppm_ch1 = {
        .pin = SLOTS_PIN_MAP[slot_num][1],
    };
    ppm_generator_init(&ppm_ch1, SLOTS_PIN_MAP[slot_num][1]);
    ppm_generator_set_value(&ppm_ch1, ((maxPulseWidth-minPulseWidth)/2)+minPulseWidth);

    waitForWorkPermit(slot_num);

    while(1){
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
            char* payload;
			char* cmd;
			int val = inputMaxVal/2;
			if(strstr(msg.str, ":")!=NULL){
				cmd = strtok_r(msg.str, ":", &payload);
				val = strtol(payload, NULL, 10);
                if(val>inputMaxVal)val = inputMaxVal;
                if(val<0) val=0;
			}else{
				cmd = strdup(msg.str);
			}
            float fVal = (float)val/inputMaxVal;
            //ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            uint16_t valP = 1500;
            if(strstr(cmd, "ch_0")!=NULL){   
                valP = (uint16_t)((float)(maxPulseWidth-minPulseWidth)*fabs(inverse[0]-fVal))+minPulseWidth;
                ppm_generator_set_value(&ppm_ch0, valP);
                //ESP_LOGD(TAG, "Set valP:%d", valP);
            }else if(strstr(cmd, "ch_1")!=NULL){
                valP = (uint16_t)((float)(maxPulseWidth-minPulseWidth)*fabs(inverse[1]-fVal))+minPulseWidth;
                ppm_generator_set_value(&ppm_ch1, valP);
                //ESP_LOGD(TAG, "Set ppm_ch1:%d", valP);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

}


void start_ppm_generator_task(uint8_t slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "ppm_generator_%d", slot_num);
	xTaskCreatePinnedToCore(ppm_generator_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "ppm_generator_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}