// ***************************************************************************
// TITLE: Counter Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include "reporter.h"
#include "executor.h"
#include <stdcommand.h>
#include <stdreport.h>
#include "me_slot_config.h"
 
#include <generated_files/gen_counter.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "COUNTER";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    COUNTERCMD_set = 0,
} COUNTERCMD;

typedef struct __tag_COUNTER_CONFIG{
    int32_t minVal;
    int32_t maxVal;
    int32_t threshold;
    int circularCounter;
    int report;
    STDCOMMANDS cmds;
} COUNTER_CONFIG, * PCOUNTER_CONFIG;

/* 
    Виртуальный модуль счетчик
    slots: 0-9
*/
void configure_counter(PCOUNTER_CONFIG ch, int slot_num){
    
    /* Максимальное значение счетчика
       По умолчанию INT32_MAX
    */
    ch->maxVal = get_option_int_val(slot_num, "maxVal", "", INT32_MAX, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set maxVal :%ld for slot:%d", ch->maxVal, slot_num);

    /* Минимальное значение счетчика
       По умолчанию 0
    */
    ch->minVal = get_option_int_val(slot_num, "minVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set minVal :%ld for slot:%d", ch->minVal, slot_num);

    /* Пороговое значение счетчика
       По умолчанию 0
    */
    ch->threshold = get_option_int_val(slot_num, "threshold", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set threshold :%ld for slot:%d", ch->threshold, slot_num);

    /* Сброс к противоположному значению при переполнении счетчика
       0-1 по умолчанию 0
    */
    ch->circularCounter = get_option_flag_val(slot_num, "circularCounter");
    ESP_LOGD(TAG, "Set circularCounter :%d for slot:%d", ch->circularCounter, slot_num);

    /* Не стандартный топик для счетчика
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/counter_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "customTopic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/counter_0") + 3];
        sprintf(t_str, "%s/counter_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    
    /* Установка значений счетчика
       Параметр может быть задан инкрементально или абсолютно
    */
    stdcommand_register(&ch->cmds, COUNTERCMD_set, "set", PARAMT_string);

    /* Отчёт значения счетчика
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "");
}

void counter_task(void *arg) {
    int slot_num = *(int*) arg;
    int32_t counter = 0;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    COUNTER_CONFIG c = {0};
    configure_counter(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    int32_t state = 0;
    int32_t prevState = INT32_MIN;

    waitForWorkPermit(slot_num);

    while(1){
        if(c.threshold > 0){
            if(counter >= c.threshold){
                state = 1;
            } else {
                state = 0;
            }
        } else {
            state = counter;
        }

        if(state != prevState){
            prevState = state;
            stdreport_i(c.report, state);
            ESP_LOGD(TAG, "Counter report:%ld", state);
        }

        int cmd = stdcommand_receive(&c.cmds, &params, portMAX_DELAY);
        char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";
        
        switch (cmd){
            case -1: // none
                break;

            case COUNTERCMD_set: 
                if(cmd_arg[0] == '+'){
                    counter += atoi(cmd_arg + 1);
                } else if(cmd_arg[0] == '-'){
                    counter -= atoi(cmd_arg + 1);
                } else {
                    counter = atoi(cmd_arg);
                }

                if(c.circularCounter){
                    if(counter < c.minVal){
                        counter = c.maxVal;
                    } else if(counter > c.maxVal){
                        counter = c.minVal;
                    }
                } else {
                    if(counter < c.minVal){
                        counter = c.minVal;
                    } else if(counter > c.maxVal){
                        counter = c.maxVal;
                    }
                }
                break;
        }
    }
}

void start_counter_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "counter_task_%d", slot_num);
    xTaskCreatePinnedToCore(counter_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "counter_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_counter()
{
    return manifesto;
}
