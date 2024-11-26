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
        ESP_LOGD(TAG, "Received CAN message:%ld %x %x %x %x %x %x %x %x", rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
        if(rx_msg.identifier == 0){
            //ESP_LOGD(TAG,"hui");
            //vTaskDelay(1/portTICK_PERIOD_MS);
            //int32_t p_int = (rx_msg.data[1]<<8) + rx_msg.data[2];
            //float tPos = (((float)((rx_msg.data[1]<<8) + rx_msg.data[2]) - 32768)/32768)*6.25;
            return (((float)((rx_msg.data[1]<<8) + rx_msg.data[2]) - 32768)/32768)*6.25;
        }
    }
    return 0;
}

void setTorque(float torque){
    twai_message_t _message;
    _message.identifier = 1;
    _message.data_length_code = 8;
    _message.extd = 0;              // Standard Format message (11-bit ID)
    _message.rtr = 0;               // Send a data frame
    _message.ss = 0;                // Not single shot
    _message.self = 0;              // Not a self reception request
    _message.dlc_non_comp = 0;      // DLC is less than 8

    _message.data[0] = 0x93;
    uint32_t int_value = *((int32_t*)(&torque));

    _message.data[1] = (uint8_t)(int_value & 0xFF);
    _message.data[2] = (uint8_t)((int_value >> 8) & 0xFF);
    _message.data[3] = (uint8_t)((int_value >> 16) & 0xFF);
    _message.data[4] = (uint8_t)((int_value >> 24) & 0xFF);

    int_value = 1;
    _message.data[5] = (uint8_t)(int_value & 0xFF);
    _message.data[6] = (uint8_t)((int_value >> 8) & 0xFF);
    _message.data[7] = (uint8_t)((int_value >> 16) & 0xFF);

    //ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x %x %x %x",_message.identifier, _message.data[0], _message.data[1], _message.data[2], _message.data[3], _message.data[4], _message.data[5], _message.data[6], _message.data[7]);

    if (twai_transmit(&_message, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGE(TAG, "Transmit CAN Error");
    }
}

void reciveCAN(float* pos){
    twai_message_t _rx_msg;
    memset(&_rx_msg, 0, sizeof(_rx_msg));
    if (twai_receive(&_rx_msg, pdMS_TO_TICKS(10)) == ESP_OK) {
        //ESP_LOGD(TAG, "<-RX id:%ld message:%x %x %x %x %x %x %x %x",_rx_msg.identifier, _rx_msg.data[0], _rx_msg.data[1], _rx_msg.data[2], _rx_msg.data[3], _rx_msg.data[4], _rx_msg.data[5], _rx_msg.data[6], _rx_msg.data[7]);
        if(_rx_msg.data[0] == 0x93){
            uint16_t int_value = (_rx_msg.data[3] & 0xFF) | ((_rx_msg.data[4] & 0xFF) << 8);
            uint16_t torque_int = ((_rx_msg.data[6] & 0x0F) << 8) | _rx_msg.data[7];
            float torque_float = torque_int * (450 * 0.41 * 8) / 4095.0 - 225 * 0.41 * 8;
            //ESP_LOGD(TAG, "torque:%f", torque_float);
            
            *pos = ((float)int_value*25/65535)-12.5;
            //ESP_LOGD(TAG, "pos:%d", int_value);
            //ESP_LOGD(TAG, "pos:%f", pos);
        }  
    }
    //return 0;
}

void dd_get_pos(float* pos){
    twai_message_t _message;
    _message.identifier = 1;
    _message.data_length_code = 1;
    _message.extd = 0;              // Standard Format message (11-bit ID)
    _message.rtr = 0;               // Send a data frame
    _message.ss = 0;                // Not single shot
    _message.self = 0;              // Not a self reception request
    _message.dlc_non_comp = 0;      // DLC is less than 8
    _message.data[0] = 0xA3;


    if (twai_transmit(&_message, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGE(TAG, "Transmit CAN Error");
    }

    twai_message_t _rx_msg;
    memset(&_rx_msg, 0, sizeof(_rx_msg));
    if (twai_receive(&_rx_msg, pdMS_TO_TICKS(100)) == ESP_OK) {
        if(_rx_msg.data[0] == 0xA3){
            uint16_t int_value = (_rx_msg.data[1] & 0xFF) | ((_rx_msg.data[2] & 0xFF) << 8);
            float angle = int_value * (2*M_PI /16384);
            //ESP_LOGD(TAG, "angle:%f", angle);

            *pos = angle;
            //ESP_LOGD(TAG, "pos:%d", int_value);
            //ESP_LOGD(TAG, "pos:%f", pos);
        }
    }
}


void dd_set_torque(float torque){
    twai_message_t _message;
    _message.identifier = 1;
    _message.data_length_code = 5;
    _message.extd = 0;              // Standard Format message (11-bit ID)
    _message.rtr = 0;               // Send a data frame
    _message.ss = 0;                // Not single shot
    _message.self = 0;              // Not a self reception request
    _message.dlc_non_comp = 0;      // DLC is less than 8
    _message.data[0] = 0xC0;
    //int32_t int_value = *((int32_t*)(&torque));
    int32_t int_value = (int32_t)torque;
    _message.data[1] = (uint8_t)(int_value & 0xFF);
    _message.data[2] = (uint8_t)((int_value >> 8) & 0xFF);
    _message.data[3] = (uint8_t)((int_value >> 16) & 0xFF);
    _message.data[4] = (uint8_t)((int_value >> 24) & 0xFF);

    //ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x %x %x %x",_message.identifier, _message.data[0], _message.data[1], _message.data[2], _message.data[3], _message.data[4], _message.data[5], _message.data[6], _message.data[7]);

    if (twai_transmit(&_message, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGE(TAG, "Transmit CAN Error");
    }
}

void dd_set_pos(float pos){
    twai_message_t _message;
    _message.identifier = 1;
    _message.data_length_code = 5;
    _message.extd = 0;              // Standard Format message (11-bit ID)
    _message.rtr = 0;               // Send a data frame
    _message.ss = 0;                // Not single shot
    _message.self = 0;              // Not a self reception request
    _message.dlc_non_comp = 0;      // DLC is less than 8
    _message.data[0] = 0xC2;
    //int32_t int_value = *((int32_t*)(&torque));
    int32_t int_value = (int32_t)(pos*(16384/(2*M_PI)));
    _message.data[1] = (uint8_t)(int_value & 0xFF);
    _message.data[2] = (uint8_t)((int_value >> 8) & 0xFF);
    _message.data[3] = (uint8_t)((int_value >> 16) & 0xFF);
    _message.data[4] = (uint8_t)((int_value >> 24) & 0xFF);
    ESP_LOGD(TAG, "set  pos:%ld", int_value);
    //ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x %x %x %x",_message.identifier, _message.data[0], _message.data[1], _message.data[2], _message.data[3], _message.data[4], _message.data[5], _message.data[6], _message.data[7]);

    if (twai_transmit(&_message, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGE(TAG, "Transmit CAN Error");
    }
}

void steadywin_task(void *arg) {

    //vTaskDelay(pdMS_TO_TICKS(500));

    int slot_num = *(int*) arg;

    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][1];

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    //twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
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

    float maxTorque=14;
    float torqueConst = 0.08;
    float torque = 0.3;

    twai_message_t message;
    message.identifier = 1;
    message.data_length_code = 5;
    message.extd = 0;              // Standard Format message (11-bit ID)
    message.rtr = 0;               // Send a data frame
    message.ss = 0;                // Not single shot
    message.self = 0;              // Not a self reception request
    message.dlc_non_comp = 0;      // DLC is less than 8
    twai_message_t rx_msg;
    memset(&rx_msg, 0, sizeof(rx_msg));

    ESP_LOGD(TAG, "Send maxTorque");
    message.data[0] = 0xB3; 
    int32_t i2 = (int32_t)(maxTorque/torqueConst);
    message.data[1] = (uint8_t)(i2 & 0xFF);
    message.data[2] = (uint8_t)((i2 >> 8) & 0xFF);
    message.data[3] = (uint8_t)((i2>> 16) & 0xFF);
    message.data[4] = (uint8_t)((i2 >> 24) & 0xFF);

    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x",message.identifier, message.data[0], message.data[1], message.data[2], message.data[3], message.data[4]);
    }
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "<-RX id:%ld message:%x %x %x %x %x %x %x %x",rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGD(TAG, "Send torque Kp");
    message.data[0] = 0xB4; 
    int32_t i1 = (int32_t)(100000);
    message.data[1] = (uint8_t)(i1 & 0xFF);
    message.data[2] = (uint8_t)((i1 >> 8) & 0xFF);
    message.data[3] = (uint8_t)((i1>> 16) & 0xFF);
    message.data[4] = (uint8_t)((i1 >> 24) & 0xFF);
    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x",message.identifier, message.data[0], message.data[1], message.data[2], message.data[3], message.data[4]);
    }
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "<-RX id:%ld message:%x %x %x %x %x %x %x %x",rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
    } 

    ESP_LOGD(TAG, "Send MaxSpeed");
    message.data[0] = 0xB2; 
    int32_t i3 = (int32_t)(100);
    message.data[1] = (uint8_t)(i3 & 0xFF);
    message.data[2] = (uint8_t)((i3 >> 8) & 0xFF);
    message.data[3] = (uint8_t)((i3>> 16) & 0xFF);
    message.data[4] = (uint8_t)((i3 >> 24) & 0xFF);
    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "TX-> id:%ld message:%x %x %x %x %x",message.identifier, message.data[0], message.data[1], message.data[2], message.data[3], message.data[4]);
    }
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
        ESP_LOGD(TAG, "<-RX id:%ld message:%x %x %x %x %x %x %x %x",rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
    } 


    uint8_t numOfZone=10;
    uint8_t currentZone=0;
    float halfZone_rad= M_PI/numOfZone;
    ESP_LOGD(TAG, "halfZone_rad:%f", halfZone_rad);

    float prevPos=0.0;
    float curPos=0.0;
    float cPos=0.0;

    float targetPos = halfZone_rad+halfZone_rad*currentZone;

    float deadZone = halfZone_rad*0.2;

    vTaskDelay(pdMS_TO_TICKS(100));

    printf("torque,pos,centrZone,deltaPos,deltaSpd\r\n");
    
    while(1){  
        //reciveCAN(&cPos);
        dd_get_pos(&cPos);
        float delta = prevPos - cPos;
        prevPos = cPos;
        curPos = curPos + delta;
        if(curPos<0){
            curPos = curPos + 2*M_PI;
        }else if(curPos>2*M_PI){
            curPos = curPos - 2*M_PI;
        }

       
        currentZone = (uint8_t)(curPos/(halfZone_rad*2));
        if(currentZone==numOfZone){
            currentZone = 0;
        }
        //ESP_LOGD(TAG, "Zone:%d ZoneCenter:%f curPos:%f", currentZone, currentZone*halfZone_rad*2+halfZone_rad, curPos);
        float centrDelt=(currentZone*halfZone_rad*2+halfZone_rad)-curPos;
        //float pos = (currentZone*halfZone_rad*2+halfZone_rad);
        //float pos = (currentZone*halfZone_rad*2);
        //ESP_LOGD(TAG, "pos:%f", pos);
        //dd_set_pos(pos);

        //ESP_LOGD(TAG, "delta:%f deltaPercent:%f",centrDelt, fabs(centrDelt/halfZone_rad));
        float tarTorque = -((centrDelt/halfZone_rad)*maxTorque)/torqueConst;
        // if(fabs(tarTorque)>fabs(torque)){
        //     torque = torque+5;
        // }else{
        //     torque = tarTorque;
        // }
        // ESP_LOGD(TAG, "torque:%f tarTorque:%f", torque, tarTorque);
        if(deadZone>fabs(centrDelt)){
            torque = 0;
        }
        //printf("%f,%f,%f,%f,%f\r\n", tarTorque, curPos,(currentZone*halfZone_rad*2+halfZone_rad), centrDelt, delta);
        //ESP_LOGD(TAG, "torque:%f zone:%d pos:%f", torque, currentZone, curPos);
        dd_set_torque(tarTorque);
        //setTorque(torque);
        vTaskDelay(pdMS_TO_TICKS(8));
        //float torque = deltaPos/halfZone_rad;
        //sendTWAI(targetState); 
        
        //currentState.pos = reciveTWAI();
        
        //targetState.pos = -targetState.pos;
        //ESP_LOGD(TAG, "pos delta:%f currentPos:%f targetPos:%f", fabs(targetState.pos - currentState.pos), currentState.pos, targetState.pos);
        
        // if(fabs(tPos - cPos)>=halfZone_rad){
        //     if(tPos < cPos){
        //         currentPos = (currentPos+1)%numOfPos;
        //     }else{
        //         currentPos = (currentPos-1+numOfPos)%numOfPos;
        //     }
        //     tPos = currentPos*halfZone_rad*2;
        //     ESP_LOGD(TAG, "curPosNum:%d targetPos:%f",currentPos, tPos);
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