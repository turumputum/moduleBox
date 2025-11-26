#include <stdio.h>
#include "accel.h"

#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"
#include "math.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "driver/i2c.h"



extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ACCEL";

void MPU9250_kick_task(void *arg) {
    #define I2C_MASTER_TIMEOUT_MS 100
    #define MPU9250_ADDR 0x68
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic", "/kick_0");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/kick_0")+3];
		sprintf(t_str, "%s/kick_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = 100000,
    };
    int i2c_num = me_state.free_i2c_num;
    me_state.free_i2c_num++;
    if(i2c_num == I2C_NUM_MAX){
        ESP_LOGE(TAG, "No free I2C driver");
        vTaskDelete(NULL);
    }

    i2c_param_config(i2c_num, &conf);
    esp_err_t ret = i2c_driver_install(i2c_num, conf.mode,  0, 0, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C_%d initialized for slot:%d", i2c_num, slot_num);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C_%d defined, try next", i2c_num);
        i2c_num++;
    }
    
    uint8_t data;
    // for(int addr = 0x0; addr <= 0xFF; addr++) {
    //     ESP_LOGD(TAG, "addr: %d", addr);
    //     i2c_master_write_read_device(i2c_num, addr, &(uint8_t){0x75}, 1, &data, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    //     if (data == 0x71) {
    //         ESP_LOGI(TAG, "MPU-9250 found on address 0x%02X", addr);
    //         break;
    //     }
    // }
    //ESP_LOGI(TAG, "search done");
    i2c_master_write_read_device(i2c_num, MPU9250_ADDR, &(uint8_t){0x75}, 1, &data, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (data != 0x71) {
        ESP_LOGE(TAG, "MPU-9250 не обнаружен");
        //vTaskDelete(NULL);
    }
    //vTaskDelete(NULL);
    // i2c_device_config_t i2c_dev_conf = {
    //     .scl_speed_hz = 100000,
    //     .device_address = MPU9250_ADDR,
    // };



    // Сброс устройства
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x6B, 0x80}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Включение акселерометра
    //i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x6B, 0x00}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    // Настройка диапазона акселерометра (±16g)
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x1C, 0x18}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    //Turn on the internal low-pass filter for accelerometer with 10.2Hz bandwidth
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x1D, 0x05}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    //turn on the bypass multiplexer
    //i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x37, 0x02}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    

    while(1){
        //int16_t accel_raw[3];
        float accel[3];
        float magnitude;

        // Чтение акселерометра
        uint8_t reg = 0x3B;
        uint8_t data[6];
        i2c_master_write_read_device(i2c_num, MPU9250_ADDR, &reg, 1, (uint8_t*)data, 6, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
        // Преобразование сырых данных в значения g
        
        for (int i = 0; i < 3; i++) {
            accel[i] = (float)((data[i*2]<<8 | data[i*2+1])/ 2048.0); // 2048 LSB/g для диапазона ±16g
        }

        //ESP_LOGD(TAG, "Acceleration  X:%.2dg, Y:%.2dg, Z:%.2d g", accel[0], accel[1], accel[2]);
        // Вычисление величины ускорения
        magnitude = sqrt(accel[0]*accel[0] + accel[1]*accel[1]);
        // Проверка на удар
        // if (magnitude > 3) {
        //     ESP_LOGI(TAG, "Обнаружен удар! Сила: %.2f g", magnitude);
        //     // Здесь можно добавить код для обработки события удара
        // }
        vTaskDelay(30 / portTICK_PERIOD_MS);
    }
}

void start_MPU9250_kick_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_MPU9250_kick_%d", slot_num);
	xTaskCreatePinnedToCore(MPU9250_kick_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);

	ESP_LOGD(TAG,"MPU9250_kick_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
