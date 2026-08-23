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
#include <math.h>
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
    SCALERCMD_set = 0,      /* pushVal - устаревшее имя целого входа */
    SCALERCMD_pushInt,
    SCALERCMD_pushFloat,
} SCALERCMD;

typedef struct __tag_SCALER_CONFIG{
    int16_t zeroDeadZone;
    int32_t inputMinVal;
    int32_t inputMaxVal;
    int32_t outputMinVal;
    int32_t outputMaxVal;
    uint8_t floatOutput;    /* 1 - event/result дробный   */
    int report;             /* event/result               */
    STDCOMMANDS cmds;
} SCALER_CONFIG, * PSCALER_CONFIG;

/* Отдать целый результат. Тип отчёта выбран конфигурацией: при дробном
   выходе stdreport_i сам напечатает 42-0, а целое значение по дороге не
   портится - в отличие от прогона int32 через float. */
static void scaler_publishInt(PSCALER_CONFIG c, int32_t value)
{
    if (c->report < 0)
        return;

    stdreport_i(c->report, value);
}

/* Отдать дробный результат. При целом выходе округляем сами: stdreport_f на
   целочисленном отчёте приводит значение усечением и теряет почти единицу. */
static void scaler_publishFloat(PSCALER_CONFIG c, float value)
{
    if (c->report < 0)
        return;

    if (c->floatOutput)
        stdreport_f(c->report, value);
    else
        stdreport_i(c->report, (int32_t)lroundf(value));
}

/* Целочисленный тракт. Промежуток считается в int64: произведение двух
   разностей по int32 переполняет 32 бита уже на диапазонах в пару тысяч.
   В отличие от дробного тракта точен на всём диапазоне int32 - у float
   мантисса 24 бита, и выше 16 миллионов значения начинают округляться. */
static int32_t scaler_calcInt(PSCALER_CONFIG c, int32_t inputVal)
{
    int32_t inLow  = (c->inputMinVal < c->inputMaxVal) ? c->inputMinVal : c->inputMaxVal;
    int32_t inHigh = (c->inputMinVal < c->inputMaxVal) ? c->inputMaxVal : c->inputMinVal;

    if (inputVal < inLow)  inputVal = inLow;
    if (inputVal > inHigh) inputVal = inHigh;

    int64_t span = (int64_t)c->inputMaxVal - (int64_t)c->inputMinVal;

    /* Схлопнутый входной диапазон - делить не на что. */
    if (span == 0)
        return c->outputMinVal;

    int64_t num = ((int64_t)inputVal - (int64_t)c->inputMinVal) *
                  ((int64_t)c->outputMaxVal - (int64_t)c->outputMinVal);

    /* Округление к ближайшему, половина от нуля. Считаем по модулю и вешаем
       знак в конце: поправка со знаком прямо на num врёт при отрицательном
       делителе, а он появляется при инвертированной входной шкале
       (inputMinVal больше inputMaxVal) и уводит весь диапазон на единицу. */
    int      negative = ((num < 0) != (span < 0));
    int64_t  absNum   = (num  < 0) ? -num  : num;
    int64_t  absSpan  = (span < 0) ? -span : span;
    int64_t  quotient = (absNum + absSpan / 2) / absSpan;

    if (negative)
        quotient = -quotient;

    int64_t out = (int64_t)c->outputMinVal + quotient;

    int32_t outLow  = (c->outputMinVal < c->outputMaxVal) ? c->outputMinVal : c->outputMaxVal;
    int32_t outHigh = (c->outputMinVal < c->outputMaxVal) ? c->outputMaxVal : c->outputMinVal;

    if (out < outLow)  out = outLow;
    if (out > outHigh) out = outHigh;

    if (llabs(out) < (int64_t)c->zeroDeadZone)
        out = 0;

    return (int32_t)out;
}

/* Дробный тракт. Нужен там, где важна дробная часть входа или выхода. */
static float scaler_calcFloat(PSCALER_CONFIG c, float inputVal)
{
    float inLow  = (c->inputMinVal < c->inputMaxVal) ? (float)c->inputMinVal : (float)c->inputMaxVal;
    float inHigh = (c->inputMinVal < c->inputMaxVal) ? (float)c->inputMaxVal : (float)c->inputMinVal;

    if (inputVal < inLow)  inputVal = inLow;
    if (inputVal > inHigh) inputVal = inHigh;

    float span = (float)c->inputMaxVal - (float)c->inputMinVal;

    if (span == 0.0f)
        return (float)c->outputMinVal;

    float ratio = (inputVal - (float)c->inputMinVal) / span;
    float out   = ratio * ((float)c->outputMaxVal - (float)c->outputMinVal) + (float)c->outputMinVal;

    float outLow  = (c->outputMinVal < c->outputMaxVal) ? (float)c->outputMinVal : (float)c->outputMaxVal;
    float outHigh = (c->outputMinVal < c->outputMaxVal) ? (float)c->outputMaxVal : (float)c->outputMinVal;

    if (out < outLow)  out = outLow;
    if (out > outHigh) out = outHigh;

    if (fabsf(out) < (float)c->zeroDeadZone)
        out = 0.0f;

    return out;
}

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
    ch->inputMinVal = get_option_int_val(slot_num, "inputMinVal", "", 0, -2147483647, 2147483647);
    ESP_LOGD(TAG, "Set inputMinVal:%ld for slot:%d", ch->inputMinVal, slot_num);
    
    /* Максимальное входное значение
       По умолчанию 255
    */
    ch->inputMaxVal = get_option_int_val(slot_num, "inputMaxVal", "", 255, -2147483647, 2147483647);
    ESP_LOGD(TAG, "Set inputMaxVal:%ld for slot:%d", ch->inputMaxVal, slot_num);

    /* Минимальное выходное значение
       По умолчанию 0
    */
    ch->outputMinVal = get_option_int_val(slot_num, "outputMinVal", "", 0, -2147483647, 2147483647);
    ESP_LOGD(TAG, "Set outputMinVal:%ld for slot:%d", ch->outputMinVal, slot_num);

    /* Максимальное выходное значение
       По умолчанию 255
    */
    ch->outputMaxVal = get_option_int_val(slot_num, "outputMaxVal", "", 255, -2147483647, 2147483647);
    ESP_LOGD(TAG, "Set outputMaxVal:%ld for slot:%d", ch->outputMaxVal, slot_num);

    /* Отдавать результат целым числом - режим по умолчанию
    */
    int intOutput = get_option_flag_val(slot_num, "intOutput");

    /* Отдавать результат с дробной частью, иначе результат округляется до целого
    */
    ch->floatOutput = get_option_flag_val(slot_num, "floatOutput");

    if (intOutput && ch->floatOutput)
    {
        ESP_LOGE(TAG, "[scaler_%d] intOutput and floatOutput are both set, using int", slot_num);
        ch->floatOutput = 0;
    }

    ESP_LOGD(TAG, "Set output type:%s for slot:%d", ch->floatOutput ? "float" : "int", slot_num);

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

    /* Целое входное значение - счёт в целых числах, точен на всём диапазоне int32, ответ в event/result
    */
    stdcommand_register(&ch->cmds, SCALERCMD_pushInt, "action/pushInt", PARAMT_int);

    /* Дробное входное значение - счёт в плавающей точке, при флаге floatOutput дробная часть сохраняется
    */
    stdcommand_register(&ch->cmds, SCALERCMD_pushFloat, "action/pushFloat", PARAMT_float);

    /* Масштабированное значение - целое, либо дробное при флаге floatOutput
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "event/result");

    /* Тип отчёта задаётся конфигурацией, а регистрация остаётся одна: два
       объявления одного топика манифест показал бы как два разных отчёта. */
    if (ch->floatOutput)
        stdreport_setType(ch->report, RPTT_float);

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
            case SCALERCMD_pushInt:
            {
                if(!active_state) break;
                if(params.count == 0) break;

                int32_t out = scaler_calcInt(&c, params.p[0].i);

                scaler_publishInt(&c, out);

                ESP_LOGD(TAG, "[scaler_%d] pushInt:%ld -> %ld", slot_num, params.p[0].i, (long)out);
                break;
            }

            case SCALERCMD_pushFloat:
            {
                if(!active_state) break;
                if(params.count == 0) break;

                /* Параметр объявлен float, но целое тоже принимается - из
                   кросслинка значения приходят целыми. */
                float in = (params.p[0].type == PARAMT_float)
                           ? params.p[0].f
                           : (float)params.p[0].i;

                float out = scaler_calcFloat(&c, in);

                scaler_publishFloat(&c, out);

                ESP_LOGD(TAG, "[scaler_%d] pushFloat:%.3f -> %.3f", slot_num, in, out);
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
