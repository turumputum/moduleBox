#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stateConfig.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include <mbdebug.h>

#include <generated_files/gen_analog.h>


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// ---------------------------------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ANALOG";


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t    MIN_VAL;
    uint16_t    MAX_VAL;
    uint8_t     inverse;
    float       k;
    uint16_t    dead_band;
    uint16_t    periodic;
    int         divider;

    char *      custom_topic;
    int         flag_custom_topic;

    int         currentReport;
    int         ratioReport;
    int         rawReport;
    int         thresholdReport;

    int         threshold;
    int         thresholdHyst;
    int         thresholdRiseLag;
    int         thresholdFallLag;
} analog_context_t;

#define ANALOG_DEFAULT() {          \
    .MIN_VAL            = 0,        \
    .MAX_VAL            = 4095,     \
    .inverse            = 0,        \
    .k                  = 1.0f,     \
    .dead_band          = 10,       \
    .periodic           = 0,        \
    .divider            = 0,        \
    .custom_topic       = NULL,     \
    .flag_custom_topic  = 0,        \
    .currentReport      = -1,       \
    .ratioReport        = -1,       \
    .rawReport          = -1,       \
    .thresholdReport    = -1,       \
    .threshold          = -1,       \
    .thresholdHyst      = 0,        \
    .thresholdRiseLag   = 0,        \
    .thresholdFallLag   = 0,        \
}

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// ---------------------------------------------------------------------------

/* 
    Старая версия поддержки ADC (для плат версии < 4)
*/
void configure_analog(analog_context_t *ctx, int slot_num)
{
    /* Флаг определяет формат выводящего значения, 
       если указан - будет выводиться значение с плавающей точкой,
       иначе - целочисленное
    */
    int flag_float_output = get_option_flag_val(slot_num, "floatOutput");
    ESP_LOGD(TAG, "S%d: float output = %d", slot_num, flag_float_output);

    /* Определяет верхний порог значений */
    ctx->MAX_VAL = get_option_int_val(slot_num, "maxVal", "", 4095, 0, 4095);
    ESP_LOGD(TAG, "S%d: max_val:%d", slot_num, ctx->MAX_VAL);

    /* Определяет нижний порог значений */
    ctx->MIN_VAL = get_option_int_val(slot_num, "minVal", "", 0, 0, 4095);
    ESP_LOGD(TAG, "S%d: min_val:%d", slot_num, ctx->MIN_VAL);

    /* Флаг задаёт инвертирование значений */
    ctx->inverse = get_option_flag_val(slot_num, "inverse");

    /* Коэфициент фильтрации */
    ctx->k = get_option_float_val(slot_num, "filterK", 1);
    ESP_LOGD(TAG, "S%d: filter k:%f", slot_num, ctx->k);
    
    /* Фильтрация дребезга - определяет порог срабатывания */
    ctx->dead_band = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4095);
    ESP_LOGD(TAG, "S%d: dead_band:%d", slot_num, ctx->dead_band);

    /* Задаёт периодичночть отсчётов в миллисекундах */
    ctx->periodic = get_option_int_val(slot_num, "periodic", "", 0, 0, 4095);
    ESP_LOGD(TAG, "S%d: periodic:%d", slot_num, ctx->periodic);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        /* Определяет топик для MQTT сообщений */
        ctx->custom_topic = get_option_string_val(slot_num, "topic", "/adc_0");
        ESP_LOGD(TAG, "S%d: custom topic:%s", slot_num, ctx->custom_topic);
        ctx->flag_custom_topic = 1;
    }

    /* Задаёт режим делителя */
    if ((ctx->divider = get_option_enum_val(slot_num, "dividerMode", "5V", "3V3", "10V", NULL)) < 0) {
        ESP_LOGE(TAG, "S%d: dividerMode: unrecognized value", slot_num);
        ctx->divider = 0;
    }

    /* Задаёт пороговое значение - при превышении рапортует 1, иначе 0
       Если задан - режим float игнорируется
    */
    ctx->threshold = get_option_int_val(slot_num, "threshold", "", -1, -1, 4095);

    /* Ширина зоны гистерезиса порога
       Для перехода в 1 значение должно превысить threshold + hysteresis/2
       Для перехода в 0 значение должно опуститься ниже threshold - hysteresis/2
    */
    ctx->thresholdHyst = get_option_int_val(slot_num, "thresholdHysteresis", "", 0, 0, 2048);

    /* Задержка подтверждения перехода в состояние 1 (мс)
       Если за это время уровень не удержался - переход игнорируется
    */
    ctx->thresholdRiseLag = get_option_int_val(slot_num, "thresholdRiseLag", "ms", 0, 0, 10000);

    /* Задержка подтверждения перехода в состояние 0 (мс)
       Если за это время уровень не удержался - переход игнорируется
    */
    ctx->thresholdFallLag = get_option_int_val(slot_num, "thresholdFallLag", "ms", 0, 0, 10000);

    if (ctx->threshold >= 0) {
        ESP_LOGD(TAG, "S%d: threshold:%d hyst:%d riseLag:%d fallLag:%d",
                 slot_num, ctx->threshold, ctx->thresholdHyst,
                 ctx->thresholdRiseLag, ctx->thresholdFallLag);
    }

    /* Возвращает текущее значение канала ввиде числа с плавающей точкой, выражающее отношение к заданной шкале
    */
    ctx->ratioReport = stdreport_register(RPTT_ratio, slot_num, "unit", "ratio", (int)ctx->MIN_VAL, (int)ctx->MAX_VAL);

    /* Возвращает текущее сырое целочисленное значение канала
    */
    ctx->rawReport = stdreport_register(RPTT_int, slot_num, "unit", "rawVal");

    /* Рапортует 0/1 при пороговом режиме
    */
    ctx->thresholdReport = stdreport_register(RPTT_int, slot_num, "bool", "threshold", 0, 1);

    if (ctx->threshold >= 0) {
        ctx->currentReport = ctx->thresholdReport;
    } else {
        ctx->currentReport = flag_float_output ? ctx->ratioReport : ctx->rawReport;
    }
}

void analog_task(void *arg)
{
    int slot_num = *(int *)arg;
    uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];

    if (slot_num == 1) {
        ESP_LOGE(TAG, "S%d: no ADC on SLOT_1, use another slot", slot_num);
        mblog(E, "no ADC on SLOT_1, use another slot");
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(NULL);
    }

    analog_context_t ctx = ANALOG_DEFAULT();

    static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
    static const adc_atten_t atten = ADC_ATTEN_DB_11;

    gpio_reset_pin(sens_pin_num);
    gpio_set_direction(sens_pin_num, GPIO_MODE_INPUT);
    adc_channel_t ADC_chan = slot_num;

    if (slot_num == 2) {
        ADC_chan = ADC2_CHANNEL_6;
        adc2_config_channel_atten(ADC_chan, atten);
    } else {
        switch (slot_num) {
        case 0: ADC_chan = ADC1_CHANNEL_3; break;
        case 3: ADC_chan = ADC1_CHANNEL_2; break;
        case 4: ADC_chan = ADC1_CHANNEL_1; break;
        case 5: ADC_chan = ADC1_CHANNEL_6; break;
        }
        adc1_config_width(width);
        adc1_config_channel_atten(ADC_chan, atten);
    }

    configure_analog(&ctx, slot_num);

    // Divider pins
    uint8_t divPin_1 = SLOTS_PIN_MAP[slot_num][2];
    esp_rom_gpio_pad_select_gpio(divPin_1);
    gpio_set_direction(divPin_1, GPIO_MODE_OUTPUT);
    uint8_t divPin_2 = SLOTS_PIN_MAP[slot_num][1];
    esp_rom_gpio_pad_select_gpio(divPin_2);
    gpio_set_direction(divPin_2, GPIO_MODE_OUTPUT);

    switch (ctx.divider) {
    case 1:
        gpio_set_level(divPin_1, 0);
        gpio_set_level(divPin_2, 0);
        ESP_LOGD(TAG, "S%d: divider mode: 3V3", slot_num);
        break;
    case 2:
        gpio_set_level(divPin_1, 0);
        gpio_set_level(divPin_2, 1);
        ESP_LOGD(TAG, "S%d: divider mode: 10V", slot_num);
        break;
    default:
        gpio_set_level(divPin_1, 1);
        gpio_set_level(divPin_2, 0);
        ESP_LOGD(TAG, "S%d: divider mode: 5V", slot_num);
        break;
    }

    // Topic setup
    if (ctx.flag_custom_topic == 0) {
        char *str = calloc(strlen(me_config.deviceName) + strlen("/analog_") + 4, sizeof(char));
        sprintf(str, "%s/analog_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = str;
    } else {
        me_state.trigger_topic_list[slot_num] = ctx.custom_topic;
    }

    uint8_t oversample = 150;
    uint16_t result = 0, prev_result = 0xFFFF;
    int threshState = 0;
    TickType_t threshPendingTick = 0;
    int threshPendingTarget = -1;  // -1 = no pending transition

    waitForWorkPermit(slot_num);

    TickType_t lastWakeTime = xTaskGetTickCount();

    // --- Initial reading and report ---
    {
        uint32_t tmp = 0;
        int raw_val = 0;
        for (int i = 0; i < oversample; i++) {
            if (slot_num == 2) {
                adc2_get_raw(ADC_chan, width, &raw_val);
            } else {
                raw_val = adc1_get_raw(ADC_chan);
            }
            if (ctx.inverse) raw_val = 4096 - raw_val;
            tmp += raw_val;
        }
        tmp /= oversample;
        result = (uint16_t)tmp;
        prev_result = result;

        if (ctx.threshold >= 0) {
            int halfHyst = ctx.thresholdHyst / 2;
            threshState = (result >= (ctx.threshold + halfHyst)) ? 1 : 0;
            stdreport_i(ctx.thresholdReport, threshState);
        } else {
            stdreport_i(ctx.currentReport, result);
        }
    }

    while (1) {
        uint32_t tmp = 0;
        int raw_val = 0;

        for (int i = 0; i < oversample; i++) {
            if (slot_num == 2) {
                adc2_get_raw(ADC_chan, width, &raw_val);
            } else {
                raw_val = adc1_get_raw(ADC_chan);
            }
            if (ctx.inverse) {
                raw_val = 4096 - raw_val;
            }
            tmp += raw_val;
        }

        tmp /= oversample;
        result = result * (1 - ctx.k) + tmp * ctx.k;
        // ESP_LOGD(TAG, "S%d: raw:%d result:%d", slot_num, raw_val, result);

        if (ctx.threshold >= 0) {
            // --- Threshold mode ---
            int halfHyst = ctx.thresholdHyst / 2;
            int riseLevel = ctx.threshold + halfHyst;
            int fallLevel = ctx.threshold - halfHyst;
            int newState = threshState;

            if (threshState == 0 && result >= riseLevel) {
                newState = 1;
            } else if (threshState == 1 && result < fallLevel) {
                newState = 0;
            }

            if (newState != threshState) {
                int lagMs = (newState == 1) ? ctx.thresholdRiseLag : ctx.thresholdFallLag;

                if (lagMs <= 0) {
                    // No lag - immediate transition
                    threshState = newState;
                    threshPendingTarget = -1;
                    stdreport_i(ctx.thresholdReport, threshState);
                } else if (threshPendingTarget != newState) {
                    // Start pending
                    threshPendingTarget = newState;
                    threshPendingTick = xTaskGetTickCount();
                }
                // else: already pending this direction, keep waiting
            } else {
                // Value returned to current state - cancel pending
                threshPendingTarget = -1;
            }

            // Check if pending lag elapsed
            if (threshPendingTarget >= 0) {
                int lagMs = (threshPendingTarget == 1) ? ctx.thresholdRiseLag : ctx.thresholdFallLag;
                if ((xTaskGetTickCount() - threshPendingTick) >= pdMS_TO_TICKS(lagMs)) {
                    // Re-check: is the condition still met?
                    int stillMet = 0;
                    if (threshPendingTarget == 1 && result >= riseLevel) stillMet = 1;
                    if (threshPendingTarget == 0 && result < fallLevel) stillMet = 1;

                    if (stillMet) {
                        threshState = threshPendingTarget;
                        stdreport_i(ctx.thresholdReport, threshState);
                    }
                    threshPendingTarget = -1;
                }
            }
        } else {
            // --- Normal (raw / ratio) mode ---
            if ((abs(result - prev_result) > ctx.dead_band) || (ctx.periodic != 0)) {
                prev_result = result;
                stdreport_i(ctx.currentReport, result);
            }
        }

        if (ctx.periodic != 0) {
            vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(ctx.periodic));
        } else {
            vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(32));
        }
    }
}
void start_analog_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;

    char taskName[32];
    sprintf(taskName, "analog_%d", slot_num);
    xTaskCreatePinnedToCore(analog_task, taskName, 1024 * 4, &t_slot_num, 12, NULL, 1);

    ESP_LOGD(TAG, "S%d: analog_task started, heap usage: %lu free: %u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_analog()
{
    return manifesto;
}
