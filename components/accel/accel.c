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
static const char *TAG = "SWIPER";

void MPU9250_kick_task(void *arg) {
    #define I2C_MASTER_TIMEOUT_MS 1000
    #define MPU9250_ADDR 0x68
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
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
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    int i2c_num = 0;
    while(i2c_num < I2C_NUM_MAX) {
        esp_err_t ret = i2c_driver_install(i2c_num, I2C_MODE_MASTER, 0, 0, 0);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C_%d initialized for slot:%d", i2c_num, slot_num);
            i2c_param_config(i2c_num, &conf);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2C_%d defined, try next", i2c_num);
            i2c_num++;
        }
    }
    if(i2c_num == I2C_NUM_MAX){
        ESP_LOGE(TAG, "No free I2C driver");
        vTaskDelete(NULL);
    }

    uint8_t data;

    i2c_master_write_read_device(i2c_num, MPU9250_ADDR, &(uint8_t){0x75}, 1, &data, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (data != 0x71) {
        ESP_LOGE(TAG, "MPU-9250 не обнаружен");
        vTaskDelete(NULL);
    }

    // Сброс устройства
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x6B, 0x80}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Включение акселерометра
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x6B, 0x00}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    // Настройка диапазона акселерометра (±2g)
    i2c_master_write_to_device(i2c_num, MPU9250_ADDR, &(uint8_t[]){0x1C, 0x18}, 2, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    

    while(1){
        int16_t accel_raw[3];
        float accel[3];
        float magnitude;

        // Чтение акселерометра
        uint8_t reg = 0x3B;
        i2c_master_write_read_device(i2c_num, MPU9250_ADDR, &reg, 1, (uint8_t*)accel_raw, 6, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
        // Преобразование сырых данных в значения g
        for (int i = 0; i < 3; i++) {
            accel[i] = (float)accel_raw[i] / 2048.0; // 2048 LSB/g для диапазона ±16g
        }
        // Вычисление величины ускорения
        magnitude = sqrt(accel[0]*accel[0] + accel[1]*accel[1] + accel[2]*accel[2]);
        // Проверка на удар
        if (magnitude > 3) {
            ESP_LOGI(TAG, "Обнаружен удар! Сила: %.2f g", magnitude);
            // Здесь можно добавить код для обработки события удара
        }
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
