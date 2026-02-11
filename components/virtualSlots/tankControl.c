// ***************************************************************************
// TITLE: Tank Control Module
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

// #include <generated_files/gen_tankControl.h>
// const char * get_manifest_tankControl()
// {
//     return manifesto;
// }

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "TANKCONTROL";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_TANKCONTROL_CONFIG{
    uint16_t deadBand;
    uint16_t inputMinVal;
    uint16_t inputMaxVal;
    uint16_t outputMaxVal;
} TANKCONTROL_CONFIG, * PTANKCONTROL_CONFIG;

/* 
    Виртуальный модуль tankControl
    Преобразует управление газ/руль в управление гусеничной машиной
    slots: 6-9
*/
void configure_tankControl(PTANKCONTROL_CONFIG ch, int slot_num)
{
    /* Мертвая зона
       По умолчанию 10
    */
    ch->deadBand = get_option_int_val(slot_num, "deadBand", "", 10, 0, 4096);
    ESP_LOGD(TAG, "Set deadBand:%d for slot:%d", ch->deadBand, slot_num);

    /* Минимальное входное значение
       По умолчанию 0
    */
    ch->inputMinVal = get_option_int_val(slot_num, "inputMinVal", "", 0, 0, 4096);
    ESP_LOGD(TAG, "Set inputMinVal:%d for slot:%d", ch->inputMinVal, slot_num);
    
    /* Максимальное входное значение
       По умолчанию 255
    */
    ch->inputMaxVal = get_option_int_val(slot_num, "inputMaxVal", "", 255, 0, 4096);
    ESP_LOGD(TAG, "Set inputMaxVal:%d for slot:%d", ch->inputMaxVal, slot_num);

    /* Максимальное выходное значение
       По умолчанию 255
    */
    ch->outputMaxVal = get_option_int_val(slot_num, "outputMaxVal", "", 255, 0, 4096);
    ESP_LOGD(TAG, "Set outputMaxVal:%d for slot:%d", ch->outputMaxVal, slot_num);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/tankControl_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/tankControl_0") + 3];
        sprintf(t_str, "%s/tankControl_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }
}

void tankControl_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(50, sizeof(command_message_t));

    TANKCONTROL_CONFIG c = {0};
    configure_tankControl(&c, slot_num);

    uint16_t inputMidlVal = (c.inputMaxVal - c.inputMinVal) / 2 + c.inputMinVal;
    ESP_LOGD(TAG, "Set inputMidlVal:%d for slot:%d", inputMidlVal, slot_num);

    uint16_t accel = c.inputMaxVal / 2;
    uint16_t steering = c.inputMaxVal / 2;

    waitForWorkPermit(slot_num);

    while(1){
        command_message_t cmd;
        if (xQueueReceive(me_state.command_queue[slot_num], &cmd, portMAX_DELAY) == pdPASS){
            char *command = cmd.str + strlen(me_state.action_topic_list[slot_num]) + 1;
            if(strstr(command, ":") == NULL){
                ESP_LOGE(TAG, "No arguments found. EXIT"); 
            } else {
                char *cmd_arg = strstr(command, ":") + 1;
                
                if(!memcmp(command, "accel", 5)){
                    accel = strtol(cmd_arg, NULL, 10);
                    if(accel > c.inputMaxVal) accel = c.inputMaxVal;
                    if(accel < c.inputMinVal) accel = c.inputMinVal;
                    if(abs(accel - inputMidlVal) < c.deadBand) accel = inputMidlVal;
                } else if(!memcmp(command, "steering", 8)){
                    steering = strtol(cmd_arg, NULL, 10);
                    if(steering > c.inputMaxVal) steering = c.inputMaxVal;
                    if(steering < c.inputMinVal) steering = c.inputMinVal;
                }
                
                int16_t midSteering = steering - inputMidlVal;
                int16_t midAccel = accel - inputMidlVal;

                float ratio = (float)c.outputMaxVal / c.inputMaxVal;
                int leftSpeed = (int)((midAccel + midSteering) + inputMidlVal) * ratio;
                int rightSpeed = (int)((midAccel - midSteering) + inputMidlVal) * ratio;
                
                char str[50];
                sprintf(str, "/ch_0:%d", leftSpeed);
                report(str, slot_num);
                memset(str, 0, sizeof(str));
                sprintf(str, "/ch_1:%d", rightSpeed);
                report(str, slot_num);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void start_tankControl_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "tankControl_task_%d", slot_num);
    xTaskCreatePinnedToCore(tankControl_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "tankControl_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


