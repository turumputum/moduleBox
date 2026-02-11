// ***************************************************************************
// TITLE: Collector Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "executor.h"
#include "me_slot_config.h"

#include <generated_files/gen_collector.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#define MAX_LINE_LENGTH 256
static const char* TAG = "COLLECTOR";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_COLLECTOR_CONFIG{
    uint8_t stringMaxLenght;
    uint16_t waitingTime;
} COLLECTOR_CONFIG, * PCOLLECTOR_CONFIG;

/* 
    Виртуальный модуль collector
    Собирает строку из поступающих сообщений
    slots: 6-9
*/
void configure_collector(PCOLLECTOR_CONFIG ch, int slot_num)
{
    /* Максимальная длина строки
       По умолчанию 7 символов
    */
    ch->stringMaxLenght = get_option_int_val(slot_num, "stringMaxLenght", "", 7, 1, 256);
    ESP_LOGD(TAG, "Set stringMaxLenght:%d for slot:%d", ch->stringMaxLenght, slot_num);

    /* Время ожидания в миллисекундах
       По умолчанию 3000 мс
    */
    ch->waitingTime = get_option_int_val(slot_num, "waitingTime", "ms", 3000, 1, 60000);
    ESP_LOGD(TAG, "Set waitingTime:%d for slot:%d", ch->waitingTime, slot_num);

    /* Не стандартный топик для collector
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/collector_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/collector_0") + 3];
        sprintf(t_str, "%s/collector_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }
}

void collector_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    COLLECTOR_CONFIG c = {0};
    configure_collector(&c, slot_num);

    uint16_t string_lenght = 0;
    char str[c.stringMaxLenght + 1];
    memset(str, 0, sizeof(str));
    uint32_t dial_start_time = 0;
    uint8_t state_flag = 0;
    
    waitForWorkPermit(slot_num);

    while(1){
        vTaskDelay(15 / portTICK_PERIOD_MS);
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            char* payload = NULL;
            char* cmd = msg.str + strlen(me_state.action_topic_list[slot_num]) + 1;
            if(strstr(cmd, ":") != NULL){
                cmd = strtok_r(cmd, ":", &payload);
            }
            
            if(strstr(cmd, "clear") != NULL){
                memset(str, 0, sizeof(str));
                string_lenght = 0;
                state_flag = 0;
                ESP_LOGD(TAG, "strUpdate:%s", str);
            } else if(strstr(cmd, "add") != NULL){
                if(payload != NULL){
                    uint8_t len = strlen(payload);
                    if(len + string_lenght > c.stringMaxLenght){
                        len = c.stringMaxLenght - string_lenght;
                    }
                    strncat(str, payload, len);
                    string_lenght += strlen(payload);
                    
                    if(state_flag == 0){
                        state_flag = 1;
                    }
                    dial_start_time = pdTICKS_TO_MS(xTaskGetTickCount());
                }
            }  
        }

        if(state_flag == 1){
            if(((pdTICKS_TO_MS(xTaskGetTickCount()) - dial_start_time) >= c.waitingTime) || (string_lenght >= c.stringMaxLenght)){
                report(str, slot_num);
                memset(str, 0, c.stringMaxLenght);
                state_flag = 0;
                string_lenght = 0;
            }
        }
    }
}

void start_collector_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "collector_task_%d", slot_num);
    xTaskCreatePinnedToCore(collector_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "collector_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_collector()
{
    return manifesto;
}
