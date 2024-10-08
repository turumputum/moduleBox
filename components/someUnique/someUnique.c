#include <stdio.h>
#include "someUnique.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/uart.h>
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_intr_alloc.h"

#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SOME_UNIQUE";



void buttonMatrix4_task(void* arg) {
    int slot_num = *(int*)arg;

    uint8_t pin_row[4];
    pin_row[0] = SLOTS_PIN_MAP[slot_num][0];
    pin_row[1] = SLOTS_PIN_MAP[slot_num][1];
    pin_row[2] = SLOTS_PIN_MAP[slot_num][2];
    pin_row[3] = SLOTS_PIN_MAP[slot_num][3];
    for(int i=0; i<4; i++){
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL<<pin_row[i];
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 0;
        esp_err_t ret = gpio_config(&io_conf);
        if(ret!= ESP_OK){
            ESP_LOGE(TAG, "gpio_config failed");
        }
        ESP_LOGD(TAG, "Set output pin: %d", pin_row[i]);
    }
    uint8_t pin_col[4];
    pin_col[0] = SLOTS_PIN_MAP[slot_num+1][0];
    pin_col[1] = SLOTS_PIN_MAP[slot_num+1][1];
    pin_col[2] = SLOTS_PIN_MAP[slot_num+1][2];
    pin_col[3] = SLOTS_PIN_MAP[slot_num+1][3];
    for(int i=0; i<4; i++){
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = 1ULL<<pin_col[i];
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 0;
        esp_err_t ret = gpio_config(&io_conf);
        if(ret!= ESP_OK){
            ESP_LOGE(TAG, "gpio_config failed");
        }
        ESP_LOGD(TAG, "Set input pin: %d", pin_col[i]);
    }

    char t_str[strlen(me_config.deviceName)+strlen("/buttonMatrix_0")+3];
    sprintf(t_str, "%s/buttonMatrix_%d",me_config.deviceName, slot_num);
    me_state.trigger_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);

    int countMatrix[4][4];
    int resMatrix[4][4];
    int _resMatrix[4][4];
    int maxCount=6;

    while (1){
        vTaskDelay(5/portTICK_PERIOD_MS);

        for(int r=0; r<4; r++){
            for(int a=0; a<4; a++){
                gpio_set_level(pin_row[a], 0);
            }
            vTaskDelay(1/portTICK_PERIOD_MS);
            gpio_set_level(pin_row[r],1);
            vTaskDelay(1/portTICK_PERIOD_MS);
            for(int c=0; c<4; c++){
                if(gpio_get_level(pin_col[c])==1){
                    countMatrix[r][c]+=1;
                    if(countMatrix[r][c]>maxCount) countMatrix[r][c]=maxCount;
                }else{
                    countMatrix[r][c]-=1;
                    if(countMatrix[r][c]<0) countMatrix[r][c]=0;
                }

                if(countMatrix[r][c]>(maxCount/2)+1) resMatrix[r][c]=1;
                else if(countMatrix[r][c]<(maxCount/2)-1) resMatrix[r][c]=0;
            }
        }

        if(memcmp(resMatrix, _resMatrix, sizeof(resMatrix))!=0){
            for(int r=0; r<4; r++){
                for(int c=0; c<4; c++){
                    _resMatrix[r][c]=resMatrix[r][c];
                    if(resMatrix[r][c]==1){
                        ESP_LOGD(TAG, "ButtonMatrix4_task: slot:%d, row:%d, col:%d", slot_num, r, c);
                        char str[12];
                        memset(str, 0, strlen(str));
				        sprintf(str, "%d %d",r,c);
                        report(str, slot_num);
                    }
                }
            }           
        }

    }
    

}


void start_buttonMatrix4_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(buttonMatrix4_task, "buttonMatrix4_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-12, NULL);
    ESP_LOGD(TAG, "buttonMatrix4_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------UART logger----------------
static int uart_read(uint8_t UART_NUM, uint8_t *data, const int length)
{
    int len_read = 0;
    while (len_read < length)
    {
        int bytes_available = uart_read_bytes(UART_NUM, data + len_read, length - len_read, 20 / portTICK_PERIOD_MS);
        if (bytes_available <= 0)
            break;
        len_read += bytes_available;
    }
    return len_read;
}

void uartLogger_task(void* arg) {
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
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config (uart_num, &uart_config);
    gpio_reset_pin (tx_pin);
    gpio_reset_pin (rx_pin);
    uart_set_pin (uart_num, tx_pin, rx_pin, -1, -1);
    uart_is_driver_installed (uart_num);
    uart_driver_install (uart_num, 255, 255, 0, NULL, 0);
    
    #define BUF_SIZE 1024
    //char data[BUF_SIZE];

    // Открытие файла для записи
    char file_name[32];
    int file_index = 0;
    FILE *file;
    do {
        snprintf(file_name, sizeof(file_name), "/sdcard/log_%d.txt", file_index);
        file = fopen(file_name, "r");
        if (file != NULL) {
            fclose(file);
            file_index++;
        }
    } while (file != NULL);

    

    uint8_t data[BUF_SIZE];
    int len_read;

    ESP_LOGD(TAG, "start logging to file: %s", file_name);

    while (1) {
        len_read = uart_read(uart_num, data, BUF_SIZE);
        if (len_read > 0) {
            file = fopen(file_name, "a");
            if (file == NULL) {
                ESP_LOGE("app_main", "Failed to open file for writing");
                return;
            }
            fwrite(data, 1, len_read, file); // Запись данных в файл
            //fflush(file);                    // Принудительная запись данных на диск
            //fprintf(file, "%s", data);
            fclose(file);
            //ESP_LOGD(TAG, "write to file: %s", data);
        }
    }
}

void start_uartLogger_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "uartLogger_task_%d", slot_num);
	xTaskCreatePinnedToCore(uartLogger_task, tmpString, 1024*5, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"uartLogger_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//-------------------dialer_task----------------
static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}


void dialer_task(void* arg) {
    int slot_num = *(int*)arg;
    uint8_t ena_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pulse_pin = SLOTS_PIN_MAP[slot_num][1];

	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

	gpio_reset_pin(pulse_pin);
	esp_rom_gpio_pad_select_gpio(pulse_pin);
    gpio_config_t in_conf = {};
    in_conf.pull_up_en = 0;
    in_conf.pull_down_en =1;
   	in_conf.intr_type = GPIO_INTR_NEGEDGE;
    in_conf.pin_bit_mask = (1ULL<<pulse_pin);
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pulse_pin, GPIO_INTR_NEGEDGE);
    //gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
	gpio_isr_handler_add(pulse_pin, gpio_isr_handler, (void*)slot_num);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL<<ena_pin;
    io_conf.pull_down_en = 1;
    io_conf.pull_up_en = 0;
    esp_err_t ret = gpio_config(&io_conf);

    uint16_t waitingTime = 3000;//ms
    if (strstr(me_config.slot_options[slot_num], "waitingTime") != NULL) {
		waitingTime = get_option_int_val(slot_num, "waitingTime");
		ESP_LOGD(TAG, "Set waitingTime:%d for slot:%d", waitingTime, slot_num);
	}

    uint8_t enaInverse = 0;
    if (strstr(me_config.slot_options[slot_num], "enaInverse") != NULL) {
		enaInverse = get_option_int_val(slot_num, "enaInverse");
		ESP_LOGD(TAG, "Set enaInverse:%d for slot:%d", enaInverse, slot_num);
	}

    uint8_t numberMaxLenght = 7;
    if (strstr(me_config.slot_options[slot_num], "numberMaxLenght") != NULL) {
		numberMaxLenght = get_option_int_val(slot_num, "numberMaxLenght");
		ESP_LOGD(TAG, "Set numberMaxLenght:%d for slot:%d", numberMaxLenght, slot_num);
	}
    int debounceGap = 20;
	if (strstr(me_config.slot_options[slot_num], "debounceGap") != NULL) {
		debounceGap = get_option_int_val(slot_num, "debounceGap");
		ESP_LOGD(TAG, "Set debounceGap:%d for slot:%d",debounceGap, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "dialerTopic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/dialer_0")+3];
		sprintf(t_str, "%s/dialer_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart dialerTopic:%s", me_state.trigger_topic_list[slot_num]);
	}


    uint8_t counter=0;
    char number_str[numberMaxLenght];
    uint string_lenght=0;
    number_str[0]='\0';
    uint8_t prev_ena_state = 0;
    uint32_t dial_start_time = 0;
    uint8_t state_flag = 0;

    uint32_t tick=xTaskGetTickCount();

    while(1){
        vTaskDelay(pdMS_TO_TICKS(30));      
        
        uint8_t ena_state = gpio_get_level(ena_pin)?!enaInverse:enaInverse;
        if(ena_state!= prev_ena_state){
            prev_ena_state = ena_state;
            dial_start_time =pdTICKS_TO_MS(xTaskGetTickCount());
            if((ena_state == 1)&&(state_flag == 0)){
                state_flag = 1;
                counter = 0;
                //ESP_LOGD(TAG, "Lets input number: %s strlen:%d dial_start_time:%ld", number_str, strlen(number_str), dial_start_time);
            }else if(ena_state == 0){
                if(counter>=10)counter=0;
                number_str[string_lenght]=(char)counter+48;
                string_lenght++;
                number_str[string_lenght]='\0';
                //ESP_LOGD(TAG, "update number_str: %s counter:%d strlen:%d", number_str, counter, strlen(number_str));
                counter = 0;
            }
        }

        if(state_flag == 1){
            if(((pdTICKS_TO_MS(xTaskGetTickCount())-dial_start_time)>=waitingTime)||(string_lenght>=numberMaxLenght)){
                if(ena_state == 0){
                    //ESP_LOGD(TAG, "Input end, report number: %s", number_str);
                    report(number_str, slot_num);
                    memset(number_str, 0, numberMaxLenght);
                    state_flag = 0;
                    string_lenght = 0;
                }
            }
        }

        uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
            vTaskDelay(debounceGap);
            if(gpio_get_level(pulse_pin) == 0){
                // if(debounceGap!=0){
                //     if((xTaskGetTickCount()-tick)<debounceGap){
                //         //ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
                //         goto exit;
                //     }
                // }

                tick = xTaskGetTickCount();
                counter++;
            }
        }
        // exit:
    }

}

void start_dialer_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(dialer_task, "dialer_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-18, NULL);
    ESP_LOGD(TAG, "dialer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//-------------------academia-kick---------------------

int hex_to_dec(char high, char low) {
    int result = 0;
    result += (high >= 'A') ? (high - 'A' + 10) : (high - '0');
    result = result << 4;
    result += (low >= 'A') ? (low - 'A' + 10) : (low - '0');
    return result;
}

void academKick_task(void* arg) {
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
        .baud_rate = 19200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config (uart_num, &uart_config);
    gpio_reset_pin (tx_pin);
    gpio_reset_pin (rx_pin);
    uart_set_pin (uart_num, tx_pin, rx_pin, -1, -1);
    uart_is_driver_installed (uart_num);
    uart_driver_install (uart_num, 255, 255, 0, NULL, 0);
    
    #define BUF_SIZE 30
    uint8_t data[BUF_SIZE];
    memset(data, 0, BUF_SIZE);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/kick_0")+3];
		sprintf(t_str, "%s/kick_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

    while (1) {
        
        int len_read = uart_read_bytes(uart_num, data, 25, 100 / portTICK_PERIOD_MS);
        if(len_read > 0){
            // Remove the first byte
            //ESP_LOGD(TAG, "In data:%s str_1:", data);
            uint8_t str_1[4];
            memmove(data, data + 1, 8);
            for (int i = 0; i < sizeof(float); i++) {
                str_1[i]=hex_to_dec(data[i*2], data[i*2+1]);
            }

            // }
            //str_1[8]='\0';
            ESP_LOGD(TAG, "Str_1:%02x %02x %02x %02x", str_1[0], str_1[1], str_1[2], str_1[3]);
            // Convert bytes 1-6 to float (little endian)
            float result;// = *(float*)(str_1);
            memcpy(&result, str_1, sizeof(float));
            ESP_LOGI(TAG, "Received float value: %.4f - %d",(result*32), (int)roundf(result*32));

            char tmpString[255];
            memset(tmpString, 0, strlen(tmpString));
            sprintf(tmpString,"%d", (int)roundf(result*32));
            report(tmpString, slot_num);

            memset(data, 0, BUF_SIZE);
        }
    }


}

void start_academKick_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(academKick_task, "academKick_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-18, NULL);
    ESP_LOGD(TAG, "academKick_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}