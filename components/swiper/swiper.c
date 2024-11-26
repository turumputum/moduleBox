#include <stdio.h>
#include "swiper.h"

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

#include "driver/i2c.h"
#include "apds9960.h"

#include "rgbHsv.h"
#include "driver/rmt_tx.h"
#include "math.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SWIPER";


void swiper_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];

    uint8_t up_down_disable = 0;
    if (strstr(me_config.slot_options[slot_num], "up_down_disable")!=NULL){
		up_down_disable=1;
	}

    if (strstr(me_config.slot_options[slot_num], "swiper_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "swiper_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/swiper_0")+3];
		sprintf(t_str, "%s/swiper_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.action_topic_list[slot_num]);
	} 



    // todo: check port
    i2c_port_t i2c_port = I2C_NUM_0;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = scl_pin,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = 100000,
        // .clk_flags = 0,          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
    };
    i2c_param_config(i2c_port, &conf);
    i2c_driver_install(i2c_port, conf.mode, 0, 0, 0);

    sensor_start(i2c_port);


    gesture_data_type gData;
    gData.state = WAITING;

    while(1){
        vTaskDelay(30 / portTICK_PERIOD_MS);
        if (readSensor(i2c_port, &gData) != ESP_OK){
            ESP_LOGE(TAG, "Error reading sensor");
            continue;
        }
        if(gData.state == GESTURE_END){
            if(processGesture(&gData)==ESP_OK){
                char str[255];
                memset(str, 0, strlen(str));
                if((gData.gesture == SWIPE_UP)&&(!up_down_disable)){
                    sprintf(str, "up");
                }else if((gData.gesture == SWIPE_DOWN)&&(!up_down_disable)){
                    sprintf(str, "down");
                }else if(gData.gesture == SWIPE_LEFT){
                    sprintf(str, "left");
                }else if(gData.gesture == SWIPE_RIGHT){
                    sprintf(str, "right");
                }else{
                    goto SKIP_REPORT;
                }
                report(str, slot_num);

                SKIP_REPORT:
                gData.state = WAITING;
                ESP_LOGD(TAG, "Gesture:%d duration:%ldms size:%d", gData.gesture, gData.duration/1000, gData.size);
            }
        }
    }

}


void start_swiper_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_swiper_%d", slot_num);
	xTaskCreatePinnedToCore(swiper_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);

	ESP_LOGD(TAG,"swiper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}




