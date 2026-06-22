// ***************************************************************************
// TITLE: Scaler Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "me_slot_config.h"

#include <generated_files/gen_scaler.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "SCALER";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    SCALERCMD_set = 0,
} SCALERCMD;

typedef struct __tag_SCALER_CONFIG{
    int16_t zeroDeadZone;
    int32_t inputMinVal;
    int32_t inputMaxVal;
    int32_t outputMinVal;
    int32_t outputMaxVal;
    int report;
    STDCOMMANDS cmds;
} SCALER_CONFIG, * PSCALER_CONFIG;

/* 
    Виртуальный модуль scaler
    Масштабирует входные значения в заданный диапазон
    slots: 0-9
*/
void configure_scaler(PSCALER_CONFIG ch, int slot_num)
{
    /* Мертвая зона вокруг нуля
       По умолчанию 0
    */
    ch->zeroDeadZone = get_option_int_val(slot_num, "zeroDeadZone", "", 0, 0, 4096);
    ESP_LOGD(TAG, "Set zeroDeadZone:%d for slot:%d", ch->zeroDeadZone, slot_num);

    /* Минимальное входное значение
       По умолчанию 0
    */
    ch->inputMinVal = get_option_int_val(slot_num, "inputMinVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set inputMinVal:%ld for slot:%d", ch->inputMinVal, slot_num);
    
    /* Максимальное входное значение
       По умолчанию 255
    */
    ch->inputMaxVal = get_option_int_val(slot_num, "inputMaxVal", "", 255, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set inputMaxVal:%ld for slot:%d", ch->inputMaxVal, slot_num);

    /* Минимальное выходное значение
       По умолчанию 0
    */
    ch->outputMinVal = get_option_int_val(slot_num, "outputMinVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set outputMinVal:%ld for slot:%d", ch->outputMinVal, slot_num);

    /* Максимальное выходное значение
       По умолчанию 255
    */
    ch->outputMaxVal = get_option_int_val(slot_num, "outputMaxVal", "", 255, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set outputMaxVal:%ld for slot:%d", ch->outputMaxVal, slot_num);

    {
        char t_str[strlen(me_config.deviceName) + strlen("/scaler_0") + 3];
        sprintf(t_str, "%s/scaler_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    /* Установить входное значение для масштабирования
    */
    stdcommand_register(&ch->cmds, SCALERCMD_set, "action/pushVal", PARAMT_int);

    /* Отчёт масштабированного значения
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "event/result");

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ch->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void scaler_task(void* arg) {
    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    SCALER_CONFIG c = {0};
    configure_scaler(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};
    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    bool active_state = !get_option_flag_val(slot_num, "disableOnStart");

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, active_state);

    while(1){
        int cmd = stdcommand_receive(&c.cmds, &params, portMAX_DELAY);

        switch (cmd){
            case -1: // none
                break;

            case STDCMD_ENABLE:
                if (params.count > 0) {
                    active_state = params.p[0].i ? 1 : 0;
                    stdreport_enable(slot_num, active_state);
                    ESP_LOGD(TAG, "[scaler_%d] enable:%d", slot_num, active_state);
                }
                break;

            case SCALERCMD_set:
            {
                if(!active_state) break;
                int32_t inputVal = params.p[0].i;
                
                if(inputVal < c.inputMinVal){
                    inputVal = c.inputMinVal;
                } else if(inputVal > c.inputMaxVal){
                    inputVal = c.inputMaxVal;
                }
                
                float inputFloat = (float)(inputVal - c.inputMinVal) / (c.inputMaxVal - c.inputMinVal);
                float outputVal = inputFloat * (c.outputMaxVal - c.outputMinVal) + c.outputMinVal;
                
                if(outputVal < c.outputMinVal){
                    outputVal = c.outputMinVal;
                } else if(outputVal > c.outputMaxVal){
                    outputVal = c.outputMaxVal;
                }
                
                if(abs((int)outputVal) < c.zeroDeadZone){
                    outputVal = 0;
                }
                
                stdreport_i(c.report, (int32_t)outputVal);
                break;
            }
        }
    }
}

void start_scaler_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "scaler_task_%d", slot_num);
    xTaskCreatePinnedToCore(scaler_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "scaler_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_scaler()
{
    return manifesto;
}
