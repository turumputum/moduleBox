#include <stdio.h>
#include "distanceSens.h"

#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DISTANCE_SENS";


uint16_t crc16_modbus(uint8_t *data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    
    for (uint8_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return crc;
}


void distanceSens_config(distanceSens_t *distanceSens, uint8_t slot_num) {
    if (strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
        distanceSens->deadBand = get_option_int_val(slot_num, "deadBand");
        if (distanceSens->deadBand <= 0) {
            ESP_LOGD(TAG, "TOF dead_band wrong format, set default slot:%d", distanceSens->deadBand);
            distanceSens->deadBand = 1; //default val
        }else {
            ESP_LOGD(TAG, "TOF set dead_band:%d for slot:%d", distanceSens->deadBand, slot_num);
        }
    }


    if (strstr(me_config.slot_options[slot_num], "floatOutput") != NULL) {
        distanceSens->flag_float_output = 1;
        ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
    }

    if (strstr(me_config.slot_options[slot_num], "maxVal") != NULL) {
        distanceSens->maxVal = get_option_int_val(slot_num, "maxVal");
        ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", distanceSens->maxVal, slot_num);
    }
    if (strstr(me_config.slot_options[slot_num], "minVal") != NULL) {
        distanceSens->minVal = get_option_int_val(slot_num, "minVal");
        ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", distanceSens->minVal, slot_num);//11111111111111
    }

    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        distanceSens->threshold = get_option_int_val(slot_num, "threshold");
        if (distanceSens->threshold <= 0){
            ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
            distanceSens->threshold = 0; // default val
        }else{
            ESP_LOGD(TAG, "threshold:%d. Slot:%d", distanceSens->threshold, slot_num);
        }
    }

    if (strstr(me_config.slot_options[slot_num], "filterK") != NULL) {
        distanceSens->k = get_option_float_val(slot_num, "filterK");
        ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", distanceSens->k, slot_num);
    }

    if (strstr(me_config.slot_options[slot_num], "inverse") != NULL) {
        distanceSens->inverse = 1;
    }
}




//---------VL53TOF_task--------------
void VL53TOF_task(void* arg) {
    int slot_num = *(int*)arg;
    
    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

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
    #define BUF_SIZE 256
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);


    distanceSens_t distanceSens=DISTANCE_SENS_DEFAULT();
    distanceSens_config(&distanceSens, slot_num);
    
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/TOF_0")+3];
		sprintf(t_str, "%s/TOF_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}



    uint8_t rawByte[10];
    uint8_t f_msg_start = 0;
    uint8_t f_report = 0;
    uint8_t index = 0;

    char str[255];

    while (1) {
        //uint16_t dist = 0;
        //uint16_t checksum = 0;
        uint16_t len;
        char str[255];

        // Read bytes from UART
        //len = uart_read_bytes(uart_num, &rawByte[index], 1, 5 / portTICK_RATE_MS);
        len = uart_read_bytes(uart_num, &rawByte[index], 1, portMAX_DELAY);
        if (len > 0) {
            if (f_msg_start == 0) {
                if (rawByte[index] == 0x01) {
                    index++;
                    f_msg_start = 1;
                }
            } else if (index == 1) {
                if (rawByte[index] == 0x03) {
                    index++;
                } else {
                    index = 0;
                    f_msg_start = 0;
                }
            } else {
                index++;
                if (index == 7) {
                    // Verify checksum
                    if (crc16_modbus(&rawByte,5) == (rawByte[6] << 8 | rawByte[5])) {
                        
                        // Extract distance data
                        distanceSens.currentPos = (rawByte[3] << 8) | rawByte[4];
                        //ESP_LOGD(TAG, "Distance:%d", dist);
                        // Apply filtering if needed
                        if(distanceSens.currentPos>65000){
                            distanceSens.currentPos=0;
                        }

                        if (distanceSens.k < 1) {
                            distanceSens.currentPos = distanceSens.prevPos * (1 - distanceSens.k) + distanceSens.currentPos * distanceSens.k;
                        }
                        if(distanceSens.currentPos>distanceSens.maxVal){
                            distanceSens.currentPos=distanceSens.maxVal;
                        }else if(distanceSens.currentPos<distanceSens.minVal){
                            distanceSens.currentPos=distanceSens.minVal;
                        }
                        
                        // Apply threshold and inverse if configured
                        if (distanceSens.threshold > 0) {
                            distanceSens.state = (distanceSens.currentPos > distanceSens.threshold) ? distanceSens.inverse : !distanceSens.inverse;
                            if(distanceSens.state!=distanceSens.prevState){
                                memset(str, 0, strlen(str));
                                sprintf(str, "%d", distanceSens.state);
                                report(str,slot_num);
                                distanceSens.prevState = distanceSens.state;
                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        }else{
                            if(abs(distanceSens.currentPos-distanceSens.prevPos)> distanceSens.deadBand){
                                memset(str, 0, strlen(str));
                                if (distanceSens.flag_float_output == 1) {
                                    float f_res =  (float)(distanceSens.currentPos-distanceSens.minVal) / (distanceSens.maxVal - distanceSens.minVal);
                                    sprintf(str, "%f", f_res);
                                }else{
                                    sprintf(str, "%d", distanceSens.currentPos);
                                }

                                report(str,slot_num);

                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        } 
                    }
                    index = 0;
                    f_msg_start = 0;
                }
            }
        }
    }
}

void start_VL53TOF_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(VL53TOF_task, "VL53TOF_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "VL53TOF_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


//---------benewakeTOF_task--------------
void benewakeTOF_task(void* arg) {
    int slot_num = *(int*)arg;
    
    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

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
    #define BUF_SIZE 256
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);


    distanceSens_t distanceSens=DISTANCE_SENS_DEFAULT();
    distanceSens_config(&distanceSens, slot_num);
    
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/TOF_0")+3];
		sprintf(t_str, "%s/TOF_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}



    uint8_t rawByte[10];
    uint8_t f_msg_start = 0;
    uint8_t f_report = 0;
    uint8_t index = 0;
    uint8_t data[255];
    

    while (1) {
        //uint16_t dist = 0;
        //uint16_t checksum = 0;
        uint16_t len;
        char str[255];

        len = uart_read_bytes(uart_num, &rawByte[index], 1, portMAX_DELAY);
        if (len > 0) {
            if (f_msg_start == 0) {
                if (rawByte[index] == 0x59) {
                    index++;
                    f_msg_start = 1;
                }
            } else if (index == 1) {
                if (rawByte[index] == 0x59) {
                    index++;
                } else {
                    index = 0;
                    f_msg_start = 0;
                }
            } else {
                index++;
                if (index == 9) {
                    //ESP_LOGD(TAG, "Data: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", rawByte[0], rawByte[1], rawByte[2], rawByte[3], rawByte[4], rawByte[5], rawByte[6], rawByte[7], rawByte[8], rawByte[9]);
                    uint8_t checksum = 0;
                    for (int i = 0; i < 8; i++) {
                        checksum += rawByte[i];
                    }
                    if (checksum == rawByte[8]) {
                        //int16_t raw_val = ((uint16_t)data[3] << 8) | data[2];
                        
                        // Extract distance data
                        distanceSens.currentPos = (rawByte[3] << 8) | rawByte[2];
                        //ESP_LOGD(TAG, "Distance:%d", dist);
                        // Apply filtering if needed
                        if (distanceSens.k < 1) {
                            distanceSens.currentPos = distanceSens.prevPos * (1 - distanceSens.k) + distanceSens.currentPos * distanceSens.k;
                        }
                        if(distanceSens.currentPos>distanceSens.maxVal){
                            distanceSens.currentPos=distanceSens.maxVal;
                        }else if(distanceSens.currentPos<distanceSens.minVal){
                            distanceSens.currentPos=distanceSens.minVal;
                        }
                        
                        // Apply threshold and inverse if configured
                        if (distanceSens.threshold > 0) {
                            distanceSens.state = (distanceSens.currentPos > distanceSens.threshold) ? distanceSens.inverse : !distanceSens.inverse;
                            if(distanceSens.state!=distanceSens.prevState){
                                memset(str, 0, strlen(str));
                                sprintf(str, "%d", distanceSens.state);
                                report(str,slot_num);
                                distanceSens.prevState = distanceSens.state;
                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        }else{
                            if(abs(distanceSens.currentPos-distanceSens.prevPos)> distanceSens.deadBand){
                                memset(str, 0, strlen(str));
                                if (distanceSens.flag_float_output == 1) {
                                    float f_res =  (float)distanceSens.currentPos / (distanceSens.maxVal - distanceSens.minVal);
                                    sprintf(str, "%f", f_res);
                                }else{
                                    sprintf(str, "%d", distanceSens.currentPos);
                                }

                                report(str,slot_num);

                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        } 
                    }
                    index = 0;
                    f_msg_start = 0;
                }
            }
        }
    }
}


void start_benewakeTOF_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(benewakeTOF_task, "benewakeTOF_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "benewakeTOF_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


//---------benewakeTOF_task--------------
void benewakeTOF_task(void* arg) {
    int slot_num = *(int*)arg;
    
    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

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
    #define BUF_SIZE 256
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);


    distanceSens_t distanceSens=DISTANCE_SENS_DEFAULT();
    distanceSens_config(&distanceSens, slot_num);
    
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/TOF_0")+3];
		sprintf(t_str, "%s/TOF_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}



    uint8_t rawByte[10];
    uint8_t f_msg_start = 0;
    uint8_t f_report = 0;
    uint8_t index = 0;
    uint8_t data[255];
    

    while (1) {
        //uint16_t dist = 0;
        //uint16_t checksum = 0;
        uint16_t len;
        char str[255];

        len = uart_read_bytes(uart_num, &rawByte[index], 1, portMAX_DELAY);
        if (len > 0) {
            if (f_msg_start == 0) {
                if (rawByte[index] == 0x59) {
                    index++;
                    f_msg_start = 1;
                }
            } else if (index == 1) {
                if (rawByte[index] == 0x59) {
                    index++;
                } else {
                    index = 0;
                    f_msg_start = 0;
                }
            } else {
                index++;
                if (index == 9) {
                    //ESP_LOGD(TAG, "Data: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", rawByte[0], rawByte[1], rawByte[2], rawByte[3], rawByte[4], rawByte[5], rawByte[6], rawByte[7], rawByte[8], rawByte[9]);
                    uint8_t checksum = 0;
                    for (int i = 0; i < 8; i++) {
                        checksum += rawByte[i];
                    }
                    if (checksum == rawByte[8]) {
                        //int16_t raw_val = ((uint16_t)data[3] << 8) | data[2];
                        
                        // Extract distance data
                        distanceSens.currentPos = (rawByte[3] << 8) | rawByte[2];
                        //ESP_LOGD(TAG, "Distance:%d", dist);
                        // Apply filtering if needed
                        if (distanceSens.k < 1) {
                            distanceSens.currentPos = distanceSens.prevPos * (1 - distanceSens.k) + distanceSens.currentPos * distanceSens.k;
                        }
                        if(distanceSens.currentPos>distanceSens.maxVal){
                            distanceSens.currentPos=distanceSens.maxVal;
                        }else if(distanceSens.currentPos<distanceSens.minVal){
                            distanceSens.currentPos=distanceSens.minVal;
                        }
                        
                        // Apply threshold and inverse if configured
                        if (distanceSens.threshold > 0) {
                            distanceSens.state = (distanceSens.currentPos > distanceSens.threshold) ? distanceSens.inverse : !distanceSens.inverse;
                            if(distanceSens.state!=distanceSens.prevState){
                                memset(str, 0, strlen(str));
                                sprintf(str, "%d", distanceSens.state);
                                report(str,slot_num);
                                distanceSens.prevState = distanceSens.state;
                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        }else{
                            if(abs(distanceSens.currentPos-distanceSens.prevPos)> distanceSens.deadBand){
                                memset(str, 0, strlen(str));
                                if (distanceSens.flag_float_output == 1) {
                                    float f_res =  (float)distanceSens.currentPos / (distanceSens.maxVal - distanceSens.minVal);
                                    sprintf(str, "%f", f_res);
                                }else{
                                    sprintf(str, "%d", distanceSens.currentPos);
                                }

                                report(str,slot_num);

                                distanceSens.prevPos = distanceSens.currentPos;
                            }
                        } 
                    }
                    index = 0;
                    f_msg_start = 0;
                }
            }
        }
    }
}


void start_benewakeTOF_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(benewakeTOF_task, "benewakeTOF_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "benewakeTOF_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}