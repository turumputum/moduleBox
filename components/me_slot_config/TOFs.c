// #include <stdint.h>
// #include <stdio.h>
// #include <string.h>
// #include "driver/gpio.h"
// #include "esp_timer.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "driver/uart.h"

// #include "reporter.h"
// #include "stateConfig.h"

// #include "esp_log.h"
// #include "me_slot_config.h"

// extern uint8_t SLOTS_PIN_MAP[10][4];
// extern configuration me_config;
// extern stateStruct me_state;

// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
// static const char* TAG = "TOFs";

// // uint16_t crc16_modbus(uint8_t *data, uint8_t length) {
// //     uint16_t crc = 0xFFFF;
    
// //     for (uint8_t i = 0; i < length; i++) {
// //         crc ^= (uint16_t)data[i];
// //         for (uint8_t j = 0; j < 8; j++) {
// //             if (crc & 0x0001) {
// //                 crc = (crc >> 1) ^ 0xA001;
// //             } else {
// //                 crc = crc >> 1;
// //             }
// //         }
// //     }
    
// //     return crc;
// // }

// //---------benewakeTOF_task--------------
// void benewakeTOF_task(void* arg) {
//     int slot_num = *(int*)arg;
//     if (slot_num > 1) {
//         ESP_LOGD(TAG, "Wrong slot!!!");
//         vTaskDelete(NULL);
//     }

//     uint8_t uart_num = UART_NUM_2;
//     if (slot_num == 0) {
//         uart_num = UART_NUM_2;
//     }
//     else if (slot_num == 1) {
//         uart_num = UART_NUM_1;
//     }
// #define BUF_SIZE 256
//     uint8_t data[BUF_SIZE];
//     size_t len;

//     uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
//     uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

//     uart_config_t uart_config = {
//         .baud_rate = 115200,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
//     };
//     uart_param_config(uart_num, &uart_config);
//     uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//     uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);

//     uint16_t deadBand = 1;
//     if (strstr(me_config.slot_options[slot_num], "dead_band") != NULL) {
//         deadBand = get_option_int_val(slot_num, "dead_band");
//         if (deadBand <= 0) {
//             ESP_LOGD(TAG, "TOF dead_band wrong format, set default slot:%d", deadBand);
//             deadBand = 1; //default val
//         }
//         else {
//             ESP_LOGD(TAG, "TOF set dead_band:%d for slot:%d", deadBand, slot_num);
//         }
//     }

//     uint16_t MIN_VAL = 0;
//     uint16_t MAX_VAL = 12000;
//     uint8_t flag_float_output = 0;
//     if (strstr(me_config.slot_options[slot_num], "float_output") != NULL) {
//         flag_float_output = 1;
//         ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
//     }
//     if (strstr(me_config.slot_options[slot_num], "max_val") != NULL) {
//         MAX_VAL = get_option_int_val(slot_num, "max_val");
//         ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", MAX_VAL, slot_num);
//     }
//     if (strstr(me_config.slot_options[slot_num], "min_val") != NULL) {
//         MIN_VAL = get_option_int_val(slot_num, "min_val");
//         ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", MIN_VAL, slot_num);
//     }

//     uint16_t threshold = 0;
//     if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
//         threshold = get_option_int_val(slot_num, "threshold");
//         if (threshold <= 0)
//         {
//             ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
//             threshold = 0; // default val
//         }
//     }

//     float k = 1;
//     if (strstr(me_config.slot_options[slot_num], "filter_k") != NULL) {
//         k = get_option_float_val(slot_num, "filter_k");
//         ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", k, slot_num);
//     }

//     uint8_t inverse = 0;
//     if (strstr(me_config.slot_options[slot_num], "inverse") != NULL) {
//         inverse = 1;
//     }

//     uint8_t flag_custom_topic = 0;
//     char* custom_topic = NULL;
//     if (strstr(me_config.slot_options[slot_num], "custom_topic") != NULL) {
//         custom_topic = get_option_string_val(slot_num, "custom_topic");
//         ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
//         flag_custom_topic = 1;
//     }
//     if (flag_custom_topic == 0) {
//         char* str = calloc(strlen(me_config.deviceName) + strlen("/distance_") + 4, sizeof(char));
//         sprintf(str, "%s/distance_%d", me_config.deviceName, slot_num);
//         me_state.trigger_topic_list[slot_num] = str;
//     }
//     else {
//         me_state.trigger_topic_list[slot_num] = custom_topic;
//     }



//     uint8_t rawByte[3];
//     uint8_t f_msg_start = 0;
//     uint8_t f_report = 0;
//     uint8_t index = 0;
//     uint64_t  reportTick = esp_timer_get_time();
//     uint16_t prev_dist = 0;
//     uint32_t msgTick = xTaskGetTickCount();
//     int16_t resault = 0;
//     char str[255];

//     while (1) {
//         uint16_t dist = 0;

//         len = uart_read_bytes(uart_num, rawByte, 1, 5 / portTICK_RATE_MS);
//         //len = uart_read_bytes(uart_num, rawByte, 1, portMAX_DELAY);
//         if (len > 0) {
//             if (f_msg_start == 1) {
//                 data[index] = rawByte[0];
//                 index++;
//                 if (index == 10) {
//                     uint8_t checksum = 0;
//                     for (int i = 0; i < 8; i++) {
//                         checksum += data[i];
//                     }
//                     if (checksum == data[8]) {
//                         int16_t raw_val = ((uint16_t)data[3] << 8) | data[2];
//                         if (k < 1) {
//                             dist = dist * (1 - k) + raw_val * k;
//                         }else {
//                             dist = raw_val;
//                         }
//                         if(dist>MAX_VAL){
//                             dist=MAX_VAL;
//                         }

                        
//                         if(threshold>0){
//                             if(dist>threshold){
//                                 dist=inverse;
//                             }else{
//                                 dist=!inverse;
//                             }
//                         }else{
//                             if (inverse) {
//                                 dist = MAX_VAL - dist;
//                             }
//                         }

//                         uint16_t delta = abs(prev_dist - dist);
                        
//                         if ((dist <= MAX_VAL) && (delta >= deadBand)) {
//                             ESP_LOGD(TAG, "dist:%d dead_band:%d threshold:%d", dist, deadBand, threshold);
//                             resault = dist;
//                             f_report = 1;
//                             prev_dist = dist;
//                         }
//                         vTaskDelay(pdMS_TO_TICKS(50));
//                         uart_flush_input(uart_num);
//                     }
//                     else {
//                         //ESP_LOGD(TAG,"Checksum FAIL %d sum:%d data:%d",slot_num,checksum, data[8]);
//                     }
//                     index = 0;
//                     f_msg_start = 0;
//                 }
//             }

//             if (rawByte[0] == 0x59) {
//                 if (index == 0) {
//                     data[index] = rawByte[0];
//                     index++;
//                 }else if (index == 1) {
//                     data[index] = rawByte[0];
//                     f_msg_start = 1;
//                     index++;
//                 }
//             }
//         }

//         if (f_report==1) {
//             //ESP_LOGD(TAG,"TOF:%d distance is:%d",slot_num,dist);
//             f_report = 0;

            
//             float f_res = 0;
//             memset(str, 0, strlen(str));

//             if (flag_float_output) {
//                 if (resault > MAX_VAL)resault = MAX_VAL;
//                 if (resault < MIN_VAL)resault = MIN_VAL;
//                 resault -= MIN_VAL;
//                 f_res = (float)resault / (MAX_VAL - MIN_VAL);
//             }else {
                
//             }

//             if (flag_custom_topic) {
//                 if (flag_float_output) {
//                     sprintf(str, "%s:%f", custom_topic, f_res);
//                 }else {
//                     sprintf(str, "%s:%d", custom_topic, resault);
//                 }
//             }else {

//                 if (flag_float_output) {
//                     sprintf(str, "%s/distance_%d:%f", me_config.deviceName, slot_num, f_res);
//                 }else {
//                     sprintf(str, "%s/distance_%d:%d", me_config.deviceName, slot_num, resault);
//                 }
//             }
//             report(str, slot_num);
//             //msgTick = xTaskGetTickCount();
//         }

//         // if(abs(xTaskGetTickCount()-msgTick)>5000000){
//         //         ESP_LOGD(TAG,"TOF fail! try  reset");
//         //         vTaskDelay(pdMS_TO_TICKS(100));
//         //         esp_restart();
//         //     }


        
//     }
// }

// void start_benewakeTOF_task(int slot_num) {
//     uint32_t heapBefore = xPortGetFreeHeapSize();
//     xTaskCreate(benewakeTOF_task, "benewakeTOF_task", 1024 * 4, &slot_num, 5, NULL);
//     ESP_LOGD(TAG, "Lidar init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }


// //---------TOF050F_task--------------
// void TOF050F_task(void* arg) {
//     int slot_num = *(int*)arg;
//     if (slot_num > 1) {
//         ESP_LOGD(TAG, "Wrong slot!!!");
//         vTaskDelete(NULL);
//     }

//     uint8_t uart_num = UART_NUM_2;
//     if (slot_num == 0) {
//         uart_num = UART_NUM_2;
//     }
//     else if (slot_num == 1) {
//         uart_num = UART_NUM_1;
//     }
// #define BUF_SIZE 256
//     uint8_t data[BUF_SIZE];
//     size_t len;

//     uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
//     uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

//     uart_config_t uart_config = {
//         .baud_rate = 115200,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
//     };
//     uart_param_config(uart_num, &uart_config);
//     uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//     uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);

//     uint16_t deadBand = 1;
//     if (strstr(me_config.slot_options[slot_num], "dead_band") != NULL) {
//         deadBand = get_option_int_val(slot_num, "dead_band");
//         if (deadBand <= 0) {
//             ESP_LOGD(TAG, "TOF dead_band wrong format, set default slot:%d", deadBand);
//             deadBand = 1; //default val
//         }
//         else {
//             ESP_LOGD(TAG, "TOF set dead_band:%d for slot:%d", deadBand, slot_num);
//         }
//     }

//     uint16_t MIN_VAL = 0;
//     uint16_t MAX_VAL = 500;
//     uint8_t flag_float_output = 0;
//     if (strstr(me_config.slot_options[slot_num], "float_output") != NULL) {
//         flag_float_output = 1;
//         ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
//     }
//     if (strstr(me_config.slot_options[slot_num], "max_val") != NULL) {
//         MAX_VAL = get_option_int_val(slot_num, "max_val");
//         ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", MAX_VAL, slot_num);
//     }
//     if (strstr(me_config.slot_options[slot_num], "min_val") != NULL) {
//         MIN_VAL = get_option_int_val(slot_num, "min_val");
//         ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", MIN_VAL, slot_num);
//     }

//     uint16_t threshold = 0;
//     if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
//         threshold = get_option_int_val(slot_num, "threshold");
//         if (threshold <= 0){
//             ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
//             threshold = 0; // default val
//         }else{
//             ESP_LOGD(TAG, "threshold:%d. Slot:%d", threshold, slot_num);
//         }
//     }

//     float k = 1;
//     if (strstr(me_config.slot_options[slot_num], "filter_k") != NULL) {
//         k = get_option_float_val(slot_num, "filter_k");
//         ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", k, slot_num);
//     }

//     uint8_t inverse = 0;
//     if (strstr(me_config.slot_options[slot_num], "inverse") != NULL) {
//         inverse = 1;
//     }

//     uint8_t flag_custom_topic = 0;
//     char* custom_topic = NULL;
//     if (strstr(me_config.slot_options[slot_num], "custom_topic") != NULL) {
//         custom_topic = get_option_string_val(slot_num, "custom_topic");
//         ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
//         flag_custom_topic = 1;
//     }
//     if (flag_custom_topic == 0) {
//         char* str = calloc(strlen(me_config.deviceName) + strlen("/distance_") + 4, sizeof(char));
//         sprintf(str, "%s/distance_%d", me_config.deviceName, slot_num);
//         me_state.trigger_topic_list[slot_num] = str;
//     }
//     else {
//         me_state.trigger_topic_list[slot_num] = custom_topic;
//     }



//     uint8_t rawByte[10];
//     uint8_t f_msg_start = 0;
//     uint8_t f_report = 0;
//     uint8_t index = 0;
//     uint64_t  reportTick = esp_timer_get_time();
//     uint16_t prev_dist = 0;
//     uint32_t msgTick = xTaskGetTickCount();
//     int16_t resault = 0;
//     char str[255];

//     while (1) {
//         uint16_t dist = 0;
//         uint16_t checksum = 0;

//         // Read bytes from UART
//         //len = uart_read_bytes(uart_num, &rawByte[index], 1, 5 / portTICK_RATE_MS);
//         len = uart_read_bytes(uart_num, &rawByte[index], 1, portMAX_DELAY);
//         if (len > 0) {
//             if (f_msg_start == 0) {
//                 if (rawByte[index] == 0x01) {
//                     index++;
//                     f_msg_start = 1;
//                 }
//             } else if (index == 1) {
//                 if (rawByte[index] == 0x03) {
//                     index++;
//                 } else {
//                     index = 0;
//                     f_msg_start = 0;
//                 }
//             } else {
//                 index++;
//                 if (index == 7) {
//                     // Verify checksum
//                     // if (crc16_modbus(&rawByte,5) == (rawByte[6] << 8 | rawByte[5])) {
                        
//                     //     // Extract distance data
//                     //     dist = (rawByte[3] << 8) | rawByte[4];
//                     //     //ESP_LOGD(TAG, "Distance:%d", dist);
//                     //     // Apply filtering if needed
//                     //     if (k < 1) {
//                     //         dist = prev_dist * (1 - k) + dist * k;
//                     //     }
                        
//                     //     // Apply threshold and inverse if configured
//                     //     if (threshold > 0) {
//                     //         dist = (dist > threshold) ? inverse : !inverse;
//                     //         ESP_LOGD(TAG, "threshold:%d distance is:%d", threshold, dist);
//                     //         if(dist)
//                     //     } else{
//                     //         if (inverse) {
//                     //             dist = MAX_VAL - dist;
//                     //         }
//                     //     }
                        
//                     //     // Check if the change is significant enough to report
//                     //     if (abs(prev_dist - dist) >= deadBand) {
//                     //         f_report = 1;
//                     //         prev_dist = dist;
//                     //     }
//                     // }
//                     index = 0;
//                     f_msg_start = 0;
//                 }
//             }
//         }
        
//         // Report the data if needed
//         if (f_report==1) {
//             //ESP_LOGD(TAG,"TOF:%d distance is:%d",slot_num,dist);
//             f_report = 0;
//             resault = dist;
            
//             float f_res = 0;
//             memset(str, 0, strlen(str));

//             if (flag_float_output) {
//                 if (resault > MAX_VAL)resault = MAX_VAL;
//                 if (resault < MIN_VAL)resault = MIN_VAL;
//                 resault -= MIN_VAL;
//                 f_res = (float)resault / (MAX_VAL - MIN_VAL);
//             }else {
                
//             }

//             if (flag_float_output) {
//                 sprintf(str, "%f", f_res);
//             }else {
//                 sprintf(str, "%d", resault);
//             }

//             report(str, slot_num);
//             //msgTick = xTaskGetTickCount();
//         }
    
//     //vTaskDelay(pdMS_TO_TICKS(5));

//     }
// }

// void start_TOF050F_task(int slot_num) {
//     uint32_t heapBefore = xPortGetFreeHeapSize();
//     xTaskCreate(TOF050F_task, "TOF050F_task", 1024 * 4, &slot_num, 5, NULL);
//     ESP_LOGD(TAG, "TOF050F_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }