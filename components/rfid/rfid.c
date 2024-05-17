#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include <driver/uart.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pn532_uart.h"
#include "rfid.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

//#define CONFIG_PN532DEBUG 1
// #define CONFIG_MIFAREDEBUG 1
extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "RFID";

void pn532Uart_task(void* arg) {
    int slot_num = *(int*)arg;

    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            goto EXIT;
        }
    }

    #define BUF_SIZE 256
    uint8_t data[BUF_SIZE];
    size_t len;

    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

    if (strstr(me_config.slot_options[slot_num], "rfid_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "rfid_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "rfid_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/rfid_0")+3];
		sprintf(t_str, "%s/rfid_%d",me_config.device_name, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart rfid_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    pn532_t *p = pn532_init(uart_num, tx_pin, rx_pin, 0);

    uint8_t buf[300];
    int l;
    char cardID[3*10+3];
    memset(cardID, 0, sizeof(cardID));
    char _cardID[3*10+3];
    memset(_cardID, 0, sizeof(cardID));

    while (1){
        buf[0] = 1;               
        buf[1] = 0;               /* 106 kbps type A(ISO / IEC14443 Type A) */
        l = pn532_tx(p, 0x4A, 2, buf, 0, NULL);
        l = pn532_rx(p, 0, NULL, sizeof(buf), buf,200);
        if (l > 0){
            memset(cardID, 0, sizeof(cardID));
            if(l==10){
                sprintf(cardID, "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9]);
                //ESP_LOGD(TAG, "Card: %s", cardID);
            }else{
                sprintf(cardID, "NONE");
            }

            if(memcmp(cardID, _cardID, strlen(cardID))!= 0){
                ESP_LOGD(TAG, "slot_num:%d cardID:%s",slot_num, cardID);
                strcpy(_cardID, cardID);
                report(cardID, slot_num);
            }
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    EXIT:
    vTaskDelete(NULL);
    
}


void start_pn532Uart_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(pn532Uart_task, "pn532Uart_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-12, NULL);
    ESP_LOGD(TAG, "pn532Uart_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}