// ***************************************************************************
// TITLE: Watchdog Module
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
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "executor.h"
#include "me_slot_config.h"

#include <generated_files/gen_watchdog.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "WATCHDOG";

extern configuration me_config;
extern stateStruct me_state;

typedef enum {
    WD_CMD_reset = 0,
} WD_CMD;

typedef struct __tag_WATCHDOG_CONFIG{
    uint32_t time;
    STDCOMMANDS cmds;
} WATCHDOG_CONFIG, * PWATCHDOG_CONFIG;

/*
    Виртуальный watchdog - перезагружает устройство если его не сбрасывать
    Команда reset перезапускает отсчёт, иначе по истечении time будет перезагрузка
    slots: 0-9
*/
void configure_watchdog(PWATCHDOG_CONFIG ch, int slot_num)
{
    stdcommand_init(&ch->cmds, slot_num);

    /* Время до перезагрузки в секундах, По умолчанию 3600
    */
    ch->time = get_option_int_val(slot_num, "time", "s", 3600, 1, INT32_MAX);
    ESP_LOGD(TAG, "Set time:%ld for slot:%d", ch->time, slot_num);

    // Standard topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/watchdog_0") + 3];
        sprintf(t_str, "%s/watchdog_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === COMMANDS === */

    /* Сбросить отсчёт - перезапускает таймер до перезагрузки
    */
    stdcommand_register(&ch->cmds, WD_CMD_reset, "action/reset", PARAMT_none);
}

static void IRAM_ATTR watchdog_isr_handler(void* arg){
    int slot_num = (int) arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void watchdog_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;

    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    WATCHDOG_CONFIG c = {0};
    configure_watchdog(&c, slot_num);

    esp_timer_handle_t virtual_timer = NULL;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &watchdog_isr_handler,
        .arg = (void*)slot_num,
        .name = "watchdog_timer"
    };
    esp_timer_create(&delay_timer_args, &virtual_timer);

    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);

    // Запускаем отсчёт сразу при старте
    esp_timer_start_once(virtual_timer, (uint64_t)c.time * 1000 * 1000);
    ESP_LOGD(TAG, "Watchdog armed on time:%ld s", c.time);

    while(1){
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
            ESP_LOGD(TAG, "%ld :: watchdog reset", xTaskGetTickCount());
            vTaskDelay(1000);
            esp_restart();
        }

        int cmd = stdcommand_receive(&c.cmds, &params, 0);
        if (cmd == WD_CMD_reset){
            if(esp_timer_is_active(virtual_timer)){
                esp_timer_stop(virtual_timer);
            }
            esp_timer_start_once(virtual_timer, (uint64_t)c.time * 1000 * 1000);
            ESP_LOGD(TAG, "Watchdog timer restarted on time:%ld", c.time);
        }
    }
}

void start_watchdog_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "watchdog_task_%d", slot_num);
    xTaskCreatePinnedToCore(watchdog_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "watchdog_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_watchdog()
{
    return manifesto;
}
