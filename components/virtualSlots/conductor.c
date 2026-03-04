// ***************************************************************************
// TITLE: Stepper Conductor Module
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
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "me_slot_config.h"

#include <generated_files/gen_conductor.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "CONDUCTOR";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    CONDUCTORCMD_currentPos = 0,
    CONDUCTORCMD_targetPos,
    CONDUCTORCMD_stop,
} CONDUCTORCMD;

typedef struct __tag_CONDUCTOR_CONFIG{
    int32_t minVal;
    int32_t maxVal;
    uint8_t multiTurnKinematics;
    uint32_t timeout;
    STDCOMMANDS cmds;
    int stopReport;
    int runUpReport;
    int runDownReport;
} CONDUCTOR_CONFIG, * PCONDUCTOR_CONFIG;

/* 
    Виртуальный модуль stepper_conductor
    Управляет шаговым двигателем с позиционированием
    slots: 0-9
*/
void configure_conductor(PCONDUCTOR_CONFIG ch, int slot_num)
{
    /* Минимальное значение позиции
       По умолчанию 0
    */
    ch->minVal = get_option_int_val(slot_num, "minVal", "", 0, 0, INT32_MAX);
    ESP_LOGD(TAG, "Set minVal:%ld for slot:%d", ch->minVal, slot_num);

    /* Максимальное значение позиции
       По умолчанию 32767
    */
    ch->maxVal = get_option_int_val(slot_num, "maxVal", "", 32767, 0, INT32_MAX);
    ESP_LOGD(TAG, "Set maxVal:%ld for slot:%d", ch->maxVal, slot_num);

    /* Многооборотная кинематика
       0-1 по умолчанию 0
    */
    ch->multiTurnKinematics = get_option_int_val(slot_num, "multiTurn", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set multiTurnKinematics:%d for slot:%d", ch->multiTurnKinematics, slot_num);

    /* Таймаут в миллисекундах (0 = без таймаута)
       По умолчанию 0
    */
    ch->timeout = get_option_int_val(slot_num, "timeout", "ms", 0, 0, UINT32_MAX);
    ESP_LOGD(TAG, "Set timeout:%lu ms for slot:%d", ch->timeout, slot_num);

    /* Не стандартный топик для conductor
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/conductor_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/conductor_0") + 3];
        sprintf(t_str, "%s/conductor_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);

    /* Обновление текущей позиции от энкодера
    */
    stdcommand_register(&ch->cmds, CONDUCTORCMD_currentPos, "currentPos", PARAMT_int);

    /* Задать целевую позицию
    */
    stdcommand_register(&ch->cmds, CONDUCTORCMD_targetPos, "targetPos", PARAMT_int);

    /* Остановить двигатель
    */
    stdcommand_register(&ch->cmds, CONDUCTORCMD_stop, "stop", PARAMT_none);

    /* Отчёт остановки двигателя
    */
    ch->stopReport = stdreport_register(RPTT_string, slot_num, "", "stop");

    /* Отчёт движения вверх
    */
    ch->runUpReport = stdreport_register(RPTT_string, slot_num, "", "runUp");

    /* Отчёт движения вниз
    */
    ch->runDownReport = stdreport_register(RPTT_string, slot_num, "", "runDown");
}

void conductor_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    CONDUCTOR_CONFIG c = {0};
    configure_conductor(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    int32_t turnSize = c.maxVal - c.minVal;

    int32_t currentPosition = 0;
    int32_t targetPosition = 0;
    int8_t isMoving = 0;
    int64_t lastCommandTime = 0;
    
    waitForWorkPermit(slot_num);

    while (1) {
        // Проверка таймаута
        if (isMoving && c.timeout > 0) {
            int64_t currentTime = esp_timer_get_time() / 1000;
            if (currentTime - lastCommandTime > c.timeout) {
                ESP_LOGD(TAG, "Timeout reached, stopping motor");
                stdreport_s(c.stopReport, "");
                lastCommandTime = esp_timer_get_time() / 1000;
                isMoving = 0;
            }
        }
        
        int cmd = stdcommand_receive(&c.cmds, &params, isMoving ? 10 : portMAX_DELAY);

        switch (cmd) {
            case -1: // none
                break;

            case CONDUCTORCMD_currentPos:
                currentPosition = params.p[0].i;
                
                // Проверка достижения целевой позиции
                if (isMoving && currentPosition == targetPosition) {
                    stdreport_s(c.stopReport, "");
                    lastCommandTime = esp_timer_get_time() / 1000;
                    isMoving = 0;
                    ESP_LOGD(TAG, "Target position reached: %ld", currentPosition);
                }
                break;

            case CONDUCTORCMD_targetPos:
            {
                int32_t newTargetPos = params.p[0].i;
                
                // Ограничение целевой позиции
                if (newTargetPos < c.minVal) {
                    newTargetPos = c.minVal;
                    ESP_LOGW(TAG, "Target position limited to min: %ld", c.minVal);
                } else if (newTargetPos > c.maxVal) {
                    newTargetPos = c.maxVal;
                    ESP_LOGW(TAG, "Target position limited to max: %ld", c.maxVal);
                }
                
                targetPosition = newTargetPos;
                
                // Расчет направления движения
                if (currentPosition != targetPosition) {
                    int16_t direction = 0;
                    
                    if (c.multiTurnKinematics && turnSize > 0) {
                        // Расчет кратчайшего пути для многооборотной кинематики
                        int32_t directPath = targetPosition - currentPosition;
                        int32_t wrapAroundPath;
                        
                        if (directPath > 0) {
                            wrapAroundPath = directPath - turnSize;
                        } else {
                            wrapAroundPath = directPath + turnSize;
                        }
                        
                        // Выбор пути с наименьшим абсолютным значением
                        if (abs(wrapAroundPath) < abs(directPath)) {
                            direction = (wrapAroundPath > 0) ? 1 : -1;
                        } else {
                            direction = (directPath > 0) ? 1 : -1;
                        }
                        
                        ESP_LOGD(TAG, "Multi-turn path calculation: direct=%ld, wrapAround=%ld, chosen direction=%d", 
                                directPath, wrapAroundPath, direction);
                    } else {
                        // Простой расчет направления для линейного движения
                        direction = (targetPosition > currentPosition) ? 1 : -1;
                    }
                    
                    // Отправка команды движения
                    if (direction > 0) {
                        stdreport_s(c.runUpReport, "");
                        ESP_LOGD(TAG, "Moving up to target: %ld from current: %ld", targetPosition, currentPosition);
                    } else {
                        stdreport_s(c.runDownReport, "");
                        ESP_LOGD(TAG, "Moving down to target: %ld from current: %ld", targetPosition, currentPosition);
                    }
                    
                    isMoving = 1;
                    lastCommandTime = esp_timer_get_time() / 1000;
                }
                break;
            }

            case CONDUCTORCMD_stop:
                stdreport_s(c.stopReport, "");
                lastCommandTime = esp_timer_get_time() / 1000;
                isMoving = 0;
                ESP_LOGD(TAG, "Stop command received");
                break;
        }
    }
}

void start_conductor_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "conductor_task_%d", slot_num);
    xTaskCreatePinnedToCore(conductor_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "conductor_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_conductor()
{
    return manifesto;
}
