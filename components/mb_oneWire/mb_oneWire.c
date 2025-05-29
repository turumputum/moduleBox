#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mb_oneWire.h"
#include "math.h"
#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"

#include "onewire_bus.h"
#include "ds18b20.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ONE_WIRE";

void ds18b20_task(void* arg) {
    int slot_num = *(int*)arg;
    uint8_t pin_DS18B20 = SLOTS_PIN_MAP[slot_num][0];
    onewire_bus_handle_t bus;
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = pin_DS18B20,
    };

    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));
    ESP_LOGI(TAG, "1-Wire bus installed on GPIO%d", pin_DS18B20);

    int ds18b20_device_num = 0;
    ds18b20_device_handle_t ds18b20s_dev;
    onewire_device_iter_handle_t iter = NULL;
    onewire_device_t next_onewire_device;
    esp_err_t search_result = ESP_OK;

    // create 1-wire device iterator, which is used for device search
    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(TAG, "Device iterator created, start searching...");
    do {
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
        if (search_result == ESP_OK) { // found a new device, let's check if we can upgrade it to a DS18B20
            ds18b20_config_t ds_cfg = {};
            if (ds18b20_new_device(&next_onewire_device, &ds_cfg, &ds18b20s_dev) == ESP_OK) {
                ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX", ds18b20_device_num, next_onewire_device.address);
                break;
            } else {
                ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    if(search_result == ESP_ERR_NOT_FOUND){
        char tmpString[strlen("DS18B20 not found, slot[0]") + 3];
        sprintf(tmpString, "DS18B20 not found, slot[%d]", slot_num);
        ESP_LOGE(TAG, "%s", tmpString);
        writeErrorTxt(tmpString);
        vTaskDelete(NULL);
    }

    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", ds18b20_device_num);

    // set resolution for all DS18B20s
    ds18b20_set_resolution(ds18b20s_dev, DS18B20_RESOLUTION_12B);

    float deadBand = 0.01;
    if(strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
        deadBand = abs(get_option_float_val(slot_num, "deadBand"));
        ESP_LOGD(TAG, "Sensor set dead_band:%f for slot:%d", deadBand, slot_num);
    }

    float threshold = 0.0;
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        threshold = get_option_int_val(slot_num, "threshold");
        ESP_LOGD(TAG, "threshold:%f. Slot:%d", threshold, slot_num);
    }

    uint8_t inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "stateInverse") != NULL) {
        inverse = 1;
        ESP_LOGD(TAG, "Setup inverse flag, for slot:%d",  slot_num);
    }
    uint8_t silent = 0;
    if (strstr(me_config.slot_options[slot_num], "silent") != NULL) {
        silent = 1;
        ESP_LOGD(TAG, "Setup silent flag, for slot:%d",  slot_num);
    }

    uint32_t periodic=0;
    if (strstr(me_config.slot_options[slot_num], "periodic")!=NULL){
        periodic = abs(get_option_int_val(slot_num, "periodic"))*1000;
		ESP_LOGD(TAG, "Set periodic:%ld. Slot:%d",periodic, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "tempTopic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/temp_0")+3];
		sprintf(t_str, "%s/temp_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart tempTopic:%s", me_state.trigger_topic_list[slot_num]);
	}

    // get temperature from sensors one by one
    float temperature;
    float prevTemperature = -55.0;
    char tmpString[155];

    uint8_t state=0;
    int8_t state_prev=-1;

    waitForWorkPermit(slot_num);

    while (1) {
        TickType_t xLastWakeTime = xTaskGetTickCount();
        

        ds18b20_trigger_temperature_conversion(ds18b20s_dev);
        if(ds18b20_get_temperature(ds18b20s_dev, &temperature)==ESP_OK) {
            //ESP_LOGD(TAG, "Temperature:%f C, delta:%f", temperature, fabs(temperature - prevTemperature));
            if((fabs(temperature - prevTemperature)>deadBand)||(periodic!=0)){
                prevTemperature = temperature;
                //ESP_LOGD(TAG, "Temperature: %f C", temperature);

                if(silent!=1){
                    memset(tmpString, 0, strlen(tmpString));
                    sprintf(tmpString,"%f", temperature);
                    report(tmpString, slot_num);
                }

                if(threshold!=0){
                    if(temperature>threshold){
                        state=!inverse;
                    }else{
                        state=inverse;
                    }
                    if(state!=state_prev){
                        state_prev=state;
                        memset(tmpString, 0, strlen(tmpString));
                        sprintf(tmpString,"/state:%i", state);
                        report(tmpString, slot_num);
                    }
                }
            }
        }

        //ESP_LOGD(TAG, "fuction work time: %ld", xTaskGetTickCount() - xLastWakeTime);
        if(periodic!=0){
			vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(periodic));
            //vTaskDelay(pdMS_TO_TICKS(periodic));
		}else{
			vTaskDelay(pdMS_TO_TICKS(50));
		}
    }

}

void start_ds18b20_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "ds18b20_task_%d", slot_num);
	xTaskCreatePinnedToCore(ds18b20_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "ds18b20_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

