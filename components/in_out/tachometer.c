// ***************************************************************************
// TITLE: Tachometer / Frequency Meter Module
//
// PROJECT: moduleBox
// ***************************************************************************
//
// Измеряет частоту входного сигнала на одном пине через аппаратный
// счётчик импульсов PCNT. refreshRate раз в секунду считается приращение
// счётчика за окно 1/refreshRate, делится на divider и приводится к
// секунде или минуте.

#include "in_out.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include "stdcommand.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/pulse_cnt.h"

// ОБЯЗАТЕЛЬНО: Включение сгенерированного манифеста
#include <generated_files/gen_tachometer.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "TACHOMETER";

// =============================================================================
// CONSTANTS
// =============================================================================

// Front (active edge) selection
#define TACHO_FRONT_RISE    0
#define TACHO_FRONT_FALL    1
#define TACHO_FRONT_ANY     2

// Time quant for frequency normalization
#define TACHO_QUANT_SEC     0
#define TACHO_QUANT_MIN     1

// PCNT counter limits - accum_count handles wrap so высокая частота не теряется
#define TACHO_PCNT_HIGH     30000
#define TACHO_PCNT_LOW      (-30000)

// Аппаратный glitch-фильтр PCNT на 80 МГц APB тянет максимум ~12 мкс,
// поэтому debounceGap (заданный в мс) зажимается к безопасному пределу
#define TACHO_MAX_GLITCH_NS 10000

// =============================================================================
// DATA STRUCTURES
// =============================================================================

typedef struct {
    int slot_num;
    int pin;

    int front;              // TACHO_FRONT_*
    int divider;            // импульсов на оборот
    int timeQuant;          // TACHO_QUANT_SEC | TACHO_QUANT_MIN
    int refreshRate;        // частота обновления в Гц (окно = 1/refreshRate)
    int debounceGap;        // антидребезг в мс (через glitch-фильтр)
    int threshold;          // 0 = выключен
    int deadBand;           // минимальное изменение частоты для рапорта
    float k;                // коэффициент сглаживания EMA, 1 = выключен

    int active_state;       // enable lifecycle

    pcnt_unit_handle_t pcnt_unit;

    int valReport;          // event/val
    int thresholdReport;    // event/threshold
    STDCOMMANDS cmds;
} tacho_context_t;

#define TACHO_CONTEXT_DEFAULT() { \
    .slot_num = 0, \
    .pin = 0, \
    .front = TACHO_FRONT_RISE, \
    .divider = 1, \
    .timeQuant = TACHO_QUANT_SEC, \
    .refreshRate = 1000, \
    .debounceGap = 0, \
    .threshold = 0, \
    .deadBand = 0, \
    .k = 1.0f, \
    .active_state = 1, \
    .pcnt_unit = NULL, \
    .valReport = -1, \
    .thresholdReport = -1, \
}

// =============================================================================
// CONFIGURATION
// =============================================================================

/*
    Тахометр - измеритель частоты входного сигнала через PCNT. По умолчанию Гц.
    slots: 0-5
*/
static void configure_tachometer(tacho_context_t *ctx, int slot_num) {
    ctx->slot_num = slot_num;
    ctx->pin = SLOTS_PIN_MAP[slot_num][0];

    stdcommand_init(&ctx->cmds, slot_num);

    /* Если флаг поднят - модуль стартует в выключенном состоянии,
       до прихода action/enable 1 (Конституция §6)
    */
    ctx->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "Initial active_state:%d for slot:%d", ctx->active_state, slot_num);

    /* Активный фронт сигнала: rise (восходящий), fall (нисходящий), any (оба)
    */
    ctx->front = get_option_enum_val(slot_num, "front", "rise", "fall", "any", NULL);
    ESP_LOGD(TAG, "Set front:%d for slot:%d", ctx->front, slot_num);

    /* Делитель - сколько импульсов приходится на один оборот, целое 1-65535
    */
    ctx->divider = get_option_int_val(slot_num, "divider", "", 1, 1, 65535);
    ESP_LOGD(TAG, "Set divider:%d for slot:%d", ctx->divider, slot_num);

    /* Квант времени: sec (частота в герцах) или min (обороты в минуту)
    */
    ctx->timeQuant = get_option_enum_val(slot_num, "timeQuant", "sec", "min", NULL);
    ESP_LOGD(TAG, "Set timeQuant:%d for slot:%d", ctx->timeQuant, slot_num);

    /* Частота обновления в герцах - сколько раз в секунду пересчитываем частоту
       и рапортуем при изменении. Окно подсчёта импульсов равно периоду 1/refreshRate
    */
    ctx->refreshRate = get_option_int_val(slot_num, "refreshRate", "Hz", 10, 1, 100);
    ESP_LOGD(TAG, "Set refreshRate:%d Hz for slot:%d", ctx->refreshRate, slot_num);

    /* Антидребезг в миллисекундах - по умолчанию 0 (выключен)
       Реализован аппаратным glitch-фильтром PCNT и зажимается к ~10 мкс
    */
    ctx->debounceGap = get_option_int_val(slot_num, "debounceGap", "ms", 0, 0, 4096);
    ESP_LOGD(TAG, "Set debounceGap:%d ms for slot:%d", ctx->debounceGap, slot_num);

    /* Минимальное изменение частоты для отправки рапорта - подавляет дрожание
       По умолчанию 0 - рапортуем при любом изменении
    */
    ctx->deadBand = get_option_int_val(slot_num, "deadBand", "", 0, 0, 1000000);
    ESP_LOGD(TAG, "Set deadBand:%d for slot:%d", ctx->deadBand, slot_num);

    /* Коэффициент сглаживания от 0 до 1 - чем меньше тем сильнее сглаживание
       При значении 1 фильтр выключен
    */
    ctx->k = get_option_float_val(slot_num, "filterK", 1.0f);
    ESP_LOGD(TAG, "Set filterK:%f for slot:%d", ctx->k, slot_num);

    /* Пороговое значение частоты - если задано (больше 0), модуль дополнительно
       рапортует threshold 1 когда частота выше порога и 0 когда ниже
    */
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        ctx->threshold = get_option_int_val(slot_num, "threshold", "", 0, 0, 1000000);
        ESP_LOGD(TAG, "Set threshold:%d for slot:%d", ctx->threshold, slot_num);
    }

    // Setup topic - база <deviceName>/tachometer_<slot>
    // trigger - для исходящих event, action - чтобы executor доставлял команды (executor.c:252)
    {
        char t_str[strlen(me_config.deviceName) + strlen("/tachometer_0") + 3];
        sprintf(t_str, "%s/tachometer_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard topic base:%s", me_state.trigger_topic_list[slot_num]);
    }

    // =============================================================================
    // REPORTS
    // =============================================================================

    /* Рапортует текущую частоту - целое значение 0-uint32
    */
    ctx->valReport = stdreport_register(RPTT_int, slot_num, "", "event/val");

    /* Рапортует состояние порога 0/1 при заданной опции threshold
    */
    ctx->thresholdReport = stdreport_register(RPTT_int, slot_num, "", "event/threshold", 0, 1);

    // =============================================================================
    // COMMANDS
    // =============================================================================

    /* Включить (1) или выключить (0) модуль (Конституция §6) */
    stdcommand_register(&ctx->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    // =============================================================================
    // EVENTS
    // =============================================================================

    /* Состояние модуля - активен (1) или спит (0). Retained */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

// =============================================================================
// PCNT SETUP
// =============================================================================

// Возвращает 0 при успехе, -1 при ошибке (нет свободного PCNT-юнита)
static int tachometer_pcnt_setup(tacho_context_t *ctx) {
    pcnt_unit_config_t unit_config = {
        .high_limit = TACHO_PCNT_HIGH,
        .low_limit = TACHO_PCNT_LOW,
        .flags.accum_count = true,
    };
    if (pcnt_new_unit(&unit_config, &ctx->pcnt_unit) != ESP_OK) {
        ESP_LOGE(TAG, "PCNT unit limit reached, slot:%d", ctx->slot_num);
        return -1;
    }

    // accum_count накапливает значение при достижении вотч-поинтов на лимитах
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(ctx->pcnt_unit, TACHO_PCNT_HIGH));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(ctx->pcnt_unit, TACHO_PCNT_LOW));

    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = ctx->pin,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t pcnt_chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(ctx->pcnt_unit, &chan_config, &pcnt_chan));

    // Выбор фронта через edge actions: pos_edge / neg_edge
    pcnt_channel_edge_action_t pos_act = PCNT_CHANNEL_EDGE_ACTION_HOLD;
    pcnt_channel_edge_action_t neg_act = PCNT_CHANNEL_EDGE_ACTION_HOLD;
    switch (ctx->front) {
        case TACHO_FRONT_RISE:
            pos_act = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
            break;
        case TACHO_FRONT_FALL:
            neg_act = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
            break;
        case TACHO_FRONT_ANY:
            pos_act = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
            neg_act = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
            break;
    }
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, pos_act, neg_act));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));

    // Антидребезг через аппаратный glitch-фильтр (мс -> нс с зажимом)
    if (ctx->debounceGap > 0) {
        uint32_t glitch_ns = (uint32_t)ctx->debounceGap * 1000000UL;
        if (glitch_ns > TACHO_MAX_GLITCH_NS) {
            ESP_LOGW(TAG, "debounceGap %d ms exceeds PCNT filter limit, clamped to %d ns slot:%d",
                     ctx->debounceGap, TACHO_MAX_GLITCH_NS, ctx->slot_num);
            glitch_ns = TACHO_MAX_GLITCH_NS;
        }
        ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(ctx->pcnt_unit,
            &(pcnt_glitch_filter_config_t){ .max_glitch_ns = glitch_ns }));
    }

    ESP_ERROR_CHECK(pcnt_unit_enable(ctx->pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(ctx->pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(ctx->pcnt_unit));
    return 0;
}

// =============================================================================
// TASK
// =============================================================================

static void tachometer_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;
    tacho_context_t ctx = TACHO_CONTEXT_DEFAULT();

    configure_tachometer(&ctx, slot_num);

    if (tachometer_pcnt_setup(&ctx) != 0) {
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(NULL);
        return;
    }

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    // Период обновления = окно подсчёта импульсов (мс), из refreshRate в Гц
    uint32_t period_ms = 1000UL / (uint32_t)ctx.refreshRate;
    if (period_ms < 1) period_ms = 1;

    // Множитель приведения: импульсы/мс -> импульсы/сек(мин)
    uint32_t scale = (ctx.timeQuant == TACHO_QUANT_MIN) ? 60000UL : 1000UL;

    int last_count = 0;
    uint32_t lastFreq = 0;
    int lastThreshold = 0;
    int firstReport = 1;
    int firstThreshold = 1;
    float filtered = 0.0f;      // состояние EMA-фильтра
    int filterSeeded = 0;       // фильтр ещё не инициализирован первым отсчётом

    STDCOMMAND_PARAMS params = {0};
    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    // Стартовое состояние enable (retained)
    stdreport_enable(slot_num, ctx.active_state);

    while (1) {
        // --- Обработка команд (включая action/enable) ---
        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        if (cmd == STDCMD_ENABLE && params.count > 0) {
            int newState = params.p[0].i ? 1 : 0;
            if (newState != ctx.active_state) {
                ctx.active_state = newState;
                ESP_LOGD(TAG, "enable:%d slot:%d", ctx.active_state, slot_num);
                stdreport_enable(slot_num, ctx.active_state);
                if (ctx.active_state) {
                    // сбрасываем базу счёта чтобы не получить скачок частоты
                    pcnt_unit_get_count(ctx.pcnt_unit, &last_count);
                    firstReport = 1;
                    firstThreshold = 1;
                    filterSeeded = 0;
                }
            }
        }

        if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(period_ms)) == pdFALSE) {
            lastWakeTime = xTaskGetTickCount();
        }

        int current_count = 0;
        ESP_ERROR_CHECK(pcnt_unit_get_count(ctx.pcnt_unit, &current_count));

        if (!ctx.active_state) {
            // держим базу синхронной, рапорты не шлём
            last_count = current_count;
            continue;
        }

        uint32_t pulses = (uint32_t)(current_count - last_count);
        last_count = current_count;

        // freq = pulses * scale / (divider * period_ms), с округлением
        uint64_t num = (uint64_t)pulses * scale;
        uint64_t den = (uint64_t)ctx.divider * period_ms;
        uint32_t freq = (den > 0) ? (uint32_t)((num + den / 2) / den) : 0;

        // --- сглаживание EMA: filtered = filtered*(1-k) + freq*k ---
        if (ctx.k < 1.0f) {
            if (!filterSeeded) {
                filtered = (float)freq;     // затравка первым отсчётом без разгона от нуля
                filterSeeded = 1;
            } else {
                filtered = filtered * (1.0f - ctx.k) + (float)freq * ctx.k;
            }
            freq = (uint32_t)(filtered + 0.5f);
        }

        // --- val: рапорт при изменении больше deadBand ---
        uint32_t diff = (freq > lastFreq) ? (freq - lastFreq) : (lastFreq - freq);
        if (firstReport || diff > (uint32_t)ctx.deadBand) {
            stdreport_i(ctx.valReport, (int)freq);
            lastFreq = freq;
            firstReport = 0;
        }

        // --- threshold: простое сравнение, рапорт при изменении ---
        if (ctx.threshold > 0) {
            int th = (freq >= (uint32_t)ctx.threshold) ? 1 : 0;
            if (firstThreshold || th != lastThreshold) {
                stdreport_i(ctx.thresholdReport, th);
                lastThreshold = th;
                firstThreshold = 0;
            }
        }
    }
}

void start_tachometer_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_tachometer_%d", slot_num);
    xTaskCreate(tachometer_task, tmpString, 1024 * 4, (void*)(intptr_t)slot_num,
                configMAX_PRIORITIES - 10, NULL);

    ESP_LOGD(TAG, "tachometer_task created for slot:%d Heap usage:%lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

// =============================================================================
// MANIFEST EXPORT
// =============================================================================

const char * get_manifest_tachometer()
{
    return manifesto;
}
