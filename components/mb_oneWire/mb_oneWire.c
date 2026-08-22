// ***************************************************************************
// TITLE
//     ds18b20 - термометры 1-Wire на нулевом пине слота
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

#include "mb_oneWire.h"
#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"
#include <stdcommand.h>
#include <stdreport.h>
#include <mbdebug.h>

#include "onewire_bus.h"
#include "ds18b20.h"

#include <generated_files/gen_mb_oneWire.h>

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

/* Датчиков на одну шину. Потолок продиктован таблицей отчётов: она одна на всё
   устройство (MAX_NUM_OF_STDREPORTS), и подтопик каждого канала обязан быть
   строковым литералом, иначе манифест получит имя переменной вместо топика. */
#define DS_MAX_SENSORS          4

/* Пауза преобразования, миллисекунды - ровно та, что выдерживает драйвер
   внутри ds18b20_trigger_temperature_conversion. Индекс - ds18b20_resolution_t.
   Датчики опрашиваются по очереди, поэтому цикл растёт линейно с их числом:
   четыре датчика на 12 битах это больше трёх секунд на проход. */
static const uint32_t DS_CONVERSION_MS[4] = { 100, 200, 400, 800 };

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct {
    ds18b20_device_handle_t dev;
    uint64_t                address;

    float                   lastSent;       /* последняя опубликованная       */
    uint8_t                 primed;         /* lastSent заполнена             */

    int8_t                  state;          /* состояние порога, -1 неизвестно */

    int                     tempReport;
    int                     threshReport;
} ds_sensor_t;

typedef struct {
    STDCOMMANDS             cmds;

    float                   deadBand;       /* градусы                        */
    float                   threshold;      /* градусы, 0 - порог выключен    */
    uint8_t                 inverse;
    uint8_t                 silent;
    uint32_t                periodic;       /* секунды, 0 - только по факту   */
    ds18b20_resolution_t    resolution;

    int                     active_state;

    int                     count;
    ds_sensor_t             sensor[ DS_MAX_SENSORS ];
} ds_ctx_t;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

static const char *TAG = "ONE_WIRE";

// ---------------------------------------------------------------------------
// ------------------------------ CONFIGURATION ------------------------------
// -----|-------------------|-------------------------------------------------

/*
    Термометры DS18B20 на шине 1-Wire, линия данных на нулевом пине слота
    На одной шине до четырёх датчиков, каналы нумеруются по возрастанию адреса ROM
    slots: 0-5
*/
void configure_ds18b20(ds_ctx_t *c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* === OPTIONS === */

    /* Минимальное изменение температуры для публикации в градусах, По умолчанию одна десятая градуса
    */
    c->deadBand = get_option_float_val(slot_num, "deadBand", 0.1);
    if (c->deadBand < 0) c->deadBand = -c->deadBand;
    ESP_LOGD(TAG, "[temp_%d] deadBand:%.2f", slot_num, c->deadBand);

    /* Порог срабатывания в градусах, 0 - порог выключен, По умолчанию 0
    */
    c->threshold = get_option_float_val(slot_num, "threshold", 0.0);
    ESP_LOGD(TAG, "[temp_%d] threshold:%.2f", slot_num, c->threshold);

    /* Инверсия состояния порога, По умолчанию выключена
    */
    c->inverse = get_option_flag_val(slot_num, "stateInverse");
    ESP_LOGD(TAG, "[temp_%d] stateInverse:%d", slot_num, c->inverse);

    /* Не публиковать саму температуру, оставить только состояние порога, По умолчанию выключено
    */
    c->silent = get_option_flag_val(slot_num, "silent");
    ESP_LOGD(TAG, "[temp_%d] silent:%d", slot_num, c->silent);

    /* Период публикации в секундах независимо от изменения, 0 - только при изменении больше deadBand, По умолчанию 0
    */
    c->periodic = get_option_int_val(slot_num, "periodic", "s", 0, 0, 86400);
    ESP_LOGD(TAG, "[temp_%d] periodic:%lu", slot_num, (unsigned long)c->periodic);

    /* Разрядность датчика 9-12 бит, влияет на время преобразования от 100 до 800 мс, По умолчанию 12
    */
    {
        int bits = get_option_int_val(slot_num, "resolution", "bit", 12, 9, 12);
        c->resolution = (ds18b20_resolution_t)(bits - 9);
        ESP_LOGD(TAG, "[temp_%d] resolution:%d bit, conversion %lu ms",
                 slot_num, bits, (unsigned long)DS_CONVERSION_MS[c->resolution]);
    }

    /* Состояние модуля при старте - 1 активен, 0 спит до action/enable 1, По умолчанию 1
    */
    c->active_state = get_option_int_val(slot_num, "defaultState", "", 1, 0, 1);

    /* Топик по умолчанию: <deviceName>/temp_<slot_num>.
       Суффиксы /action/ и /event/ добавляются при приёме команд и публикации.
       Заполняем обе таблицы - без action_topic_list не дойдёт action/enable. */
    {
        char t_str[strlen(me_config.deviceName) + strlen("/temp_0") + 3];
        sprintf(t_str, "%s/temp_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]  = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart temp topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль - выключенный не опрашивает датчики
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");

    /* Температура датчика 0 в градусах Цельсия
    */
    c->sensor[0].tempReport   = stdreport_register(RPTT_float, slot_num, "C", "event/ch_0/temperature");

    /* Порог по датчику 0 пройден - 1 выше порога, 0 ниже
    */
    c->sensor[0].threshReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_0/threshold");

    /* Температура датчика 1 в градусах Цельсия
    */
    c->sensor[1].tempReport   = stdreport_register(RPTT_float, slot_num, "C", "event/ch_1/temperature");

    /* Порог по датчику 1 пройден - 1 выше порога, 0 ниже
    */
    c->sensor[1].threshReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_1/threshold");

    /* Температура датчика 2 в градусах Цельсия
    */
    c->sensor[2].tempReport   = stdreport_register(RPTT_float, slot_num, "C", "event/ch_2/temperature");

    /* Порог по датчику 2 пройден - 1 выше порога, 0 ниже
    */
    c->sensor[2].threshReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_2/threshold");

    /* Температура датчика 3 в градусах Цельсия
    */
    c->sensor[3].tempReport   = stdreport_register(RPTT_float, slot_num, "C", "event/ch_3/temperature");

    /* Порог по датчику 3 пройден - 1 выше порога, 0 ниже
    */
    c->sensor[3].threshReport = stdreport_register(RPTT_int, slot_num, "", "event/ch_3/threshold");

    for (int i = 0; i < DS_MAX_SENSORS; i++)
    {
        c->sensor[i].state  = -1;
        c->sensor[i].primed = 0;
    }
}

// ---------------------------------------------------------------------------
// --------------------------------- SEARCH ----------------------------------
// -----|-------------------|-------------------------------------------------

/* Перебрать шину и подобрать все DS18B20, но не больше DS_MAX_SENSORS.
   Возвращает число найденных. */
static int ds_discover(ds_ctx_t *c, onewire_bus_handle_t bus, int slot_num)
{
    onewire_device_iter_handle_t    iter = NULL;
    onewire_device_t                device;
    esp_err_t                       res;

    if (onewire_new_device_iter(bus, &iter) != ESP_OK)
    {
        ESP_LOGE(TAG, "[temp_%d] cannot create 1-Wire device iterator", slot_num);
        return 0;
    }

    do {
        res = onewire_device_iter_get_next(iter, &device);

        if (res != ESP_OK)
            continue;

        if (c->count >= DS_MAX_SENSORS)
        {
            ESP_LOGE(TAG, "[temp_%d] more than %d sensors on the bus, %016llX ignored",
                     slot_num, DS_MAX_SENSORS, device.address);
            mblog(E, "ds18b20 slot_%d: more than %d sensors, extra ones ignored", slot_num, DS_MAX_SENSORS);
            continue;
        }

        ds18b20_config_t cfg = {};

        if (ds18b20_new_device(&device, &cfg, &c->sensor[c->count].dev) == ESP_OK)
        {
            c->sensor[c->count].address = device.address;
            ESP_LOGI(TAG, "[temp_%d] ch_%d address %016llX", slot_num, c->count, device.address);
            c->count++;
        }
        else
        {
            ESP_LOGW(TAG, "[temp_%d] not a DS18B20, address %016llX", slot_num, device.address);
        }

    } while (res != ESP_ERR_NOT_FOUND);

    onewire_del_device_iter(iter);

    return c->count;
}

// ---------------------------------------------------------------------------
// --------------------------------- REPORT ----------------------------------
// -----|-------------------|-------------------------------------------------

static void ds_handleValue(ds_ctx_t *c, int idx, float temperature, int forcePublish)
{
    ds_sensor_t * s = &c->sensor[idx];

    if (!c->silent)
    {
        /* Дедбенд считается от последнего ОТПРАВЛЕННОГО значения, поэтому
           медленный дрейф не накапливается незамеченным. */
        int changed = !s->primed || (fabsf(temperature - s->lastSent) >= c->deadBand);

        if (changed || forcePublish)
        {
            s->lastSent = temperature;
            s->primed   = 1;

            if (s->tempReport >= 0)
                stdreport_f(s->tempReport, temperature);
        }
    }

    if (c->threshold != 0.0f)
    {
        int8_t state = (temperature > c->threshold) ? !c->inverse : c->inverse;

        if (state != s->state)
        {
            s->state = state;

            if (s->threshReport >= 0)
                stdreport_i(s->threshReport, state);
        }
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------- TASK -----------------------------------
// -----|-------------------|-------------------------------------------------

void ds18b20_task(void* arg)
{
    int                     slot_num = (int)(intptr_t)arg;
    ds_ctx_t                c        = { 0 };
    STDCOMMAND_PARAMS       params   = { 0 };
    onewire_bus_handle_t    bus      = NULL;
    uint8_t                 pin      = SLOTS_PIN_MAP[slot_num][0];

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    configure_ds18b20(&c, slot_num);

    onewire_bus_config_t bus_config = {
        .bus_gpio_num = pin,
    };

    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10, // 1 байт команды ROM + 8 байт адреса + 1 байт команды устройству
    };

    /* Без ESP_ERROR_CHECK: занятый RMT-канал или занятый пин не повод ронять
       всё устройство в панику - остальные слоты работать обязаны. */
    if (onewire_new_bus_rmt(&bus_config, &rmt_config, &bus) != ESP_OK)
    {
        ESP_LOGE(TAG, "[temp_%d] cannot install 1-Wire bus on GPIO%d, no free RMT channel?", slot_num, pin);
        mblog(E, "ds18b20 slot_%d: 1-Wire bus init failed on pin %d", slot_num, pin);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[temp_%d] 1-Wire bus on GPIO%d", slot_num, pin);

    if (ds_discover(&c, bus, slot_num) == 0)
    {
        ESP_LOGE(TAG, "[temp_%d] no DS18B20 found on GPIO%d", slot_num, pin);
        mblog(E, "ds18b20 slot_%d: no sensor found on pin %d", slot_num, pin);
        onewire_bus_del(bus);
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < c.count; i++)
        ds18b20_set_resolution(c.sensor[i].dev, c.resolution);

    ESP_LOGI(TAG, "[temp_%d] ready: %d sensor(s), %d bit, deadBand %.2f C, periodic %lu s",
             slot_num, c.count, c.resolution + 9, c.deadBand, (unsigned long)c.periodic);

    waitForWorkPermit(slot_num);

    /* Стартовый рапорт состояния модуля (Конституция §6) */
    stdreport_enable(slot_num, c.active_state);

    /* Опрос не чаще, чем шина успевает обойти все датчики. Период короче
       времени прохода превратил бы vTaskDelayUntil в холостой цикл без пауз. */
    uint32_t cycleMs = (uint32_t)c.count * (DS_CONVERSION_MS[c.resolution] + 60);
    uint32_t pollMs  = (cycleMs < 1000) ? 1000 : cycleMs;

    if (c.periodic > 0)
    {
        uint32_t want = c.periodic * 1000;

        if (want < cycleMs)
        {
            ESP_LOGE(TAG, "[temp_%d] periodic %lu s is shorter than %lu ms needed for %d sensor(s), using %lu ms",
                     slot_num, (unsigned long)c.periodic, (unsigned long)cycleMs, c.count, (unsigned long)cycleMs);
            want = cycleMs;
        }

        pollMs = want;
    }

    ESP_LOGI(TAG, "[temp_%d] poll period %lu ms", slot_num, (unsigned long)pollMs);

    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1)
    {
        /* Команды разбираем без ожидания - задачу ведёт таймер опроса. */
        int cmd;

        while ((cmd = stdcommand_receive(&c.cmds, &params, 0)) != -1)
        {
            switch (cmd)
            {
                case STDCMD_ENABLE:
                    if (params.count > 0)
                    {
                        int newState = params.p[0].i ? 1 : 0;

                        if (newState != c.active_state)
                        {
                            c.active_state = newState;
                            ESP_LOGD(TAG, "[temp_%d] enable:%d", slot_num, c.active_state);
                            /* event/enable публикуем явно - авто-рассылки нет */
                            stdreport_enable(slot_num, c.active_state);
                        }
                    }
                    break;

                default:
                    break;
            }
        }

        if (c.active_state)
        {
            for (int i = 0; i < c.count; i++)
            {
                float temperature;

                if (ds18b20_trigger_temperature_conversion(c.sensor[i].dev) != ESP_OK)
                {
                    ESP_LOGW(TAG, "[temp_%d] ch_%d conversion failed", slot_num, i);
                    continue;
                }

                if (ds18b20_get_temperature(c.sensor[i].dev, &temperature) != ESP_OK)
                {
                    ESP_LOGW(TAG, "[temp_%d] ch_%d read failed", slot_num, i);
                    continue;
                }

                ds_handleValue(&c, i, temperature, c.periodic > 0);
            }
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(pollMs));
    }
}

void start_ds18b20_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];

    sprintf(tmpString, "ds18b20_task_%d", slot_num);
    xTaskCreatePinnedToCore(ds18b20_task, tmpString, 1024 * 4, (void*)(intptr_t)slot_num,
                            configMAX_PRIORITIES - 12, NULL, 0);

    ESP_LOGD(TAG, "ds18b20_task init ok: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_mb_oneWire()
{
    return manifesto;
}
