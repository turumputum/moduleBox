// #include "sdkconfig.h"

// #include "reporter.h"
// #include "stateConfig.h"
// #include "esp_timer.h"

// #include "executor.h"
// #include "esp_log.h"
// #include "me_slot_config.h"

// #include <stdio.h>
// #include <string.h>
// #include <stdlib.h>
// #include <inttypes.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"
// #include "driver/gpio.h"
// #include <driver/uart.h>

// #include "hlk_sens.h"

// extern uint8_t SLOTS_PIN_MAP[10][4];
// extern configuration me_config;
// extern stateStruct me_state;

// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
// static const char *TAG = "HLK_SENS";

// void hlk2420_task(void* arg) {
//     int slot_num = *(int*)arg;

//     int uart_num = UART_NUM_1; // Начинаем с минимального порта
//     while (uart_is_driver_installed(uart_num)) {
//         uart_num++;
//         if (uart_num >= UART_NUM_MAX) {
//             ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
//             vTaskDelete(NULL);
//         }
//     }

//     uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
//     uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

//     uart_config_t uart_config = {
//         .baud_rate = 115200,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
//         .source_clk = UART_SCLK_DEFAULT,
//     };
//     uart_param_config (uart_num, &uart_config);
//     gpio_reset_pin (tx_pin);
//     gpio_reset_pin (rx_pin);
//     uart_set_pin (uart_num, tx_pin, rx_pin, -1, -1);
//     uart_is_driver_installed (uart_num);
//     uart_driver_install (uart_num, 255, 255, 0, NULL, 0);
    
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
//     uint16_t MAX_VAL = 800;
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
//         ESP_LOGD(TAG, "Set threshold:%d. Slot:%d", threshold, slot_num);
//     }

//     uint32_t fall_lag = 3000*1000;
//     if (strstr(me_config.slot_options[slot_num], "fall_lag") != NULL) {
//         fall_lag = get_option_int_val(slot_num, "fall_lag")*1000;
//         ESP_LOGD(TAG, "Set fall_lag:%lu. Slot:%d", fall_lag, slot_num);
//     }


//     char t_str[strlen(me_config.deviceName)+strlen("/hlk2420_0")+3];
//     sprintf(t_str, "%s/hlk2420_%d",me_config.deviceName, slot_num);
//     me_state.trigger_topic_list[slot_num]=strdup(t_str);
//     ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);

//     #define BUF_SIZE 256
//     char data[BUF_SIZE];
//     int len;
//     uint8_t rawByte[3];
//     int index = 0;
//     uint16_t dist = 0;
//     uint16_t _dist = 0;
//     int state=0;
//     int _state=0;
//     uint32_t fall_tick=0;

//     while(1){
//         len = uart_read_bytes(uart_num, rawByte, 1, 10 / portTICK_RATE_MS);
//         if (len > 0) {
//             data[index] = rawByte[0];
//             index++;
//             if (data[index-1] == '\n') {
//                 if(index > 2) { 
//                     if(strstr(data, "OFF") != NULL) {
//                         if(state==1)fall_tick = esp_timer_get_time();
//                         state = 0;
//                         dist = MAX_VAL;
//                     }else if(strstr(data, "Range") != NULL) {
//                         dist = atoi(data+5);
//                     }else if(strstr(data, "ON") != NULL){
//                         if(threshold!= 0) {
//                             if(dist < threshold) {
//                                 state = 1;
//                             }else {
//                                 if(state==1)fall_tick = esp_timer_get_time();
//                                 state = 0;
//                             }
//                         }else {
//                             state = 1;
//                         }
//                     }
//                     //char *str=strndup(data, index-1);
//                     //ESP_LOGD(TAG, "state:%d dist:%d UART data:%s", state, dist, str);
//                 }
//                 memset(data, 0, index);
//                 index = 0;
//             }
//         }

//         if(state != _state) {
//             // if(state == 0) {
//             //     ESP_LOGD(TAG, "fall delta:%lld fall_lag:%lu", esp_timer_get_time() - fall_tick, fall_lag);
//             // }
//             if((state == 1)||(state ==0 && ((esp_timer_get_time() - fall_tick) > fall_lag))) {
//                 _state=state;
//                 char str[10];
//                 memset(str, 0, strlen(str));
//                 sprintf(str, "%d", state);
//                 report(str, slot_num);
//             }
//         }

//         if(dist != _dist) {
//             _dist=dist;
//             char str[50];
//             memset(str, 0, strlen(str));
//             sprintf(str, "/distance:%d", dist);
//             report(str, slot_num);
//         }

//     }

// }

// void start_hlk2420_task(int slot_num){
// 	uint32_t heapBefore = xPortGetFreeHeapSize();
// 	int t_slot_num = slot_num;
// 	char tmpString[60];
// 	sprintf(tmpString, "task_hlk2420_%d", slot_num);
// 	xTaskCreatePinnedToCore(hlk2420_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

// 	ESP_LOGD(TAG,"hlk2420 task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }

// void hlk2410_task(void* arg) {
//     int slot_num = *(int*)arg;

//     int uart_num = UART_NUM_1; // Начинаем с минимального порта
//     while (uart_is_driver_installed(uart_num)) {
//         uart_num++;
//         if (uart_num >= UART_NUM_MAX) {
//             ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
//             vTaskDelete(NULL);
//         }
//     }

//     uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
//     uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

//     uart_config_t uart_config = {
//         .baud_rate = 256000,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
//         .source_clk = UART_SCLK_DEFAULT,
//     };
//     uart_param_config (uart_num, &uart_config);
//     gpio_reset_pin (tx_pin);
//     gpio_reset_pin (rx_pin);
//     uart_set_pin (uart_num, tx_pin, rx_pin, -1, -1);
//     uart_is_driver_installed (uart_num);
//     uart_driver_install (uart_num, 255, 255, 0, NULL, 0);
    
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
//     uint16_t MAX_VAL = 800;
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
//         ESP_LOGD(TAG, "Set threshold:%d. Slot:%d", threshold, slot_num);
//     }

//     uint32_t fall_lag = 3000*1000;
//     if (strstr(me_config.slot_options[slot_num], "fall_lag") != NULL) {
//         fall_lag = get_option_int_val(slot_num, "fall_lag")*1000;
//         ESP_LOGD(TAG, "Set fall_lag:%lu. Slot:%d", fall_lag, slot_num);
//     }


//     if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
// 		char* custom_topic=NULL;
//     	custom_topic = get_option_string_val(slot_num, "topic");
// 		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
// 		ESP_LOGD(TAG, "tempTopic:%s", me_state.trigger_topic_list[slot_num]);
//     }else{
// 		char t_str[strlen(me_config.deviceName)+strlen("/hlk2410_0")+3];
// 		sprintf(t_str, "%s/hlk2410_%d",me_config.deviceName, slot_num);
// 		me_state.trigger_topic_list[slot_num]=strdup(t_str);
// 		ESP_LOGD(TAG, "Standart tempTopic:%s", me_state.trigger_topic_list[slot_num]);
// 	}

//     // char t_str[strlen(me_config.deviceName)+strlen("/hlk2410_0")+3];
//     // sprintf(t_str, "%s/hlk2410_%d",me_config.deviceName, slot_num);
//     // me_state.trigger_topic_list[slot_num]=strdup(t_str);
//     // ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);

//     #define BUF_SIZE 256
//     char data[BUF_SIZE];
//     int len;
//     uint8_t rawByte[3];
//     int index = 0;
//     uint16_t dist = 0;
//     uint16_t _dist = 0;
//     int state=0;
//     int _state=0;
//     uint32_t fall_tick=0;

//     #define START_PACKET_BYTES "\xf4\xf3\xf2\xf1"
//     #define END_PACKET_BYTES "\xf8\xf7\xf6\xf5"
//     int packet_start = -1;
//     int packet_end = -1;
//     int packet_len = 0;

//     while(1){
//         int len_read = 0;
//         while (len_read < BUF_SIZE){
//             int bytes_available = uart_read_bytes(uart_num, data + len_read, BUF_SIZE - len_read, 20 / portTICK_PERIOD_MS);
//             if (bytes_available <= 0)
//                 break;
//             len_read += bytes_available;
//         }
//         if (len_read > 0){
//             for (int i = 0; i < len_read; i++){
//                 // Поиск начала пакета
//                 if (packet_start == -1 && data[i] == START_PACKET_BYTES[0]){
//                     if (strncmp((char *)&data[i], START_PACKET_BYTES, strlen(START_PACKET_BYTES)) == 0){
//                         packet_start = i;
//                     }
//                 }
//                 // Поиск конца пакета
//                 else if (packet_start != -1 && data[i] == END_PACKET_BYTES[0]){
//                     if (strncmp((char *)&data[i], END_PACKET_BYTES, strlen(END_PACKET_BYTES)) == 0){
//                         packet_end = i + strlen(END_PACKET_BYTES);
//                         break;
//                     }
//                 }
//             }

//             // Если найден полный пакет, копируем его в буфер
//             if (packet_start != -1 && packet_end != -1){
//                 packet_len = packet_end - packet_start - strlen(START_PACKET_BYTES);
//                 if (packet_len <= BUF_SIZE){
//                     uint8_t packet_buffer[packet_len];
//                     memcpy(packet_buffer, &data[packet_start + strlen(START_PACKET_BYTES)], packet_len);
//                     //ESP_LOGD(TAG, "UART data read:%s packLen:%d", packet_buffer, packet_len);
//                     if(packet_buffer[4] == 0x00){
//                         dist = MAX_VAL;
//                     }else if((packet_buffer[4] == 0x01)||(packet_buffer[4] == 0x03)){
//                         //moving target
//                         dist = (packet_buffer[6]<<8) + packet_buffer[5];
//                     }else if(packet_buffer[4] == 0x02){
//                         dist = (packet_buffer[9]<<8) + packet_buffer[8];
//                     }
//                     // int16_t rawDist_1 = (packet_buffer[6]<<8) + packet_buffer[5];
//                     // int16_t rawDist_2 = (packet_buffer[9]<<8) + packet_buffer[8];
//                     if(dist>=threshold){
//                         if(state==1)fall_tick = esp_timer_get_time();
//                         state = 0;
//                     }else{
//                         state = 1;                        
//                     }
//                     //ESP_LOGD(TAG, "UART rawDist:%d", dist);
//                     //ESP_LOG_BUFFER_HEX(TAG, packet_buffer,packet_len);
//                 }
//             }
//             //ESP_LOGD(TAG, "UART data read:%s", data);
//             //ESP_LOG_BUFFER_HEX(TAG, data,len_read);
//         }
//         vTaskDelay(100 / portTICK_PERIOD_MS);


//         if(state != _state) {
//             if((state == 1)||(state ==0 && ((esp_timer_get_time() - fall_tick) > fall_lag))) {
//                 _state=state;
//                 char str[10];
//                 memset(str, 0, strlen(str));
//                 sprintf(str, "%d", state);
//                 report(str, slot_num);
//             }
//         }

//         if(abs(dist -_dist)>deadBand) {
//             _dist=dist;
//             char str[50];
//             memset(str, 0, strlen(str));
//             if(flag_float_output){
//                 float f_res =  (float)(dist-MIN_VAL) / (MAX_VAL - MIN_VAL);
//                 sprintf(str, "%f", f_res);
//             }else{
//                 sprintf(str, "/distance:%d", dist);
//             }
//             report(str, slot_num);
//         }
//     }

// }


// void start_hlk2410_task(int slot_num){
// 	uint32_t heapBefore = xPortGetFreeHeapSize();
// 	int t_slot_num = slot_num;
// 	char tmpString[60];
// 	sprintf(tmpString, "task_hlk2410_%d", slot_num);
// 	xTaskCreatePinnedToCore(hlk2410_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

// 	ESP_LOGD(TAG,"hlk2410 task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }