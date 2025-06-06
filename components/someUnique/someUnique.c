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



void buttonMatrix_task(void* arg) {
    int slot_num = *(int*)arg;

    int outSlots[5] = {0};
    int outSlotsCount = 0;

    if (strstr(me_config.slot_options[slot_num], "outSlots:") != NULL) {
        char *strPtr=strstr(me_config.slot_options[slot_num], "outSlots:")+strlen("outSlots:");
        char strDup[strlen(strPtr)];
        strcpy(strDup, strPtr);
        char* rest = NULL;
        char* payload = strtok_r(strDup, ",", &rest);
        
        if (payload != NULL) {
            char* token = strtok(payload, " ");
            while (token != NULL && outSlotsCount < 5) {
                outSlots[outSlotsCount++] = atoi(token);
                token = strtok(NULL, " ");
            }
        }
    }else{
        outSlots[0]=slot_num;
        outSlotsCount=1;
    }
    uint8_t out_pin[outSlotsCount+3];
    for(int i=0; i<outSlotsCount; i++){
        out_pin[i*3] = SLOTS_PIN_MAP[outSlots[i]][0];
        out_pin[i*3+1] = SLOTS_PIN_MAP[outSlots[i]][1];
        out_pin[i*3+2] = SLOTS_PIN_MAP[outSlots[i]][2];
    }
    

    int inSlots[5] = {0};
    int inSlotsCount = 0;
    if (strstr(me_config.slot_options[slot_num], "inSlots:") != NULL) {
        char *strPtr=strstr(me_config.slot_options[slot_num], "inSlots:")+strlen("inSlots:");
        char strDup[strlen(strPtr)];
        strcpy(strDup, strPtr);
        char* rest = NULL;
        char* payload = strtok_r(strDup, ",", &rest);
        
        if (payload != NULL) {
            char* token = strtok(payload, " ");
            while (token != NULL && inSlotsCount < 5) {
                inSlots[inSlotsCount++] = atoi(token);
                token = strtok(NULL, " ");
            }
        }
    }else{
        inSlots[0]=slot_num+1;
        inSlotsCount=1;
    }
    uint8_t in_pin[inSlotsCount+3];
    for(int i=0; i<inSlotsCount; i++){
        in_pin[i*3] = SLOTS_PIN_MAP[inSlots[i]][0];
        in_pin[i*3+1] = SLOTS_PIN_MAP[inSlots[i]][1];
        in_pin[i*3+2] = SLOTS_PIN_MAP[inSlots[i]][2];
    }

    for(int x=0; x<outSlotsCount*3; x++){
        for(int y=0; y<inSlotsCount*3; y++){
            if(out_pin[x]==in_pin[y]){
                char errorString[50];
                sprintf(errorString,  "buttonMatrix_%d wrong slots config", slot_num);
                ESP_LOGE(TAG, "%s", errorString);
                writeErrorTxt(errorString);
                vTaskDelete(NULL);
            }
        }
    }

    for(int i=0; i<(outSlotsCount*3); i++){
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL<<out_pin[i];
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 0;
        esp_err_t ret = gpio_config(&io_conf);
        if(ret!= ESP_OK){
            ESP_LOGE(TAG, "gpio_config failed");
        }
        ESP_LOGD(TAG, "Set output pin: %d", out_pin[i]);
    }

    for(int i=0; i<(inSlotsCount*3); i++){
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = 1ULL<<in_pin[i];
        io_conf.pull_down_en = 1;
        io_conf.pull_up_en = 0;
        esp_err_t ret = gpio_config(&io_conf);
        if(ret!= ESP_OK){
            ESP_LOGE(TAG, "gpio_config failed");
        }
        ESP_LOGD(TAG, "Set input pin: %d", in_pin[i]);
    }

    char charMap[(inSlotsCount*3*outSlotsCount*3)+1];
    memset(charMap, 0, sizeof(charMap));
    if (strstr(me_config.slot_options[slot_num], "mapping:") != NULL) {
        char *strPtr=strstr(me_config.slot_options[slot_num], "mapping:")+strlen("mapping:");
        if(strstr(strPtr, ",")!=NULL){
            char* pt = strchr(strPtr, ',');
            pt[0] = "\0";
        }
        int len=strlen(strPtr);
        if(len>(inSlotsCount*3*outSlotsCount*3)){
            len=inSlotsCount*3*outSlotsCount*3;
        }
        strPtr[len]='\0';
        strcpy(charMap, strPtr);
    }
    ESP_LOGD(TAG, "mapping:%s", charMap);


    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "dialerTopic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/buttonMatrix_0")+3];
		sprintf(t_str, "%s/buttonMatrix_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart buttonMatrixTopic:%s", me_state.trigger_topic_list[slot_num]);
	}

    for(int i=0; i<(outSlotsCount*3); i++){
        gpio_set_level(out_pin[i],0);
    }


    int countMatrix[outSlotsCount*3][inSlotsCount*3];
    memset(countMatrix, 0, sizeof(countMatrix));
    uint8_t resMatrix[outSlotsCount*3][inSlotsCount*3];
    memset(resMatrix, 0, sizeof(resMatrix));
    uint8_t _resMatrix[outSlotsCount*3][inSlotsCount*3];
    memset(_resMatrix, 0, sizeof(_resMatrix));
    uint8_t maxCount=6;

    waitForWorkPermit(slot_num);

    while (1){
        vTaskDelay(15/portTICK_PERIOD_MS);

        for(int r=0; r<(outSlotsCount*3); r++){
            // for(int a=0; a<(outSlotsCount*3); a++){
            //      gpio_set_level(out_pin[a],0);
            // }
            // vTaskDelay(1/portTICK_PERIOD_MS);
            gpio_set_level(out_pin[r],1);
            vTaskDelay(1/portTICK_PERIOD_MS);
            for(int c=0; c<(inSlotsCount*3); c++){
                if(gpio_get_level(in_pin[c])==1){
                    countMatrix[r][c]+=1;
                    if(countMatrix[r][c]>maxCount) countMatrix[r][c]=maxCount;
                }else{
                    countMatrix[r][c]-=1;
                    if(countMatrix[r][c]<0) countMatrix[r][c]=0;
                }

                if(countMatrix[r][c]>(maxCount/2)+1){ 
                    resMatrix[r][c]=1;
                }else if(countMatrix[r][c]<(maxCount/2)-1) resMatrix[r][c]=0;
            }
            gpio_set_level(out_pin[r],0);
            //vTaskDelay(1/portTICK_PERIOD_MS);
        }

        if(memcmp(resMatrix, _resMatrix, sizeof(resMatrix))!=0){
            for(int r=0; r<(outSlotsCount*3); r++){
                for(int c=0; c<(inSlotsCount*3); c++){
                    //printf("%d ", resMatrix[r][c]);
                    _resMatrix[r][c]=resMatrix[r][c];
                    if(resMatrix[r][c]==1){
                        ESP_LOGD(TAG, "ButtonMatrix4_task: slot:%d, row:%d, col:%d", slot_num, r, c);
                        char str[2];
                        memset(str, 0, strlen(str));
				        sprintf(str, "%c",charMap[r*inSlotsCount*3+c]);
                        ESP_LOGD(TAG, "report:%s", str);
                        report(str, slot_num);
                    }
                }
                //printf("\n");
            }           
        }

    }
    

}

void start_buttonMatrix_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(buttonMatrix_task, "buttonMatrix_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-12, NULL);
    ESP_LOGD(TAG, "buttonMatrix_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
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
    uart_driver_install (uart_num, 256, 256, 0, NULL, 0);
    
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

    waitForWorkPermit(slot_num);

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
    uint8_t ena_pin = SLOTS_PIN_MAP[slot_num][1];
    uint8_t pulse_pin = SLOTS_PIN_MAP[slot_num][0];

	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

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
		enaInverse = 1;
		ESP_LOGD(TAG, "Set enaInverse:%d for slot:%d", enaInverse, slot_num);
	}

    uint8_t pulseInverse = 0;
    if (strstr(me_config.slot_options[slot_num], "pulseInverse") != NULL) {
		pulseInverse = 1;
		ESP_LOGD(TAG, "Set pulseInverse:%d for slot:%d", pulseInverse, slot_num);
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

    gpio_reset_pin(pulse_pin);
	esp_rom_gpio_pad_select_gpio(pulse_pin);
    gpio_config_t in_conf = {};
    in_conf.pull_up_en = 0;
    in_conf.pull_down_en =0;
   	in_conf.pin_bit_mask = (1ULL<<pulse_pin);
    in_conf.mode = GPIO_MODE_INPUT;
    in_conf.intr_type = pulseInverse?GPIO_INTR_POSEDGE:GPIO_INTR_NEGEDGE;
    gpio_config(&in_conf);
	gpio_set_intr_type(pulse_pin, pulseInverse?GPIO_INTR_POSEDGE:GPIO_INTR_NEGEDGE);
    //gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
	gpio_isr_handler_add(pulse_pin, gpio_isr_handler, (void*)slot_num);


    uint8_t counter=0;
    char number_str[numberMaxLenght];
    uint string_lenght=0;
    number_str[0]='\0';
    uint8_t prev_ena_state = 0;
    uint32_t dial_start_time = 0;
    uint8_t state_flag = 0;

    uint32_t tick=xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    while(1){
        //vTaskDelay();      
        
        uint8_t ena_state = gpio_get_level(ena_pin)?enaInverse:!enaInverse;
        if(ena_state!= prev_ena_state){
            prev_ena_state = ena_state;
            dial_start_time =pdTICKS_TO_MS(xTaskGetTickCount());
            if((ena_state == !enaInverse)&&(state_flag == 0)){
                state_flag = 1;
                counter = 0;
                ESP_LOGD(TAG, "Lets input number: %s strlen:%d dial_start_time:%ld", number_str, strlen(number_str), dial_start_time);
            }else if(ena_state == enaInverse){
                if(counter>=10)counter=0;
                number_str[string_lenght]=(char)counter+48;
                string_lenght++;
                number_str[string_lenght]='\0';
                ESP_LOGD(TAG, "update number_str: %s counter:%d strlen:%d", number_str, counter, strlen(number_str));
                counter = 0;
            }
        }

        if(state_flag == 1){
            if(((pdTICKS_TO_MS(xTaskGetTickCount())-dial_start_time)>=waitingTime)||(string_lenght>=numberMaxLenght)){
                if(ena_state == enaInverse){
                    ESP_LOGD(TAG, "Input end, report number: %s", number_str);
                    report(number_str, slot_num);
                    memset(number_str, 0, numberMaxLenght);
                    state_flag = 0;
                    string_lenght = 0;
                    vTaskDelay(50);
                }
            }
        }

        uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, pdMS_TO_TICKS(15)) == pdPASS){
            if(gpio_get_level(pulse_pin) == pulseInverse){
                
                // if(gpio_get_level(pulse_pin) == pulseInverse){
                    // if(debounceGap!=0){
                    //     if((xTaskGetTickCount()-tick)<debounceGap){
                    //         //ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
                    //         goto exit;
                    //     }
                    // }

                    tick = xTaskGetTickCount();
                    counter++;
                    vTaskDelay(debounceGap);
                // }
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

//-------------------academia-kick---------------------

void volnaKolya_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint16_t period = 2000;//ms
    if (strstr(me_config.slot_options[slot_num], "period") != NULL) {
		period = get_option_int_val(slot_num, "period");
		ESP_LOGD(TAG, "Set period:%d for slot:%d", period, slot_num);
	}

    uint32_t m1Dist = 10000;//ms
    if (strstr(me_config.slot_options[slot_num], "m1Dist") != NULL) {
		m1Dist = get_option_int_val(slot_num, "m1Dist");
		ESP_LOGD(TAG, "Set m1Dist:%ld for slot:%d", m1Dist, slot_num);
	}

    uint32_t m2Dist = 10000;//ms
    if (strstr(me_config.slot_options[slot_num], "m2Dist") != NULL) {
		m2Dist = get_option_int_val(slot_num, "m2Dist");
		ESP_LOGD(TAG, "Set m2Dist:%ld for slot:%d", m2Dist, slot_num);
	}
    
    uint32_t m3Dist = 10000;//ms
    if (strstr(me_config.slot_options[slot_num], "m3Dist") != NULL) {
		m3Dist = get_option_int_val(slot_num, "m3Dist");
		ESP_LOGD(TAG, "Set m3Dist:%ld for slot:%d", m3Dist, slot_num);
	}

    uint32_t startDelay = 10000;//ms
    if (strstr(me_config.slot_options[slot_num], "startDelay") != NULL) {
		startDelay = get_option_int_val(slot_num, "startDelay");
		ESP_LOGD(TAG, "Set startDelay:%ld for slot:%d", startDelay, slot_num);
	}


    //---
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "stepper_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/volna_")+3];
		sprintf(t_str, "%s/volna_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart volna topic:%s", me_state.action_topic_list[slot_num]);
	}

    //float p8q = ((float)period/8000)*((float)period/8000);
    //float p8q = ((float)period/6000)*((float)period/6000);
    float p4q = ((float)period/4000)*((float)period/4000);

    uint32_t m1Accel = 2*((float)m1Dist/2)/p4q;
    uint32_t m2Accel = 2*((float)m2Dist/2)/p4q;
    uint32_t m3Accel = 2*((float)m3Dist/2)/p4q;

    

    vTaskDelay(pdMS_TO_TICKS(startDelay));
    
    char tmpStr[124];
    sprintf(tmpStr,"moduleBox/stepper_0/setAccel:%ld",m1Accel);
    execute(tmpStr);

    sprintf(tmpStr,"moduleBox/stepper_2/setAccel:%ld",m2Accel);
    execute(tmpStr);

    sprintf(tmpStr,"moduleBox/stepper_4/setAccel:%ld",m3Accel);
    execute(tmpStr);

    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    while(1){
        //---0---
        lastWakeTime = xTaskGetTickCount();
        sprintf(tmpStr,"moduleBox/stepper_4/moveTo:%ld",m3Dist);
        execute(tmpStr);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));
        
        //---1---
        // lastWakeTime = xTaskGetTickCount();

        // vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));

        //---3---
        lastWakeTime = xTaskGetTickCount();
        sprintf(tmpStr,"moduleBox/stepper_0/moveTo:%d",0);
        execute(tmpStr);
        sprintf(tmpStr,"moduleBox/stepper_2/moveTo:%ld",m2Dist);
        execute(tmpStr);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));
        
        //---4---
        lastWakeTime = xTaskGetTickCount();
        sprintf(tmpStr,"moduleBox/stepper_4/moveTo:%d",0);
        execute(tmpStr);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));

        // //---5---
        // lastWakeTime = xTaskGetTickCount();
       
        // vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));

        //---7---
        lastWakeTime = xTaskGetTickCount();
        sprintf(tmpStr,"moduleBox/stepper_0/moveTo:%ld",m1Dist);
        execute(tmpStr);
        sprintf(tmpStr,"moduleBox/stepper_2/moveTo:%d",0);
        execute(tmpStr);
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period/4));


        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str;
            if(strstr(cmd, "stop")!=NULL){
                while(1){
                    vTaskDelay(1000);
                }
            }   
        }
    }


}


void start_volnaKolya_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(volnaKolya_task, "volnaKolya_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-18, NULL);
    ESP_LOGD(TAG, "volnaKolya_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//---------------------furbyEye----------------

void furbyEye_task(void* arg) {
    int slot_num = *(int*)arg;

    int uart_num = UART_NUM_1; // Начинаем с минимального порта
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

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
    uart_driver_install (uart_num, 256, 256, 0, NULL, 0);
    
    #define BUF_SIZE 1024
    //char data[BUF_SIZE];

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "actionTopic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/eye_0")+3];
		sprintf(t_str, "%s/eye_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    waitForWorkPermit(slot_num);

    int val;
    TickType_t lastWakeTime = xTaskGetTickCount(); 
    while(1){
        command_message_t temp_msg;
        command_message_t msg;
        uint8_t recv_state=0;

        while(xQueueReceive(me_state.command_queue[slot_num], &temp_msg, 0) == pdPASS) {
            msg = temp_msg;
            recv_state=1;
        }
        if(recv_state==1){
            ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            //ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num])+1;
            if(strstr(cmd, "setPic")!=NULL){
                val = atoi(payload);
                if(uart_write_bytes(uart_num, &val, 1)==1){
                    ESP_LOGD(TAG, "setPic to:%d", val);
                }else{
                    ESP_LOGE(TAG, "setPic failed");
                }
            }
        }
        vTaskDelayUntil(&lastWakeTime, 10);
    }
}

void start_furbyEye_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "furbyEye_task_%d", slot_num);
	xTaskCreatePinnedToCore(furbyEye_task, tmpString, 1024*8, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"furbyEye_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


