// ***************************************************************************
// TITLE
//      Incremental Encoder Module (PCNT)
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "reporter.h"
#include "stateConfig.h"
#include "driver/pcnt.h"
#include "driver/pulse_cnt.h"
#include "esp_system.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include "stdcommand.h"
#include <mbdebug.h>

#include <generated_files/gen_encoder_inc.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ENCODER_INC";

#define INCREMENTAL 0
#define ABSOLUTE 1

typedef enum {
    INCCMD_reset = 0,
} INCCMD;

typedef struct __tag_INCCONFIG
{
    uint8_t                 mode;
    uint8_t                 dirInverse;
    uint8_t                 linearCounter;
    int                     active_state;
    int16_t                 zeroShift;
    int32_t                 minVal;
    int32_t                 maxVal;
    uint16_t                refreshPeriod;
    int                     pole;
    uint16_t                divider;
    uint16_t                glitchFilter;

    int                     report;

    STDCOMMANDS             cmds;
} INCCONFIG, * PINCCONFIG;

/*
    Инкрементальный энкодер (обычно оптический) на периферии PCNT
    ESP32-S3 имеет всего 4 PCNT unit на encoderInc + tachometer + stepper
    slots: 0-3
*/
void configure_encoderInc(PINCCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Флаг - стартовать в выключенном состоянии до прихода action/enable 1 */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[encoder_%d] initial active_state:%d", slot_num, c->active_state);

    /* Флаг - абсолютный режим - По умолчанию инкрементальный */
    c->mode = get_option_flag_val(slot_num, "absolute");
    ESP_LOGD(TAG, "[encoder_%d] mode:%s", slot_num, c->mode ? "absolute" : "incremental");

    /* Флаг - инверсия направления счета */
    c->dirInverse = get_option_flag_val(slot_num, "dirInverse");
    ESP_LOGD(TAG, "[encoder_%d] dirInverse:%d", slot_num, c->dirInverse);

    /* Флаг - линейный счетчик (стоп на краях) вместо зацикленного - По умолчанию циклический */
    c->linearCounter = get_option_flag_val(slot_num, "linearCounter");
    ESP_LOGD(TAG, "[encoder_%d] counter:%s", slot_num, c->linearCounter ? "linear" : "circular");

    /* Аппаратный фильтр дребезга в наносекундах - По умолчанию 800 */
    c->glitchFilter = get_option_int_val(slot_num, "glitchFilter", "ns", 800, 1, 4095);
    ESP_LOGD(TAG, "[encoder_%d] glitchFilter:%d ns", slot_num, c->glitchFilter);

    /* Делитель импульсов на один шаг позиции - По умолчанию 1 */
    c->divider = get_option_int_val(slot_num, "divider", "", 1, 1, UINT16_MAX);
    ESP_LOGD(TAG, "[encoder_%d] divider:%d", slot_num, c->divider);

    /* Смещение нуля после делителя - По умолчанию 0 */
    c->zeroShift = get_option_int_val(slot_num, "zeroShift", "", 0, INT16_MIN, INT16_MAX);
    ESP_LOGD(TAG, "[encoder_%d] zeroShift:%d", slot_num, c->zeroShift);

    /* Минимальное значение позиции - По умолчанию 0 */
    c->minVal = get_option_int_val(slot_num, "minVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[encoder_%d] minVal:%ld", slot_num, c->minVal);

    /* Максимальное значение позиции - По умолчанию 4096 */
    c->maxVal = get_option_int_val(slot_num, "maxVal", "", 4096, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "[encoder_%d] maxVal:%ld", slot_num, c->maxVal);

    /* Период опроса значений в Гц - По умолчанию 20, максимум 100 */
    c->refreshPeriod = 1000 / (get_option_int_val(slot_num, "refreshRate", "fps", 20, 1, 100));
    ESP_LOGD(TAG, "[encoder_%d] refreshPeriod:%d", slot_num, c->refreshPeriod);

    c->pole = (c->maxVal - c->minVal) + 1;

    {
        char t_str[strlen(me_config.deviceName) + strlen("/encoder_0") + 3];
        sprintf(t_str, "%s/encoder_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === REPORTS === */

    /* Текущее значение позиции (абс) или приращение (инкр) */
    c->report = stdreport_register(RPTT_string, slot_num, "", "event/val");

    /* Состояние модуля - активен 1 или спит 0 */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");

    /* === COMMANDS === */

    /* Обнулить счетчик */
    stdcommand_register(&c->cmds, INCCMD_reset, "action/reset", PARAMT_none);

    /* Включить 1 или выключить 0 модуль */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);
}

void encoder_inc_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;

    INCCONFIG c = {0};
    configure_encoderInc(&c, slot_num);

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t a_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t b_pin_num = SLOTS_PIN_MAP[slot_num][1];

    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
        .flags.accum_count = true, // accumulate the counter value
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    esp_err_t err = pcnt_new_unit(&unit_config, &pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCNT unit limit reached (slot:%d), task terminated. err:%d",
                 slot_num, err);
        vTaskDelete(NULL);
    }

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = c.glitchFilter,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = a_pin_num,
        .level_gpio_num = b_pin_num,
    };

    pcnt_channel_handle_t pcnt_chan_a = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_a_config, &pcnt_chan_a));

    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = b_pin_num,
        .level_gpio_num = a_pin_num,
    };
    pcnt_channel_handle_t pcnt_chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_b_config, &pcnt_chan_b));

    if (!c.dirInverse) {
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    } else {
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
        ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    }
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));
    pcnt_unit_add_watch_point(pcnt_unit, INT16_MAX);
    pcnt_unit_add_watch_point(pcnt_unit, INT16_MIN);

    int rawVal = 0;            // pcnt count - pcnt_unit_get_count expects int*
    int32_t prewRawVal = 0;    // pcnt count in int16 MAX-MIN range
    int32_t count = 0;         // global accumulated count
    int32_t pos = 0;           // after divider
    int32_t prev_pos = INT32_MIN;

    STDCOMMAND_PARAMS params = {0};
    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c.active_state);

    while (1)
    {
        // --- Обработка команд (включая action/enable и action/reset) ---
        int cmd = stdcommand_receive(&c.cmds, &params, 0);
        if (cmd == INCCMD_reset) {
            pcnt_unit_clear_count(pcnt_unit);
        } else if (cmd == STDCMD_ENABLE) {
            if (params.count > 0) {
                c.active_state = params.p[0].i ? 1 : 0;
                ESP_LOGD(TAG, "enable:%d slot:%d", c.active_state, slot_num);
                stdreport_enable(slot_num, c.active_state);
            }
        }

        err = pcnt_unit_get_count(pcnt_unit, &rawVal);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pcnt_get_counter_value failed: %d", err);
            return;
        }

        int32_t delta = rawVal - prewRawVal;
        if (abs(delta) > (INT16_MAX / 2)) {
            // Произошло переполнение
            if (delta > 0) {
                // переполнение в отрицательной зоне
                delta = rawVal - (prewRawVal - INT16_MIN);
            } else {
                delta = rawVal - (prewRawVal - INT16_MAX);
            }
        }

        if (c.dirInverse) {
            count += delta;
        } else {
            count -= delta;
        }
        prewRawVal = rawVal;

        pos = count / c.divider;
        pos += c.zeroShift;

        if (c.linearCounter) {
            if (pos < c.minVal) {
                pos = c.minVal;
                count = c.minVal;
            } else if (pos > c.maxVal) {
                pos = c.maxVal;
                count = c.maxVal * c.divider;
            }
        } else {
            while (pos < c.minVal) {
                pos = pos + c.pole;
            }
            while (pos > c.maxVal) {
                pos = pos - c.pole;
            }
        }

        if (pos != prev_pos) {
            // Счет идет всегда - рапорт только когда модуль активен
            if (c.active_state) {
                char str[40];
                if (c.mode == ABSOLUTE) {
                    sprintf(str, "%ld", pos);
                } else {
                    int32_t diff = pos - prev_pos;
                    sprintf(str, "%ld", diff);
                }
                stdreport_s(c.report, str);
            }
            prev_pos = pos;
        }

        vTaskDelayUntil(&lastWakeTime, c.refreshPeriod);
    }
}

void start_encoder_inc_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(encoder_inc_task, "task_encInc", 1024 * 4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 10, NULL);
    ESP_LOGD(TAG, "encoder_inc_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_encoder_inc()
{
    return manifesto;
}
