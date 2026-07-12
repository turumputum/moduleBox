// ***************************************************************************
// TITLE: Random Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include "reporter.h"
#include <stdcommand.h>
#include <stdreport.h>
#include "me_slot_config.h"
 
#include <generated_files/gen_random.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "RANDOM";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    RNDCMD_gen = 0,
} RNDCMD;

typedef struct __tag_RND_CONFIG{
    int32_t minVal;
    int32_t maxVal;
    int report;
    STDCOMMANDS cmds;
} RND_CONFIG, * PRND_CONFIG;

/*
    Модуль для генерации случайных чисел
    slots: 0-9
*/
void configure_random(PRND_CONFIG ch, int slot_num){
    
    /* Максимальное значение
       По умолчанию INT32_MAX
    */
    ch->maxVal = get_option_int_val(slot_num, "maxVal", "", INT32_MAX, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set maxVal :%ld for slot:%d", ch->maxVal, slot_num);

    /* Минимальное значение
       По умолчанию 0
    */
    ch->minVal = get_option_int_val(slot_num, "minVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set minVal :%ld for slot:%d", ch->minVal, slot_num);

    // Standard topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/random_0") + 3];
        sprintf(t_str, "%s/random_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    /* Сгенерировать число. Без параметра - в диапазоне minVal-maxVal из опций;
       одно число N - в диапазоне 0-N; два числа через пробел - в этом диапазоне
    */
    stdcommand_register(&ch->cmds, RNDCMD_gen, "action/generate", PARAMT_none);

    /* Возвращает сгенерированное значение
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "event/val", (int)ch->minVal, (int)ch->maxVal);

    /* === COMMANDS === */

    /* Включить (1) или выключить (0) модуль (Конституция §6). */
    stdcommand_register(&ch->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void random_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    RND_CONFIG c = {0};
    configure_random(&c, slot_num);
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
                    ESP_LOGD(TAG, "[random_%d] enable:%d", slot_num, active_state);
                    stdreport_enable(slot_num, active_state);
                }
                break;

            case RNDCMD_gen: {
                if(!active_state) break;

                // Диапазон выбирается по нагрузке команды:
                //   без параметра  -> minVal-maxVal из опций
                //   одно число N   -> 0-N
                //   два числа A B  -> A-B
                int32_t lo, hi;
                if (params.count >= 2) {
                    int32_t a = atoi(params.p[0].p);
                    int32_t b = atoi(params.p[1].p);
                    lo = a < b ? a : b;
                    hi = a < b ? b : a;
                } else if (params.count == 1) {
                    int32_t n = atoi(params.p[0].p);
                    lo = n < 0 ? n : 0;
                    hi = n < 0 ? 0 : n;
                } else {
                    lo = c.minVal;
                    hi = c.maxVal;
                }

                // span в int64 - защита от переполнения при широких диапазонах
                int64_t span = (int64_t)hi - (int64_t)lo + 1;
                int32_t val = (int32_t)(lo + (rand() % span));
                ESP_LOGD(TAG, "Gen:%ld range:%ld-%ld argc:%d", (long)val, (long)lo, (long)hi, params.count);
                stdreport_i(c.report, val);
                break;
            }
        }
    }
}

void start_random_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "random_task_%d", slot_num);
    xTaskCreatePinnedToCore(random_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "random_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_random()
{
    return manifesto;
}
