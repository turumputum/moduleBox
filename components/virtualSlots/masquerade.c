// ***************************************************************************
// TITLE: Masquerade Module
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

#include <generated_files/gen_masquerade.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "MASQUERADE";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    MASQUERADEMCMD_set = 0,
} MASQUERADEMCMD;

typedef struct __tag_MASQUERADE_CONFIG{
    int report;
    STDCOMMANDS cmds;
} MASQUERADE_CONFIG, * PMASQUERADE_CONFIG;

/* 
    Виртуальный модуль masquerade
    Перенаправляет входящие значения в замаскированный выходной топик.
    slots: 0-9
*/
void configure_masquerade(PMASQUERADE_CONFIG ch, int slot_num)
{
    /* Входящий топик для команд masquerade
       По умолчанию deviceName/masquerade_x
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/masquerade_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/masquerade_0") + 3];
        sprintf(t_str, "%s/masquerade_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* Выходной замаскированный топик
       По умолчанию deviceName/msq_x
    */
    if (strstr(me_config.slot_options[slot_num], "mask") != NULL) {
        char* custom_out_topic = NULL;
        custom_out_topic = get_option_string_val(slot_num, "mask", "/masq_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_out_topic);
        ESP_LOGD(TAG, "Custom outTopic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/masq_0") + 3];
        sprintf(t_str, "%s/msq_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart outTopic:%s", me_state.trigger_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);

    /* Передать значение в замаскированный топик
    */
    stdcommand_register(&ch->cmds, MASQUERADEMCMD_set, "push", PARAMT_string);

    /* Отчёт значения в выходной топик
    */
    ch->report = stdreport_register(RPTT_string, slot_num, "", "");
}

void masquerade_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    MASQUERADE_CONFIG c = {0};
    configure_masquerade(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);

    while(1){
        int cmd = stdcommand_receive(&c.cmds, &params, pdMS_TO_TICKS(100));
        char * cmd_arg = (params.count > 0) ? params.p[0].p : NULL;

        switch (cmd){
            case -1: // none
                break;

            case MASQUERADEMCMD_set:
                if(cmd_arg != NULL){
                    ESP_LOGD(TAG, "Masquerade slot:%d value:%s -> %s", slot_num, cmd_arg, me_state.trigger_topic_list[slot_num]);
                    stdreport_s(c.report, cmd_arg);
                }
                break;
        }
    }
}

void start_masquerade_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "masquerade_task_%d", slot_num);
    xTaskCreatePinnedToCore(masquerade_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "masquerade_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_masquerade()
{
    return manifesto;
}
