#include <stdio.h>
#include "VESC.h"
#include "sdkconfig.h"
#include <stdint.h>
#include "math.h"
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "reporter.h"
#include "stateConfig.h"
#include "executor.h"

#include "driver/twai.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "VESC";

typedef enum {
	CAN_PACKET_SET_DUTY = 0,
	CAN_PACKET_SET_CURRENT,
	CAN_PACKET_SET_CURRENT_BRAKE,
	CAN_PACKET_SET_RPM,
	CAN_PACKET_SET_POS,
	CAN_PACKET_SET_CURRENT_REL = 10,
	CAN_PACKET_SET_CURRENT_BRAKE_REL,
	CAN_PACKET_SET_CURRENT_HANDBRAKE,
	CAN_PACKET_SET_CURRENT_HANDBRAKE_REL,
	CAN_PACKET_MAKE_ENUM_32_BITS = 0xFFFFFFFF,
} CAN_PACKET_ID;


void buffer_append_int16(uint8_t* buffer, int16_t number, int32_t *index) {
	buffer[(*index)++] = number >> 8;
	buffer[(*index)++] = number;
}

void buffer_append_int32(uint8_t* buffer, int32_t number, int32_t *index) {
	buffer[(*index)++] = number >> 24;
	buffer[(*index)++] = number >> 16;
	buffer[(*index)++] = number >> 8;
	buffer[(*index)++] = number;
}

void buffer_append_float16(uint8_t* buffer, float number, float scale, int32_t *index) {
    buffer_append_int16(buffer, (int16_t)(number * scale), index);
}

void buffer_append_float32(uint8_t* buffer, float number, float scale, int32_t *index) {
    buffer_append_int32(buffer, (int32_t)(number * scale), index);
}


//void _send_can_package(uint8_t can_id, uint8_t cmd_id, uint16_t option, uint8_t len, uint8_t* data)
void can_transmit_eid(uint32_t id, const uint8_t *data, uint8_t len) {
       
    twai_message_t message;
    memset(&message, 0, sizeof(message));
    message.extd = 1; //enable extended frame format
    message.identifier = id;

    message.data_length_code = len;
    for (int i = 0; i < len; i++) {
        message.data[i] = data[i];
    }

    ESP_LOGD(TAG, "TX->id:%.2x %.2x %.2x %.2x data:%.2x %.2x %.2x %.2x", (uint8_t)(message.identifier >> 24), (uint8_t)(message.identifier >> 16), (uint8_t)(message.identifier >> 8), (uint8_t)(message.identifier), message.data[0], message.data[1], message.data[2], message.data[3]);
    

    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit CAN message");
    }
}


void comm_can_set_duty(uint8_t controller_id, float duty) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)(duty * 100000.0), &send_index);
	can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_DUTY << 8), buffer, send_index);
}

void CAN_VESC_task(void *arg) {
    int slot_num = *(int*) arg;

    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][1];

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver");
    }

    if (twai_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver");
    } 

    uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_TX_IDLE | TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR;
    if (twai_reconfigure_alerts(alerts_to_enable, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "CAN Alerts not reconfigured");
    }

    vTaskDelay(100);
    comm_can_set_duty(1,0.2);
    vTaskDelay(1000);
    comm_can_set_duty(1,0.0);

    ESP_LOGD(TAG, "PUK");
    while(1){
        vTaskDelay(1000);
    }
}


void start_CAN_VESC_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_CAN_VESC_%d", slot_num);
	xTaskCreatePinnedToCore(CAN_VESC_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);
	ESP_LOGD(TAG,"CAN_VESC_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}