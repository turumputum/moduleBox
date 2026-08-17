// ***************************************************************************
// TITLE
//     Аналоговый ввод (ADC)
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc/adc_continuous.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"
#include <stdreport.h>
#include <mbdebug.h>
#include "analog.h"

#include <generated_files/gen_analog.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL     ESP_LOG_DEBUG

// Режимы делителя. Значения - позиции в списке get_option_enum_val ниже
// ("5V", "3V3", "10V"), менять их можно только вместе с этим списком.
// MODE_5V обязан быть нулём: get_option_enum_val возвращает 0 и когда опция
// dividerMode вообще не задана, то есть ноль - это дефолт.
#define MODE_5V             0
#define MODE_3V3            1
#define MODE_10V            2

// Число отсчётов, усредняемых перед фильтром. Одинаково для обоих путей.
#define ANALOG_OVERSAMPLE   150

// Период опроса легаси-пути, когда periodic не задан, мс
#define ANALOG_IDLE_PERIOD  32

#define ADC_READ_LEN        (64 * SOC_ADC_DIGI_DATA_BYTES_PER_CONV)

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define ADC_GET_CHANNEL(p_data)     ((p_data)->type1.channel)
#define ADC_GET_DATA(p_data)        ((p_data)->type1.data)
#else
#define ADC_GET_CHANNEL(p_data)     ((p_data)->type2.channel)
#define ADC_GET_DATA(p_data)        ((p_data)->type2.data)
#endif


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_ANALOG_CHANNEL
{
    // --- поля непрерывного ADC (легаси-путь их не трогает) ---
    bool                    used;
    int                     pattern_num;
    TaskHandle_t            s_task_handle;
    adc_continuous_evt_cbs_t cb;
    int                     slot_num;

    int                     averagingCount;
    uint64_t                averagingSum;

    // --- общее состояние ---
    uint16_t                result;
    uint16_t                prev_result;

    uint16_t                MIN_VAL;
    uint16_t                MAX_VAL;
    uint8_t                 inverse;
    float                   k;
    uint16_t                dead_band;
    uint16_t                periodic;

    int                     divider;

    int                     currentReport;
    int                     ratioReport;
    int                     rawReport;
    int                     thresholdReport;

    int                     threshold;
    int                     thresholdHyst;
    int                     thresholdRiseLag;
    int                     thresholdFallLag;

    int                     threshState;
    TickType_t              threshPendingTick;
    int                     threshPendingTarget;

    TickType_t              lastReportTime;

} ANALOG_CHANNEL, * PANALOG_CHANNEL;


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t              SLOTS_PIN_MAP       [10][4];
extern configuration        me_config;
extern stateStruct          me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *         TAG                 = "ANALOG";

static SemaphoreHandle_t    startSemaphore      = NULL;

static ANALOG_CHANNEL       analog_channels     [ NUM_OF_SLOTS ]            = { 0 };

// --- состояние непрерывного ADC1 ---

static adc_continuous_handle_cfg_t adc1_config =
        {
            .max_store_buf_size = 1024,
            .conv_frame_size    = ADC_READ_LEN
        };

adc_continuous_handle_t     adc1_handle         = NULL;
static bool                 started             = false;

static
adc_continuous_config_t     adc1_dig_cfg =
    {
        .pattern_num        = 0,
        .adc_pattern        = 0,
        .sample_freq_hz     = 20 * 1000,
        .conv_mode          = ADC_CONV_SINGLE_UNIT_1,
        .format             = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

static
adc_digi_pattern_config_t   adc1_pattern        [ SOC_ADC_PATT_LEN_MAX ]    = { 0 };

// Обратное отображение канал->слот, заполняется в рантайме.
// Индекс - номер канала ADC1 (0..9), значение - индекс слота либо -1.
static int channel_to_slot[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

// Единственная задача, остающаяся на дежурстве и читающая данные ADC. Её будит ISR.
static TaskHandle_t s_duty_task_handle = NULL;


// ---------------------------------------------------------------------------
// -------------------------- CONTINUOUS ADC1 (v4+) --------------------------
// -----------------|---------------------------(|------------------|---------

// Для ESP32-S3: каналы ADC1 0-9 соответствуют GPIO 1-10.
static int gpio_to_adc1_channel(uint8_t gpio)
{
    if (gpio >= 1 && gpio <= 10)
        return (int)(gpio - 1);
    return -1;
}

static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    if (s_duty_task_handle)
        vTaskNotifyGiveFromISR(s_duty_task_handle, &mustYield);
    return (mustYield == pdTRUE);
}

static bool adc1_init()
{
    bool result = true;

    if (adc1_handle == NULL)
    {
        ESP_ERROR_CHECK(adc_continuous_new_handle(&adc1_config, &adc1_handle));

        if (adc1_handle == NULL)
        {
            result = false;
        }
    }

    return result;
}

static bool adc1_start_and_there_can_be_only_one(void)
{
    bool result = false;

    if ((adc1_handle != NULL) && !started)
    {
        if( xSemaphoreTake(startSemaphore, portMAX_DELAY) == pdTRUE)
        {
            if (!started)
            {
                // Хэндл дежурной задачи ставим ДО старта - ISR может сработать сразу.
                s_duty_task_handle = xTaskGetCurrentTaskHandle();

                adc1_dig_cfg.adc_pattern = adc1_pattern;

                if (adc_continuous_config(adc1_handle, &adc1_dig_cfg) == ESP_OK)
                {
                    if (adc_continuous_start(adc1_handle) == ESP_OK)
                    {
                        result = started = true;
                    }
                }
                else
                    ESP_LOGE(TAG, "ADC configuration failed!");
            }

            xSemaphoreGive(startSemaphore);
        }
    }

    return result;
}

static bool adc1_add_channel(PANALOG_CHANNEL ch)
{
    bool result = false;

    int channel = gpio_to_adc1_channel(SLOTS_PIN_MAP[ch->slot_num][0]);

    if (channel < 0)
    {
        ESP_LOGE(TAG, "S%d: GPIO %d has no ADC1 channel", ch->slot_num, SLOTS_PIN_MAP[ch->slot_num][0]);
        return false;
    }

    if( xSemaphoreTake(startSemaphore, portMAX_DELAY) == pdTRUE)
    {
        if (!ch->used)
        {
            ch->s_task_handle = xTaskGetCurrentTaskHandle();
            ch->cb.on_conv_done = s_conv_done_cb;
            ch->pattern_num = adc1_dig_cfg.pattern_num;

            adc_digi_pattern_config_t * p = &adc1_pattern[ch->pattern_num];

            p->atten        = ADC_ATTEN_DB_12;
            p->channel      = (adc_channel_t)channel;
            p->unit         = ADC_UNIT_1;
            p->bit_width    = SOC_ADC_DIGI_MAX_BITWIDTH;

            // Добавляем CB только для первого потока, он и будет рабочим.
            if (!adc1_dig_cfg.pattern_num)
            {
                ESP_LOGD(TAG, "ADC CB was set on slot %d (GPIO %d, CH %d)", ch->slot_num, SLOTS_PIN_MAP[ch->slot_num][0], channel);

                if (adc_continuous_register_event_callbacks(adc1_handle, &ch->cb, ch) == ESP_OK)
                {
                    result = true;
                }
            }
            else
                result = true;

            if (result)
            {
                channel_to_slot[channel] = ch->slot_num;
                adc1_dig_cfg.pattern_num++;
                ch->used = true;
            }
        }

        xSemaphoreGive(startSemaphore);
    }

    return result;
}


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

/* Пины делителя и выбор режима 5V/3V3/10V */
static void analog_setupDivider(PANALOG_CHANNEL ch, int slot_num)
{
    uint8_t divPin_1 = SLOTS_PIN_MAP[slot_num][2];
    esp_rom_gpio_pad_select_gpio(divPin_1);
    gpio_set_direction(divPin_1, GPIO_MODE_OUTPUT);

    uint8_t divPin_2 = SLOTS_PIN_MAP[slot_num][1];
    esp_rom_gpio_pad_select_gpio(divPin_2);
    gpio_set_direction(divPin_2, GPIO_MODE_OUTPUT);

    switch (ch->divider)
    {
        case MODE_3V3:
            gpio_set_level(divPin_1, 0);
            gpio_set_level(divPin_2, 0);
            ESP_LOGD(TAG, "S%d: divider mode: 3V3", slot_num);
            break;

        case MODE_10V:
            gpio_set_level(divPin_1, 0);
            gpio_set_level(divPin_2, 1);
            ESP_LOGD(TAG, "S%d: divider mode: 10V", slot_num);
            break;

        case MODE_5V:
        default:
            gpio_set_level(divPin_1, 1);
            gpio_set_level(divPin_2, 0);
            ESP_LOGD(TAG, "S%d: divider mode: 5V", slot_num);
            break;
    }
}

/*
    Модуль аналогового ввода - ADC с делителем 5V/3V3/10V
    На платах v4 и новее работает непрерывный ADC1, на более старых - легаси single-shot
    slots: 0-5
*/
void configure_analog(PANALOG_CHANNEL ch, int slot_num)
{
    ch->slot_num            = slot_num;
    ch->prev_result         = 0xFFFF;
    ch->threshState         = 0;
    ch->threshPendingTarget = -1;

    /* Флаг - выводить значение с плавающей точкой вместо целого
    */
    int flag_float_output = get_option_flag_val(slot_num, "floatOutput");
    ESP_LOGD(TAG, "S%d: float output = %d", slot_num, flag_float_output);

    /* Верхний порог значений - По умолчанию 4095
    */
    ch->MAX_VAL = get_option_int_val(slot_num, "maxVal", "", 4095, 0, 4095);
    ESP_LOGD(TAG, "S%d: max_val:%d", slot_num, ch->MAX_VAL);

    /* Нижний порог значений - По умолчанию 0
    */
    ch->MIN_VAL = get_option_int_val(slot_num, "minVal", "", 0, 0, 4095);
    ESP_LOGD(TAG, "S%d: min_val:%d", slot_num, ch->MIN_VAL);

    /* Флаг инвертирования значений
    */
    ch->inverse = get_option_flag_val(slot_num, "inverse");

    /* Коэффициент фильтрации - По умолчанию 1
    */
    ch->k = get_option_float_val(slot_num, "filterK", 1);
    ESP_LOGD(TAG, "S%d: filter k:%f", slot_num, ch->k);

    /* Порог срабатывания фильтра дребезга - По умолчанию 10
    */
    ch->dead_band = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4095);
    ESP_LOGD(TAG, "S%d: dead_band:%d", slot_num, ch->dead_band);

    /* Периодичность отсчётов в мс - По умолчанию 0
    */
    ch->periodic = get_option_int_val(slot_num, "periodic", "", 0, 0, 4095);
    ESP_LOGD(TAG, "S%d: periodic:%d", slot_num, ch->periodic);

    /* Режим делителя 5V/3V3/10V - По умолчанию 5V
    */
    if ((ch->divider = get_option_enum_val(slot_num, "dividerMode", "5V", "3V3", "10V", NULL)) < 0)
    {
        ESP_LOGE(TAG, "S%d: dividerMode: unrecognized value", slot_num);
        ch->divider = MODE_5V;
    }
    // Пины делителя разводим сразу здесь: режим уже разобран, а оба пути
    // (непрерывный и легаси) нуждаются в нём одинаково.
    analog_setupDivider(ch, slot_num);

    /* Пороговое значение - выше порога рапорт 1 иначе 0, при задании режим float игнорируется - По умолчанию -1 (выкл)
    */
    ch->threshold = get_option_int_val(slot_num, "threshold", "", -1, -1, 4095);

    /* Ширина зоны гистерезиса порога - По умолчанию 0
    */
    ch->thresholdHyst = get_option_int_val(slot_num, "thresholdHysteresis", "", 0, 0, 2048);

    /* Задержка подтверждения перехода в 1 в мс - По умолчанию 0
    */
    ch->thresholdRiseLag = get_option_int_val(slot_num, "thresholdRiseLag", "ms", 0, 0, 10000);

    /* Задержка подтверждения перехода в 0 в мс - По умолчанию 0
    */
    ch->thresholdFallLag = get_option_int_val(slot_num, "thresholdFallLag", "ms", 0, 0, 10000);

    if (ch->threshold >= 0)
    {
        ESP_LOGD(TAG, "S%d: threshold:%d hyst:%d riseLag:%d fallLag:%d",
                 slot_num, ch->threshold, ch->thresholdHyst,
                 ch->thresholdRiseLag, ch->thresholdFallLag);
    }

    /* Значение канала как отношение к шкале minVal-maxVal
    */
    ch->ratioReport = stdreport_register(RPTT_ratio, slot_num, "unit", "event/ratio", (int)ch->MIN_VAL, (int)ch->MAX_VAL);

    /* Сырое целочисленное значение канала
    */
    ch->rawReport = stdreport_register(RPTT_int, slot_num, "unit", "event/rawVal");

    /* Состояние порога 0 или 1
    */
    ch->thresholdReport = stdreport_register(RPTT_int, slot_num, "bool", "event/threshold", 0, 1);

    if (ch->threshold >= 0)
    {
        ch->currentReport = ch->thresholdReport;
    }
    else
    {
        ch->currentReport = flag_float_output ? ch->ratioReport : ch->rawReport;
    }
}

/* Обработка очередного усреднённого отсчёта: фильтр, пороговый автомат, рапорт.
   Общая для обоих путей - в непрерывном и легаси вариантах эта логика была
   скопирована один в один и расходилась только в условии периодического рапорта
   (здесь взят вариант по времени: он верен и когда период цикла не равен periodic). */
static void analog_processSample(PANALOG_CHANNEL ch, uint32_t raw_val)
{
    if (ch->prev_result == 0xFFFF)
    {
        /* Первый отсчёт: фильтр не применяем, а инициализируем им состояние.
           Иначе при filterK < 1 стартовое значение занижалось - result равен нулю,
           и первый же рапорт уходил как raw*k вместо raw. */
        ch->result      = (uint16_t)raw_val;
        ch->prev_result = ch->result;

        if (ch->threshold >= 0)
        {
            ch->threshState = (ch->result >= (ch->threshold + ch->thresholdHyst / 2)) ? 1 : 0;
            stdreport_i(ch->thresholdReport, ch->threshState);
        }
        else
        {
            stdreport_i(ch->currentReport, ch->result);
        }

        ch->lastReportTime = xTaskGetTickCount();
        return;
    }

    ch->result = ch->result * (1 - ch->k) + raw_val * ch->k;

    if (ch->threshold >= 0)
    {
        // --- Пороговый режим ---
        int halfHyst  = ch->thresholdHyst / 2;
        int riseLevel = ch->threshold + halfHyst;
        int fallLevel = ch->threshold - halfHyst;
        int newState  = ch->threshState;

        if (ch->threshState == 0 && ch->result >= riseLevel)
        {
            newState = 1;
        }
        else if (ch->threshState == 1 && ch->result < fallLevel)
        {
            newState = 0;
        }

        if (newState != ch->threshState)
        {
            int lagMs = (newState == 1) ? ch->thresholdRiseLag : ch->thresholdFallLag;

            if (lagMs <= 0)
            {
                // Без задержки - переход сразу
                ch->threshState = newState;
                ch->threshPendingTarget = -1;
                stdreport_i(ch->thresholdReport, ch->threshState);
            }
            else if (ch->threshPendingTarget != newState)
            {
                // Начинаем отсчёт подтверждения
                ch->threshPendingTarget = newState;
                ch->threshPendingTick   = xTaskGetTickCount();
            }
            // иначе: переход в эту сторону уже ждёт подтверждения
        }
        else
        {
            // Значение вернулось в текущее состояние - отменяем ожидание
            ch->threshPendingTarget = -1;
        }

        if (ch->threshPendingTarget >= 0)
        {
            int lagMs = (ch->threshPendingTarget == 1) ? ch->thresholdRiseLag : ch->thresholdFallLag;

            if ((xTaskGetTickCount() - ch->threshPendingTick) >= pdMS_TO_TICKS(lagMs))
            {
                // Перепроверяем: условие всё ещё выполняется?
                int stillMet = 0;
                if (ch->threshPendingTarget == 1 && ch->result >= riseLevel) stillMet = 1;
                if (ch->threshPendingTarget == 0 && ch->result <  fallLevel) stillMet = 1;

                if (stillMet)
                {
                    ch->threshState = ch->threshPendingTarget;
                    stdreport_i(ch->thresholdReport, ch->threshState);
                }
                ch->threshPendingTarget = -1;
            }
        }
    }
    else
    {
        // --- Обычный режим (raw / ratio) ---
        if ((abs(ch->result - ch->prev_result) > ch->dead_band) ||
            ((ch->periodic != 0) && ((xTaskGetTickCount() - ch->lastReportTime) >= pdMS_TO_TICKS(ch->periodic))))
        {
            ch->prev_result = ch->result;
            stdreport_i(ch->currentReport, ch->result);
            ch->lastReportTime = xTaskGetTickCount();
        }
    }
}

/* Модуль только рапортует, команд не принимает - нужен лишь trigger-топик */
static void analog_setupTopic(int slot_num)
{
    char *str = calloc(strlen(me_config.deviceName) + strlen("/analog_") + 4, sizeof(char));
    sprintf(str, "%s/analog_%d", me_config.deviceName, slot_num);
    me_state.trigger_topic_list[slot_num] = str;
}


// ---------------------------------------------------------------------------
// ---------------------------------- TASKS ----------------------------------
// -----------------|---------------------------(|------------------|---------

/* Непрерывный ADC1 (платы v4 и новее).
   Первая стартовавшая задача остаётся на дежурстве и разбирает отсчёты ВСЕХ
   каналов, остальные завершаются - у периферии один общий поток данных. */
static void analog_continuous_task(void *arg)
{
    uint32_t        ret_num     = 0;
    int             slot_num    = (int)(intptr_t)arg;
    PANALOG_CHANNEL ch          = &analog_channels[slot_num];
    uint8_t         result      [ ADC_READ_LEN ] = {0};

    configure_analog(ch, slot_num);
    analog_setupTopic(slot_num);

    adc1_init();

    if (adc1_add_channel(ch))
    {
        waitForWorkPermit(slot_num);

        if (adc1_start_and_there_can_be_only_one())
        {
            ESP_LOGD(TAG, "task on slot %d remained on duty and processes all channels", slot_num);

            while (1)
            {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

                while (adc_continuous_read(adc1_handle, result, ADC_READ_LEN, &ret_num, 0) == ESP_OK)
                {
                    for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES)
                    {
                        adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];

                        uint32_t chan_num = ADC_GET_CHANNEL(p);
                        uint32_t raw_val  = ADC_GET_DATA(p);

                        int slot = (chan_num < 10) ? channel_to_slot[chan_num] : -1;

                        if (slot < 0)
                            continue;

                        PANALOG_CHANNEL chan = &analog_channels[slot];

                        if (chan->averagingCount < ANALOG_OVERSAMPLE)
                        {
                            if (chan->inverse)
                                raw_val = 4096 - raw_val;

                            chan->averagingSum += raw_val;
                            chan->averagingCount++;
                        }

                        if (chan->averagingCount >= ANALOG_OVERSAMPLE)
                        {
                            raw_val = chan->averagingSum / chan->averagingCount;

                            chan->averagingCount = 0;
                            chan->averagingSum   = 0;

                            analog_processSample(chan, raw_val);
                        }
                    }

                    vTaskDelay(1);
                }
            }
        }
    }
    else
    {
        ESP_LOGE(TAG, "S%d: adc1_add_channel FAILED (GPIO=%d)", slot_num, SLOTS_PIN_MAP[slot_num][0]);
    }

    ESP_LOGD(TAG, "task of slot %d dismissed", slot_num);

    vTaskDelete(NULL);
}

/* Легаси single-shot ADC (платы младше v4): непрерывного ADC на них нет,
   опрашиваем канал сами с усреднением на месте. */
static void analog_legacy_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;
    uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];

    if (slot_num == 1)
    {
        ESP_LOGW(TAG, "S%d: no ADC on SLOT_1, use another slot", slot_num);
        mblog(W, "no ADC on SLOT_1, use another slot");
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(NULL);
    }

    PANALOG_CHANNEL ch = &analog_channels[slot_num];

    static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
    static const adc_atten_t atten = ADC_ATTEN_DB_11;

    gpio_reset_pin(sens_pin_num);
    gpio_set_direction(sens_pin_num, GPIO_MODE_INPUT);
    adc_channel_t ADC_chan = slot_num;

    if (slot_num == 2)
    {
        ADC_chan = ADC2_CHANNEL_6;
        adc2_config_channel_atten(ADC_chan, atten);
    }
    else
    {
        switch (slot_num)
        {
            case 0: ADC_chan = ADC1_CHANNEL_3; break;
            case 3: ADC_chan = ADC1_CHANNEL_2; break;
            case 4: ADC_chan = ADC1_CHANNEL_1; break;
            case 5: ADC_chan = ADC1_CHANNEL_6; break;
        }
        adc1_config_width(width);
        adc1_config_channel_atten(ADC_chan, atten);
    }

    configure_analog(ch, slot_num);
    analog_setupTopic(slot_num);

    waitForWorkPermit(slot_num);

    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1)
    {
        uint32_t tmp = 0;
        int raw_val = 0;

        for (int i = 0; i < ANALOG_OVERSAMPLE; i++)
        {
            if (slot_num == 2)
            {
                adc2_get_raw(ADC_chan, width, &raw_val);
            }
            else
            {
                raw_val = adc1_get_raw(ADC_chan);
            }

            if (ch->inverse)
                raw_val = 4096 - raw_val;

            tmp += raw_val;
        }

        tmp /= ANALOG_OVERSAMPLE;

        analog_processSample(ch, tmp);

        if (ch->periodic != 0)
        {
            vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(ch->periodic));
        }
        else
        {
            vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(ANALOG_IDLE_PERIOD));
        }
    }
}

void start_analog_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();

    char taskName[32];
    sprintf(taskName, "task_analog_%d", slot_num);

    if (me_config.boardVersion < 4)
    {
        // На платах младше v4 непрерывного ADC нет - идём легаси-путём.
        ESP_LOGD(TAG, "S%d: boardVersion %d < 4, legacy single-shot ADC", slot_num, me_config.boardVersion);
        xTaskCreatePinnedToCore(analog_legacy_task, taskName, 1024 * 4, (void*)(intptr_t)slot_num, 12, NULL, 1);
    }
    else
    {
        if (!startSemaphore)
            startSemaphore = xSemaphoreCreateMutex();

        xTaskCreatePinnedToCore(analog_continuous_task, taskName, 1024 * 4, (void*)(intptr_t)slot_num, 12, NULL, 1);
    }

    ESP_LOGD(TAG, "S%d: analog task started, heap usage: %lu free: %u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_analog()
{
    return manifesto;
}
