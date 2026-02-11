// ***************************************************************************
// TITLE: Flywheel Module
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
#include "me_slot_config.h"
 
#include <generated_files/gen_flywheel.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "FLYWHEEL";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_FLYWHEEL_CONFIG{
    float decrement;
    uint16_t period;
    uint16_t threshold;
    int32_t maxVal;
    int32_t minVal;
} FLYWHEEL_CONFIG, * PFLYWHEEL_CONFIG;

/* 
    Виртуальный модуль flywheel
    Счетчик с постепенным уменьшением значения
    slots: 6-9
*/
void configure_flywheel(PFLYWHEEL_CONFIG ch, int slot_num)
{
    /* Значение декремента за один период
       По умолчанию 0.1
    */
    ch->decrement = get_option_float_val(slot_num, "decrement", 0.1);
    ESP_LOGD(TAG, "Set decrement:%f for slot:%d", ch->decrement, slot_num);

    /* Период обновления в миллисекундах
       По умолчанию 100 мс
    */
    ch->period = get_option_int_val(slot_num, "period", "ms", 100, 1, 10000);
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", ch->period, slot_num);

    /* Пороговое значение
       По умолчанию 0
    */
    ch->threshold = get_option_int_val(slot_num, "threshold", "", 0, 0, 4096);
    if (ch->threshold <= 0){
        ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
        ch->threshold = 0;
    } else {
        ESP_LOGD(TAG, "threshold:%d. Slot:%d", ch->threshold, slot_num);
    }

    /* Максимальное значение
       По умолчанию 20
    */
    ch->maxVal = get_option_int_val(slot_num, "maxVal", "", 20, 0, 10000);
    ESP_LOGD(TAG, "Set max_counter:%ld for slot:%d", ch->maxVal, slot_num);

    /* Минимальное значение
       По умолчанию 0
    */
    ch->minVal = get_option_int_val(slot_num, "minVal", "", 0, 0, 10000);
    ESP_LOGD(TAG, "Set min_counter:%ld for slot:%d", ch->minVal, slot_num);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/flywheel_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/flywheel_0") + 3];
        sprintf(t_str, "%s/flywheel_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }
}

void flywheel_task(void *arg){
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(25, sizeof(command_message_t));

    FLYWHEEL_CONFIG c = {0};
    configure_flywheel(&c, slot_num);

    float flywheelCount = 0;
    float _flywheelCount = 0;
    uint8_t flywheel_state = 0;
    uint8_t _flywheel_state = 0;

    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    for(;;) {
        flywheelCount -= c.decrement;
        if(flywheelCount < c.minVal){
            flywheelCount = c.minVal;
        }

        if(c.threshold > 0){
            if(flywheelCount > c.threshold){
                flywheel_state = 1;
            } else {
                flywheel_state = 0;
            }

            if(flywheel_state != _flywheel_state){
                _flywheel_state = flywheel_state;
                char str[3];
                memset(str, 0, sizeof(str));
                sprintf(str, "%d", flywheel_state);
                report(str, slot_num);
            }
        } else {
            if((int)flywheelCount != (int)_flywheelCount){
                _flywheelCount = flywheelCount;
                char str[10];
                memset(str, 0, sizeof(str));
                sprintf(str, "/count:%d", (int)flywheelCount);
                report(str, slot_num);
            }
        }

        // Проверка команд
        command_message_t msg;
        while(xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS) {
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]) + 1;
            
            if(strstr(cmd, "setCount") != NULL){
                if(strstr(payload, "+") != NULL){
                    flywheelCount += atoi(payload);
                } else if(strstr(payload, "-") != NULL){
                    flywheelCount -= atoi(payload);
                } else {
                    flywheelCount = atoi(payload);
                }
                if(flywheelCount > c.maxVal) flywheelCount = c.maxVal;
                if(flywheelCount < c.minVal) flywheelCount = c.minVal;
                ESP_LOGD(TAG, "Set flywheelCount:%f for slot:%d", flywheelCount, slot_num);
            }
        }
        vTaskDelayUntil(&lastWakeTime, c.period);
    }
}

void start_flywheel_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "flywheel_task_%d", slot_num);
    xTaskCreate(flywheel_task, tmpString, 1024*4, &t_slot_num, 12, NULL);
    ESP_LOGD(TAG, "flywheel_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_flywheel()
{
    return manifesto;
}
