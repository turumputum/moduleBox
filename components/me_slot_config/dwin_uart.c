

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "reporter.h"
#include "stateConfig.h"
#include "executor.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DWIN_UART";

void dwinUart_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    // if (slot_num > 1) {
    //     ESP_LOGD(TAG, "Wrong slot!!!");
    //     vTaskDelete(NULL);
    // }

    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

#define BUF_SIZE 256
    uint8_t data[BUF_SIZE];
    size_t len;

    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGD(TAG, "UART initialized for slot: %d uart_num:%d rx_pin:%d tx_pin:%d", slot_num, uart_num, rx_pin, tx_pin);

    if (strstr(me_config.slot_options[slot_num], "dwin_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "dwin_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/dwinUart_0")+3];
		sprintf(t_str, "%s/dwinUart_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    int page = 0;
    uint8_t *rawByte = (uint8_t *) malloc(10);

    while(1){
        //uart_read_bytes(uart_num, rawByte, 1, 5 / portTICK_RATE_MS);

        command_message_t msg;
		if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
			char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
            if(strstr(cmd, "setPage")!=NULL){
                rawByte[0]=0x5a;
                rawByte[1]=0xa5;
                rawByte[2]=0x07;
                rawByte[3]=0x82;
                rawByte[4]=0x00;
                rawByte[5]=0x84;
                rawByte[6]=0x5a;
                rawByte[7]=0x01;
                rawByte[8]=0x00;
                rawByte[9]= atoi(payload);
                uart_write_bytes(uart_num, (const char *)rawByte, 10);
                vTaskDelay(1000 / portTICK_RATE_MS);
            }
        }

    }

}

void start_dwinUart_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(dwinUart_task, "dwinUart_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "dwinUart_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

void testUart_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    // if (slot_num > 1) {
    //     ESP_LOGD(TAG, "Wrong slot!!!");
    //     vTaskDelete(NULL);
    // }

    uint8_t uart_num = UART_NUM_2;
    if (slot_num == 0) {
        uart_num = UART_NUM_2;
    }
    else if (slot_num == 2) {
        uart_num = UART_NUM_1;
    }
#define BUF_SIZE 256
    uint8_t data[BUF_SIZE];
    size_t len;

    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGD(TAG, "UART initialized for slot: %d uart_num:%d rx_pin:%d tx_pin:%d", slot_num, uart_num, rx_pin, tx_pin);

    if (strstr(me_config.slot_options[slot_num], "testUart_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "testUart_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/testUart_0")+3];
		sprintf(t_str, "%s/testUart_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    // int page = 0;
    uint8_t *rawByte = (uint8_t *) malloc(100);
    // rawByte[0]=0x5a;
    // rawByte[1]=0xa5;
    // rawByte[2]=0x07;
    // rawByte[3]=0x82;
    // rawByte[4]=0x00;
    // rawByte[5]=0x84;
    // rawByte[6]=0x5a;
    // rawByte[7]=0x01;
    // rawByte[8]=0x00;

    while(1){
        uart_read_bytes(uart_num, rawByte, 1, 5 / portTICK_RATE_MS);
        ESP_LOGD(TAG, "Read byte:%s", rawByte);
        vTaskDelay(300 / portTICK_RATE_MS);
        //int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        // command_message_t msg;
		// if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
		// 	char* payload;
        //     char* cmd = strtok_r(msg.str, ":", &payload);
        //     ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
        //     cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
        //     if(strstr(cmd, "setPage")!=NULL){
        //         rawByte[9]= atoi(payload);;
        //         uart_write_bytes(uart_num, (const char *)rawByte, 10);
        //         page = !page;
        //         vTaskDelay(1000 / portTICK_RATE_MS);
        //     }
        // }

    }

}

void start_testUart_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(testUart_task, "dwinUart_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "dwinUart_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}