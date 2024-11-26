#include <stdio.h>
#include "cybergear.h"

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

uint8_t CYBERGEAR_CAN_ID = 127;
uint8_t MASTER_CAN_ID = 0x00;

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "CYBERGEAR";


uint16_t _float_to_uint(float x, float x_min, float x_max, int bits){
    if (bits>16) bits=16;
    float span = x_max - x_min;
    float offset = x_min;
    if(x > x_max) x = x_max;
    else if(x < x_min) x = x_min;
    uint16_t res = (int) ((x-offset)*((float)((1<<bits)-1))/span);
    //ESP_LOGD(TAG, "float:%f to uint:%d",x,res);
    return res;
}
float _uint_to_float(uint16_t x, float x_min, float x_max){
    uint16_t type_max = 0xFFFF;
    float span = x_max - x_min;
    return (float) x / type_max * span + x_min;
}


void _send_can_package(uint8_t can_id, uint8_t cmd_id, uint16_t option, uint8_t len, uint8_t* data){
    
    //ESP_LOGD(TAG,"cmd:%d, option:%d, id:%d", cmd_id, option, can_id);
    uint32_t id = cmd_id << 24 | option << 8 | can_id;
    
    twai_message_t message;
    memset(&message, 0, sizeof(message));
    message.extd = 1; //enable extended frame format
    message.identifier = id;

    message.data_length_code = len;
    for (int i = 0; i < len; i++) {
        message.data[i] = data[i];
    }

    //ESP_LOGD(TAG, "TX->id:%.2x %.2x %.2x %.2x data:%.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x", (uint8_t)(message.identifier >> 24), (uint8_t)(message.identifier >> 16), (uint8_t)(message.identifier >> 8), (uint8_t)(message.identifier), message.data[0], message.data[1], message.data[2], message.data[3],  message.data[4], message.data[5], message.data[6], message.data[7]);
    
    if (twai_transmit(&message, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit CAN message");
    }
}

void _send_can_float_package(uint8_t can_id, uint16_t addr, float value, float min, float max){
    uint8_t data[8] = {0x00};
    data[0] = addr & 0x00FF;
    data[1] = addr >> 8;

    float val = (max < value) ? max : value;
    val = (min > value) ? min : value;
    memcpy(&data[4], &val, 4);
    _send_can_package(can_id, CMD_RAM_WRITE, MASTER_CAN_ID, 8, data);
}

void set_run_mode(uint8_t mode){
    uint8_t data[8] = {0x00};
    data[0] = ADDR_RUN_MODE & 0x00FF;
    data[1] = ADDR_RUN_MODE >> 8;
    data[4] = mode;
    _send_can_package(CYBERGEAR_CAN_ID, CMD_RAM_WRITE, MASTER_CAN_ID, 8, data);
}

void enable_motor(){
    uint8_t data[8] = {0x00};
    _send_can_package(CYBERGEAR_CAN_ID, CMD_ENABLE, MASTER_CAN_ID, 8, data);
}

void request_status(void) {
    uint8_t data[8] = {0x00};
    _send_can_package(CYBERGEAR_CAN_ID, CMD_GET_STATUS, MASTER_CAN_ID, 8, data);
}

void send_motion_control(cgCmd_t cmd){
    uint8_t data[8] = {0x00};

    uint16_t position = _float_to_uint(cmd.position, POS_MIN, POS_MAX, 16);
    data[0] = position >> 8;
    data[1] = position & 0x00FF;

    uint16_t speed = _float_to_uint(cmd.speed, V_MIN, V_MAX, 16);
    data[2] = speed >> 8;
    data[3] = speed & 0x00FF;

    uint16_t kp = _float_to_uint(cmd.kp, KP_MIN, KP_MAX, 16);
    data[4] = kp >> 8;
    data[5] = kp & 0x00FF;

    uint16_t kd = _float_to_uint(cmd.kd, KD_MIN, KD_MAX, 16);
    data[6] = kd >> 8;
    data[7] = kd & 0x00FF;

    uint16_t torque = _float_to_uint(cmd.torque, T_MIN, T_MAX, 16);

    _send_can_package(cmd.can_id, CMD_POSITION, torque, 8, data);
}

static void check_alerts(){
  // Check if alert happened
  uint32_t alerts_triggered;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(1000));
  twai_status_info_t twai_status;
  twai_get_status_info(&twai_status);

  // Handle alerts
  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    ESP_LOGD(TAG, "Alert: TWAI controller has become error passive.");
  }
  if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
    ESP_LOGD(TAG, "Alert: A (Bit, Stuff, CRC, Form, ACK) error has occurred on the bus.");
    ESP_LOGD(TAG, "Bus error count: %ld\n", twai_status.bus_error_count);
  }
  if (alerts_triggered & TWAI_ALERT_TX_FAILED) {
    ESP_LOGD(TAG, "Alert: The Transmission failed.");
    ESP_LOGD(TAG, "TX buffered: %ld\t", twai_status.msgs_to_tx);
    ESP_LOGD(TAG, "TX error: %ld\t", twai_status.tx_error_counter);
    ESP_LOGD(TAG, "TX failed: %ld\n", twai_status.tx_failed_count);
  }
  // if (alerts_triggered & TWAI_ALERT_TX_SUCCESS) {
  //   Serial.println("Alert: The Transmission was successful.");
  //   Serial.printf("TX buffered: %d\t", twai_status.msgs_to_tx);
  // }

  // Check if message is received
  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
      //ESP_LOGD(TAG, "skip incoming message");
    }
  }
}

void cybergear_task(void *arg) {
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

    

    uint8_t data[8] = {0x00};
    _send_can_package(CYBERGEAR_CAN_ID, CMD_STOP, MASTER_CAN_ID, 8, data);
    
    set_run_mode(MODE_CURRENT);
    //set_run_mode(MODE_MOTION);

    uint8_t rdata[8] = {0x00};
    rdata[0]=1;
    _send_can_package(CYBERGEAR_CAN_ID, CMD_SET_MECH_POSITION_TO_ZERO, MASTER_CAN_ID, 8, rdata);
    

    float limitTorque=4;
    if (strstr(me_config.slot_options[slot_num], "limitTorque")!=NULL){
        limitTorque = get_option_float_val(slot_num, "limitTorque");
		ESP_LOGD(TAG, "Set limitTorque:%f.  Slot:%d", limitTorque, slot_num);
	}
    //_send_can_float_package(CYBERGEAR_CAN_ID, ADDR_LIMIT_TORQUE, limitTorque, 0.0f, T_MAX);
    

    float cKp=0.1;
    if (strstr(me_config.slot_options[slot_num], "cKp")!=NULL){
        cKp = get_option_float_val(slot_num, "cKp");
		ESP_LOGD(TAG, "Set cKp:%f.  Slot:%d", cKp, slot_num);
	}
    _send_can_float_package(CYBERGEAR_CAN_ID, ADDR_CURRENT_KP, cKp, 0.0f, I_MAX);
    float cKi=0.001;
    if (strstr(me_config.slot_options[slot_num], "cKi")!=NULL){
        cKi = get_option_float_val(slot_num, "cKi");
		ESP_LOGD(TAG, "Set cKi:%f.  Slot:%d", cKi, slot_num);
	}
    _send_can_float_package(CYBERGEAR_CAN_ID, ADDR_CURRENT_KI, cKi, 0.0f, I_MAX);
    //_send_can_float_package(CYBERGEAR_CAN_ID, ADDR_LIMIT_SPEED, 5, 0.0f, V_MAX);
    //_send_can_float_package(CYBERGEAR_CAN_ID, ADDR_SPEED_REF, 5, 0.0f, V_MAX);
    
    float pKp=10;
    if (strstr(me_config.slot_options[slot_num], "pKp")!=NULL){
        pKp = get_option_float_val(slot_num, "pKp");
		ESP_LOGD(TAG, "Set pKp:%f.  Slot:%d", pKp, slot_num);
	}
    float sKp=3;
    if (strstr(me_config.slot_options[slot_num], "sKp")!=NULL){
        sKp = get_option_float_val(slot_num, "sKp");
		ESP_LOGD(TAG, "Set sKp:%f.  Slot:%d", sKp, slot_num);
	}
    float deadBand=0.1;
    if (strstr(me_config.slot_options[slot_num], "deadBand")!=NULL){
        deadBand = get_option_float_val(slot_num, "deadBand");
		ESP_LOGD(TAG, "Set deadBand:%f.  Slot:%d", pKp, slot_num);
	}
    //_send_can_float_package(CYBERGEAR_CAN_ID, ADDR_POSITION_REF, 0, POS_MIN, POS_MAX);
    enable_motor();
    
    float position = 1;
    float speed = 0;
    float torque = 0;
    float temp = 0;


    float deadband = 0.2;
    while (1){
        //request_status();

        cgCmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.can_id = CYBERGEAR_CAN_ID;
        cmd.position = 0;
        cmd.speed = 0;
        cmd.torque = 0;
        cmd.kp = 0;
        cmd.kd = 0;
        send_motion_control(cmd);

        twai_message_t rx_msg;
        memset(&rx_msg, 0, sizeof(rx_msg));
        if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
            //if (((rx_msg.identifier & 0xFF00) >> 8) == CYBERGEAR_CAN_ID){
                uint16_t raw_position = rx_msg.data[1] | rx_msg.data[0] << 8;
                position = _uint_to_float(raw_position, POS_MIN, POS_MAX);
                uint16_t raw_speed = rx_msg.data[3] | rx_msg.data[2] << 8;
                speed = _uint_to_float(raw_speed, V_MIN, V_MAX);
                uint16_t raw_torque = rx_msg.data[5] | rx_msg.data[4] << 8;
                torque = _uint_to_float(raw_torque, T_MIN, T_MAX);
                uint16_t raw_temperature = rx_msg.data[7] | rx_msg.data[6] << 8;
                temp = raw_temperature/10;
                //ESP_LOGD(TAG, "Position: %f, Speed: %f, Torque: %f, Temperature: %f", position, speed, torque, temp);
                //ESP_LOGD(TAG, "Raw Position: %d, Raw Speed: %d, Raw Torque: %d, Raw Temperature: %d", raw_position, raw_speed, raw_torque, raw_temperature);
            //}
        }
        float posAntiiTorque = -(position*pKp);
        //float posAntiiTorque = 0;
        float speedAntiTorque = -(speed*sKp);
        float setTorque = posAntiiTorque + speedAntiTorque;

        if(fabs(setTorque) < deadBand){
            setTorque=0;
        }

        if(setTorque >limitTorque){
            setTorque=limitTorque;
        }else if(setTorque < -limitTorque){
            setTorque=-limitTorque;
        }
        _send_can_float_package(CYBERGEAR_CAN_ID, ADDR_I_REF, setTorque, -I_MAX, I_MAX);

        ESP_LOGD(TAG, "antoTorque:%f posAntiTourque:%f speedAntiTorque:%f", setTorque, posAntiiTorque, speedAntiTorque);

        // float outVal = -(position*1)-(speed*1);

        // float torqueLim = fabs(position);
        // if(torqueLim < deadband){
        //     torqueLim = deadband*5;
        // }

        // if(position >2){
        //     position = 2;
        // }else if(position<-2){
        //     position = -2;
        // }
        // if(fabs(position) < deadband ){
        //     position = 0;
        // }
        // ESP_LOGD(TAG, "pos:%f torqueLim: %f", position, torqueLim);
        // _send_can_float_package(CYBERGEAR_CAN_ID, ADDR_LIMIT_TORQUE, torqueLim, T_MIN, T_MAX);
        // _send_can_float_package(CYBERGEAR_CAN_ID, ADDR_I_REF, -position, -I_MAX, I_MAX);
        
        
            //ESP_LOGD(TAG, "Sending position: %f", position);
            //check_alerts();
        

        

        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
}



void start_cybergear_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_cybergear_%d", slot_num);
	xTaskCreatePinnedToCore(cybergear_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-12, NULL, 1);

	ESP_LOGD(TAG,"cybergear_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}