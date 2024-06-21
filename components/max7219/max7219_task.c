#include "max7219_task.h"
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

#include <max7219.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "MAX7219";

void max7219_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();

    me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));
	//---init hardware---
	uint8_t data_pin = SLOTS_PIN_MAP[slot_num][2];
    uint8_t clk_pin = SLOTS_PIN_MAP[slot_num][1];
    uint8_t cs_pin = SLOTS_PIN_MAP[slot_num][0];

    // Configure SPI bus
    #define HOST SPI3_HOST
    spi_bus_config_t cfg = {
       .mosi_io_num = data_pin,
       .miso_io_num = -1,
       .sclk_io_num = clk_pin,
       .quadwp_io_num = -1,
       .quadhd_io_num = -1,
       .max_transfer_sz = 0,
       .flags = 0
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HOST, &cfg, SPI_DMA_CH_AUTO));

    // Configure device
    max7219_t dev = {
       .cascade_size = 8,
       .digits = 64,
       .mirrored = true
    };
    ESP_ERROR_CHECK(max7219_init_desc(&dev, HOST, 10000, cs_pin));
    ESP_ERROR_CHECK(max7219_init(&dev));
    max7219_set_brightness(&dev, MAX7219_MAX_BRIGHTNESS);
    //max7219_set_decode_mode(&dev, true);

    char t_str[strlen(me_config.deviceName)+strlen("/disp_0")+3];
    sprintf(t_str, "%s/disp_%d",me_config.deviceName, slot_num);
    me_state.action_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);

    size_t offs = 0;

    uint32_t count = 0;
    while(1){
        count++;
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            char str[8];
            strncpy(&str, payload, 8);
            str[8]='\0';
            max7219_draw_text_7seg(&dev, offs, str);
        }
        

        // char tmpString[60];
        // sprintf(tmpString, "%8ld", count);
        // ESP_LOGD(TAG, "count: %8ld", count);
        // ESP_ERROR_CHECK(max7219_draw_text_7seg(&dev, offs, tmpString));
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}


void start_max7219_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_max7219_%d", slot_num);
	xTaskCreatePinnedToCore(max7219_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"max7219 task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}