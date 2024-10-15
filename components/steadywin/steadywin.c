#include <stdio.h>
#include "steadywin.h"

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

#include "driver/twai.h"

//uint8_t CYBERGEAR_CAN_ID = 0x7F;
//uint8_t MASTER_CAN_ID = 0x00;

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEADYWIN";

typedef struct{
    float pos;
    float vel;
    float kP;
    float kD;
    float T;
}motorState_t;

union FloatBytes {
    float f;
    uint8_t bytes[4];
};


uint16_t float_to_uint(float v,float v_min,float v_max,uint32_t width){
    float temp;
    int32_t utemp;
    temp = ((v-v_min)/(v_max-v_min))*((float)width);
    utemp = (int32_t)temp;
    if(utemp < 0)
    utemp = 0;
    if(utemp > width)
    utemp = width;
    return utemp;
}

float uint_to_float(uint16_t x, float x_min, float x_max){
    uint16_t type_max = 0xFFFF;
    float span = x_max - x_min;
    return (float) x / type_max * span + x_min;
}

void sendTWAI(motorState_t targetState){
    twai_message_t _message;
    _message.identifier = 1;
    _message.data_length_code = 8;
    uint16_t s_p_int = float_to_uint(targetState.pos, -6.25, 6.25, 65535);
    uint16_t s_v_int = float_to_uint(targetState.vel, -65, 65, 4096);
    uint16_t s_Kp_int = float_to_uint(targetState.kP, 0 , 500, 4096);
    uint16_t s_Kd_int = float_to_uint(targetState.kD, 0 , 5, 4096);
    uint16_t s_c_int = float_to_uint(targetState.T, -4, 4, 4096);
    _message.data[0] = s_p_int>>8;
    _message.data[1] = s_p_int&0xFF;
    _message.data[2] = s_v_int>>4;;
    _message.data[3] = ((s_v_int&0xF)<<4) + (s_Kp_int >>8);
    _message.data[4] = s_Kp_int &0xFF;
    _message.data[5] = s_Kd_int>>4;
    _message.data[6] = ((s_Kd_int &0xF)<<4) + (s_c_int >>8);
    _message.data[7] = s_c_int&0xFF;;

    if (twai_transmit(&_message, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit CAN message");
    }
}
float reciveTWAI(){    
    twai_message_t rx_msg;
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        //ESP_LOGD(TAG, "Received CAN message:%ld %x %x %x %x %x %x %x %x", rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
        if(rx_msg.identifier == 100){
            //ESP_LOGD(TAG,"hui");
            //vTaskDelay(1/portTICK_PERIOD_MS);
            //int32_t p_int = (rx_msg.data[1]<<8) + rx_msg.data[2];
            //float tPos = (((float)((rx_msg.data[1]<<8) + rx_msg.data[2]) - 32768)/32768)*6.25;
            return (((float)((rx_msg.data[1]<<8) + rx_msg.data[2]) - 32768)/32768)*6.25;
        }
    }
    return 0;
}

void steadywin_task(void *arg) {
    int slot_num = *(int*) arg;

    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][1];

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    //twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
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
    twai_message_t message;
    message.identifier = 1;
    message.data_length_code = 8;
    twai_message_t rx_msg;

    message.data[0] = 0x81;
    message.data[1] = 0x00;
    message.data[2] = 0x00;
    message.data[3] = 0x00;
    message.data[4] = 0x00;
    message.data[5] = 0x00;
    message.data[6] = 0x00;
    message.data[7] = 0x00; 
    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit CAN message");
    }
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "Received CAN message after reset:%x %x %x %x %x %x %x %x", rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
    }

    message.data[0] = 0x91;
    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit CAN message");
    }
     if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "Received CAN message after start::%x %x %x %x %x %x %x %x", rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
    }

    
    // message.data[7] = 0xFB; 
    // if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to transmit CAN message");
    // }

    // message.data[7] = 0xFC;
    // if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to transmit CAN message");
    // }

    uint8_t numOfPos=12;
    uint8_t currentPos=0;
    float halfZone_rad= M_PI*2/numOfPos;
    ESP_LOGD(TAG, "halfZone_rad:%f", halfZone_rad);

    motorState_t targetState;
    targetState.pos = 0;
    targetState.vel = 30;
    targetState.kP=100;
    targetState.kD=0.5;
    targetState.T=0.4;
    motorState_t currentState;
    currentState.pos = 0;

    // vTaskDelay(pdMS_TO_TICKS(100));
    // sendTWAI(targetState);
    // currentState.pos = reciveTWAI();
    // vTaskDelay(pdMS_TO_TICKS(1000));
    while(1){
        message.data[0] = 0x93;
        message.data[1] = 0x00;
        message.data[2] = 0x00;
        message.data[3] = 0x00;
        message.data[4] = 0x00;
        message.data[5] = 0x00;
        message.data[6] = 0x00;
        message.data[7] = 0x00;

        union FloatBytes posFloat, durFloat;
    
        message.data[0] = 0x95;
        
        posFloat.f = 1.0;
        for (int i = 1; i < 5; i++) {
            message.data[i] = posFloat.bytes[i];
        }
        
        durFloat.f = 1000.0;
        for (int i = 5; i < 9; i++) {
            message.data[i] = durFloat.bytes[i];
        }

        if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to transmit CAN message");
        }
        twai_message_t rx_msg;
        if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
            ESP_LOGD(TAG, "Received CAN message:%ld %x %x %x %x %x %x %x %x", rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
        }


        // sendTWAI(targetState); 
        vTaskDelay(pdMS_TO_TICKS(100));
        // currentState.pos = reciveTWAI();
        
        // // targetState.pos = -targetState.pos;
        // ESP_LOGD(TAG, "pos delta:%f currentPos:%f targetPos:%f", fabs(targetState.pos - currentState.pos), currentState.pos, targetState.pos);
        
        // if(fabs(targetState.pos - currentState.pos)>=halfZone_rad){
        //     if(targetState.pos < currentState.pos){
        //         currentPos = (currentPos+1)%numOfPos;
        //     }else{
        //         currentPos = (currentPos-1+numOfPos)%numOfPos;
        //     }
        //     targetState.pos = currentPos*halfZone_rad*2;
        //     ESP_LOGD(TAG, "curPosNum:%d targetPos:%f",currentPos, targetState.pos);
        // }

        
    }

}


void start_steadywin_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "steadywin_task_%d", slot_num);
	xTaskCreatePinnedToCore(steadywin_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);

	ESP_LOGD(TAG,"steadywin_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}