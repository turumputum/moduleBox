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

#include <generated_files/gen_tankControl.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "TANKCONTROL";

extern configuration me_config;
extern stateStruct me_state;

typedef enum {
    TANK_CMD_accel = 0,
    TANK_CMD_steering,
} TANK_CMD;

typedef struct __tag_TANKCONTROL_CONFIG{
    uint16_t deadBand;
    uint16_t inputMinVal;
    uint16_t inputMaxVal;
    uint16_t outputMaxVal;
    int active_state;
    int leftReport;
    int rightReport;
    STDCOMMANDS cmds;
} TANKCONTROL_CONFIG, * PTANKCONTROL_CONFIG;

/*
    Виртуальный tankControl - микширует газ и руль в две гусеницы
    Команды accel и steering на входе, скорости ch_0 (лево) и ch_1 (право) на выходе
    slots: 6-9
*/
void configure_tankControl(PTANKCONTROL_CONFIG ch, int slot_num)
{
    stdcommand_init(&ch->cmds, slot_num);

    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    ch->active_state = !get_option_flag_val(slot_num, "disableOnStart");

    /* Мертвая зона газа, По умолчанию 10
    */
    ch->deadBand = get_option_int_val(slot_num, "deadBand", "", 10, 0, 4096);
    ESP_LOGD(TAG, "Set deadBand:%d for slot:%d", ch->deadBand, slot_num);

    /* Минимальное входное значение, По умолчанию 0
    */
    ch->inputMinVal = get_option_int_val(slot_num, "inputMinVal", "", 0, 0, 4096);
    ESP_LOGD(TAG, "Set inputMinVal:%d for slot:%d", ch->inputMinVal, slot_num);

    /* Максимальное входное значение, По умолчанию 255
    */
    ch->inputMaxVal = get_option_int_val(slot_num, "inputMaxVal", "", 255, 0, 4096);
    ESP_LOGD(TAG, "Set inputMaxVal:%d for slot:%d", ch->inputMaxVal, slot_num);

    /* Максимальное выходное значение, По умолчанию 255
    */
    ch->outputMaxVal = get_option_int_val(slot_num, "outputMaxVal", "", 255, 0, 4096);
    ESP_LOGD(TAG, "Set outputMaxVal:%d for slot:%d", ch->outputMaxVal, slot_num);

    // Standard topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/tankControl_0") + 3];
        sprintf(t_str, "%s/tankControl_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* === EVENTS === */

    /* Скорость левой гусеницы
    */
    ch->leftReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_0");

    /* Скорость правой гусеницы
    */
    ch->rightReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_1");

    /* === COMMANDS === */

    /* Газ - входное значение
    */
    stdcommand_register(&ch->cmds, TANK_CMD_accel, "action/accel", PARAMT_int);

    /* Руль - входное значение
    */
    stdcommand_register(&ch->cmds, TANK_CMD_steering, "action/steering", PARAMT_int);

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ch->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void tankControl_task(void* arg) {
    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    TANKCONTROL_CONFIG c = {0};
    configure_tankControl(&c, slot_num);

    uint16_t inputMidlVal = (c.inputMaxVal - c.inputMinVal) / 2 + c.inputMinVal;
    ESP_LOGD(TAG, "Set inputMidlVal:%d for slot:%d", inputMidlVal, slot_num);

    int accel = inputMidlVal;
    int steering = inputMidlVal;

    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c.active_state);

    while(1){
        int cmd = stdcommand_receive(&c.cmds, &params, portMAX_DELAY);
        if (cmd < 0) continue;

        if (cmd == STDCMD_ENABLE){
            if (params.count > 0){
                c.active_state = params.p[0].i ? 1 : 0;
                ESP_LOGD(TAG, "[tankControl_%d] enable:%d", slot_num, c.active_state);
                stdreport_enable(slot_num, c.active_state);
            }
            continue;
        }

        if (!c.active_state) continue;

        if (cmd == TANK_CMD_accel){
            accel = params.p[0].i;
            if(accel > c.inputMaxVal) accel = c.inputMaxVal;
            if(accel < c.inputMinVal) accel = c.inputMinVal;
            if(abs(accel - inputMidlVal) < c.deadBand) accel = inputMidlVal;
        } else if (cmd == TANK_CMD_steering){
            steering = params.p[0].i;
            if(steering > c.inputMaxVal) steering = c.inputMaxVal;
            if(steering < c.inputMinVal) steering = c.inputMinVal;
        }

        int midSteering = steering - inputMidlVal;
        int midAccel = accel - inputMidlVal;

        float ratio = (float)c.outputMaxVal / c.inputMaxVal;
        int leftSpeed = (int)(((midAccel + midSteering) + inputMidlVal) * ratio);
        int rightSpeed = (int)(((midAccel - midSteering) + inputMidlVal) * ratio);

        stdreport_i(c.leftReport, leftSpeed);
        stdreport_i(c.rightReport, rightSpeed);
    }
}

void start_tankControl_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "tankControl_task_%d", slot_num);
    xTaskCreatePinnedToCore(tankControl_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "tankControl_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_tankControl()
{
    return manifesto;
}
