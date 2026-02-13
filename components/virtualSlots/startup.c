// ***************************************************************************
// TITLE: Startup Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "me_slot_config.h"

#include <generated_files/gen_startup.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "STARTUP";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_STARTUP_CONFIG{
    int delay;
    int startReport;
} STARTUP_CONFIG, * PSTARTUP_CONFIG;

/* 
    Модуль отправки сообщения при запуске
*/
void configure_startup(PSTARTUP_CONFIG ch, int slot_num)
{
    /* Параметр задержки перед отправкой сообщения о запуске (мс)
       Числовое значение 0-4096, по умолчанию 0
    */
    ch->delay = get_option_int_val(slot_num, "delay", "", 0, 0, 4096);
    ESP_LOGD(TAG, "Set startup delay:%d. Slot:%d", ch->delay , slot_num);
    
    /* Пользовательский топик для отправки сообщения о запуске
       По умолчанию используется стандартный топик deviceName/startup_slotNum
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/startup_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "customTopic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/startup_0") + 3];
        sprintf(t_str, "%s/startup_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* Рапортует при старте
    Используется для инициализации начального состояния
    */
    ch->startReport = stdreport_register(RPTT_int, slot_num, "bool", "started", 0, 1);
}

void startup_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    STARTUP_CONFIG ch;
    configure_startup(&ch, slot_num);

    waitForWorkPermit(slot_num);

    vTaskDelay(pdMS_TO_TICKS(ch.delay));

    // Отправка сообщения о запуске
    stdreport_i(ch.startReport, 1);

    vTaskDelete(NULL);
}

void start_startup_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "startup_task_%d", slot_num);
    xTaskCreatePinnedToCore(startup_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "startup_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_startup()
{
    return manifesto;
}
