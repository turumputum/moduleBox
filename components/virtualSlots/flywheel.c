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

typedef enum{
    FLYWHEELCMD_setCount = 0,
} FLYWHEELCMD;

typedef struct __tag_FLYWHEEL_CONFIG{
    float decrement;
    uint16_t period;
    uint16_t threshold;
    int32_t maxVal;
    int32_t minVal;
    int stateReport;
    int countReport;
    STDCOMMANDS cmds;
} FLYWHEEL_CONFIG, * PFLYWHEEL_CONFIG;

/* 
    Виртуальный модуль flywheel
    Счетчик с постепенным уменьшением значения
    slots: 0-9
*/
void configure_flywheel(PFLYWHEEL_CONFIG ch, int slot_num)
{
    /* Значение декремента за один период
       По умолчанию 0,1
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
        ESP_LOGW(TAG, "threshold not set, using default 0. Slot:%d", slot_num);
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

    // Standard topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/flywheel_0") + 3];
        sprintf(t_str, "%s/flywheel_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    /* Установка значения счетчика
       Параметр может быть задан инкрементально (+/-) или абсолютно
    */
    stdcommand_register(&ch->cmds, FLYWHEELCMD_setCount, "action/setCount", PARAMT_string);

    /* Отчёт состояния в пороговом режиме (0/1)
    */
    ch->stateReport = stdreport_register(RPTT_int, slot_num, "", "event/val", 0, 1);

    /* Отчёт значения счетчика
    */
    ch->countReport = stdreport_register(RPTT_int, slot_num, "", "event/count");

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ch->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void flywheel_task(void *arg){
    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(25, sizeof(command_message_t));

    FLYWHEEL_CONFIG c = {0};
    configure_flywheel(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    float flywheelCount = 0;
    float _flywheelCount = 0;
    uint8_t flywheel_state = 0;
    uint8_t _flywheel_state = 0;
    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    bool active_state = !get_option_flag_val(slot_num, "disableOnStart");

    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, active_state);

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
                if(active_state) stdreport_i(c.stateReport, flywheel_state);
            }
        } else {
            if((int)flywheelCount != (int)_flywheelCount){
                _flywheelCount = flywheelCount;
                if(active_state) stdreport_i(c.countReport, (int)flywheelCount);
            }
        }

        int cmd = stdcommand_receive(&c.cmds, &params, 0);
        char * cmd_arg = (params.count > 0) ? params.p[0].p : NULL;

        switch (cmd){
            case -1: // none
                break;

            case STDCMD_ENABLE:
                if (params.count > 0) {
                    active_state = params.p[0].i ? 1 : 0;
                    stdreport_enable(slot_num, active_state);
                    ESP_LOGD(TAG, "[flywheel_%d] enable:%d", slot_num, active_state);
                }
                break;

            case FLYWHEELCMD_setCount:
                if(cmd_arg != NULL){
                    if(cmd_arg[0] == '+'){
                        flywheelCount += atoi(cmd_arg + 1);
                    } else if(cmd_arg[0] == '-'){
                        flywheelCount -= atoi(cmd_arg + 1);
                    } else {
                        flywheelCount = atoi(cmd_arg);
                    }
                    if(flywheelCount > c.maxVal) flywheelCount = c.maxVal;
                    if(flywheelCount < c.minVal) flywheelCount = c.minVal;
                    ESP_LOGD(TAG, "Set flywheelCount:%f for slot:%d", flywheelCount, slot_num);
                }
                break;
        }

        vTaskDelayUntil(&lastWakeTime, c.period);
    }
}

void start_flywheel_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "flywheel_task_%d", slot_num);
    xTaskCreate(flywheel_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, 12, NULL);
    ESP_LOGD(TAG, "flywheel_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_flywheel()
{
    return manifesto;
}
