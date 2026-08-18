// ***************************************************************************
// TITLE
//     PPMservo - одна серва на слот, управление длительностью импульса
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "servos.h"
#include "executor.h"
#include "me_slot_config.h"
#include "stateConfig.h"
#include <stdcommand.h>
#include <stdreport.h>

#include <generated_files/gen_PPMservo.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

// Сигнал сервы - импульс 1000-2000 мкс с периодом 20 мс (50 Гц).
// Генерирует LEDC: период задаёт таймер, длительность импульса - duty канала.
// Таймер 1 взят потому, что LEDC_TIMER (нулевой) занят светодиодными модулями
// с их 5 kHz - на общем таймере частоту сервы не выставить.
#define SERVO_LEDC_TIMER        LEDC_TIMER_1
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
// 14 бит - потолок разрешения LEDC у esp32s3. На периоде 20 мс шаг выходит
// 20000/16384 = 1,2 мкс, для сервы это тоньше её собственной зоны нечувствительности
#define SERVO_LEDC_RES          LEDC_TIMER_14_BIT
#define SERVO_FREQ_HZ           50
#define SERVO_PERIOD_US         (1000000 / SERVO_FREQ_HZ)
#define SERVO_DUTY_FULL         (1UL << 14)

// Разумные пределы длительности импульса - за ними серва не работает,
// а рулевая машинка встаёт в упор. В get_option_int_val подставлены числами:
// manifesto читает исходник статически и макросы в манифест не разворачивает.

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_SERVOCONFIG
{
    uint16_t                minPulse;
    uint16_t                maxPulse;
    uint16_t                centerPulse;

    // Длительность импульса, которая уходит на выход (invert уже применён)
    uint16_t                currentPulse;

    uint8_t                 invert;
    // 0 - позиция ещё не задана (startPos 0), выход молчит
    uint8_t                 hasPulse;

    int                     active_state;
    int                     ledc_channel;

    STDCOMMANDS             cmds;

} SERVOCONFIG, * PSERVOCONFIG;

typedef enum
{
    servoCMD_setPulse = 0
} servoCMD;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "PPMservo";

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// Границы хода в мкс - minPulse может быть задан больше maxPulse
static inline uint16_t servo_lowLimit(PSERVOCONFIG c){
    return (c->minPulse < c->maxPulse) ? c->minPulse : c->maxPulse;
}

static inline uint16_t servo_highLimit(PSERVOCONFIG c){
    return (c->minPulse < c->maxPulse) ? c->maxPulse : c->minPulse;
}

static uint16_t servo_clamp(PSERVOCONFIG c, int pulse){
    if (pulse < (int)servo_lowLimit(c))  pulse = servo_lowLimit(c);
    if (pulse > (int)servo_highLimit(c)) pulse = servo_highLimit(c);
    return (uint16_t)pulse;
}

// Зеркалим позицию относительно нейтрали centerPulse
static uint16_t servo_invert(PSERVOCONFIG c, uint16_t pulse){
    if (!c->invert) return pulse;
    return servo_clamp(c, 2 * (int)c->centerPulse - (int)pulse);
}

// Выдаём импульс в железо. Модуль спит или позиция не задана - duty 0,
// серва остаётся без сигнала и не держит момент
static void servo_apply(PSERVOCONFIG c){
    uint32_t duty = 0;

    if (c->active_state && c->hasPulse){
        // с округлением до ближайшего шага duty
        duty = ((uint32_t)c->currentPulse * SERVO_DUTY_FULL + SERVO_PERIOD_US/2) / SERVO_PERIOD_US;
    }

    ledc_set_duty(SERVO_LEDC_MODE, (ledc_channel_t)c->ledc_channel, duty);
    ledc_update_duty(SERVO_LEDC_MODE, (ledc_channel_t)c->ledc_channel);
}

static void servo_setPulse(PSERVOCONFIG c, int pulse_us){
    c->currentPulse = servo_invert(c, servo_clamp(c, pulse_us));
    c->hasPulse = 1;
    servo_apply(c);
}

/*
    Рулевая машинка или регулятор с управлением длительностью импульса - PPM 50 Гц
    Одна серва на слот, сигнал на втором пине слота
    slots: 0-5
*/
void configure_PPMservo(PSERVOCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* === OPTIONS === */

    /* Длительность импульса в крайнем положении min, По умолчанию 1000 мкс
    */
    c->minPulse = get_option_int_val(slot_num, "minPulse", "us", 1000, 500, 2500);
    ESP_LOGD(TAG, "[servo_%d] minPulse:%d", slot_num, c->minPulse);

    /* Длительность импульса в крайнем положении max, По умолчанию 2000 мкс
    */
    c->maxPulse = get_option_int_val(slot_num, "maxPulse", "us", 2000, 500, 2500);
    ESP_LOGD(TAG, "[servo_%d] maxPulse:%d", slot_num, c->maxPulse);

    /* Нейтраль руля, 0 - взять середину между minPulse и maxPulse, По умолчанию 0
    */
    c->centerPulse = get_option_int_val(slot_num, "centerPulse", "us", 0, 0, 2500);
    if (c->centerPulse == 0){
        c->centerPulse = (uint16_t)(((int)c->minPulse + (int)c->maxPulse) / 2);
    }
    c->centerPulse = servo_clamp(c, c->centerPulse);
    ESP_LOGD(TAG, "[servo_%d] centerPulse:%d", slot_num, c->centerPulse);

    /* Зеркалит ход сервы относительно нейтрали centerPulse, По умолчанию выключен
    */
    c->invert = get_option_flag_val(slot_num, "invert");
    ESP_LOGD(TAG, "[servo_%d] invert:%d", slot_num, c->invert);

    /* Состояние модуля при старте - 1 активен, 0 спит до action/enable 1, По умолчанию 1
    */
    c->active_state = get_option_int_val(slot_num, "defaultState", "", 1, 0, 1);
    ESP_LOGD(TAG, "[servo_%d] defaultState:%d", slot_num, c->active_state);

    /* Положение при старте в микросекундах, 0 - не дёргать серву при старте, По умолчанию 0
    */
    int startPos = get_option_int_val(slot_num, "startPos", "us", 0, 0, 2500);
    if (startPos > 0){
        c->currentPulse = servo_invert(c, servo_clamp(c, startPos));
        c->hasPulse = 1;
        ESP_LOGD(TAG, "[servo_%d] startPos:%d pulse:%d", slot_num, startPos, c->currentPulse);
    }else{
        ESP_LOGD(TAG, "[servo_%d] startPos:0 - no signal until setPulse", slot_num);
    }

    /* Топик по умолчанию: <deviceName>/servo_<slot_num>.
       Суффиксы /action/ и /event/ добавляются при приёме команд и публикации событий.
       Заполняем обе таблицы - без trigger_topic_list репортер не публикует события. */
    {
        char t_str[strlen(me_config.deviceName)+strlen("/servo_0")+3];
        sprintf(t_str, "%s/servo_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart servo topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* === COMMANDS === */

    /* Задать длительность импульса в микросекундах - значение обрезается по minPulse и maxPulse
    */
    stdcommand_register(&c->cmds, servoCMD_setPulse, "action/setPulse", PARAMT_int);

    /* Включить 1 или выключить 0 модуль - выключенный не выдаёт импульсы и серва не держит момент
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void PPMservo_task(void *arg)
{
    SERVOCONFIG             c       = { 0 };
    STDCOMMAND_PARAMS       params  = { 0 };

    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    configure_PPMservo(&c, slot_num);

    ledc_timer_config_t ledc_timer = {
        .speed_mode         = SERVO_LEDC_MODE,
        .timer_num          = SERVO_LEDC_TIMER,
        .duty_resolution    = SERVO_LEDC_RES,
        .freq_hz            = SERVO_FREQ_HZ,
        .clk_cfg            = LEDC_AUTO_CLK };
    if (ledc_timer_config(&ledc_timer) != ESP_OK){
        ESP_LOGE(TAG, "LEDC timer config failed, slot:%d", slot_num);
        vTaskDelete(NULL);
    }

    c.ledc_channel = get_next_ledc_channel();
    if (c.ledc_channel < 0){
        ESP_LOGE(TAG, "LEDC channel has ended, slot:%d", slot_num);
        vTaskDelete(NULL);
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = SERVO_LEDC_MODE,
        .channel        = (ledc_channel_t)c.ledc_channel,
        .timer_sel      = SERVO_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SLOTS_PIN_MAP[slot_num][1],
        .duty           = 0,
        .hpoint         = 0 };
    if (ledc_channel_config(&ledc_channel) != ESP_OK){
        ESP_LOGE(TAG, "LEDC channel config failed, slot:%d", slot_num);
        vTaskDelete(NULL);
    }
    ESP_LOGD(TAG, "[servo_%d] pin:%d ledc channel:%d", slot_num, SLOTS_PIN_MAP[slot_num][1], c.ledc_channel);

    waitForWorkPermit(slot_num);

    /* Стартовый рапорт состояния модуля (Конституция §6) */
    stdreport_enable(slot_num, c.active_state);

    // Стартовая позиция уезжает только сейчас - до разрешения работы пин молчит
    servo_apply(&c);

    while (1){

        switch (stdcommand_receive(&c.cmds, &params, portMAX_DELAY))
        {
            case -1: // none
                break;

            case servoCMD_setPulse:
                if (params.count > 0){
                    servo_setPulse(&c, params.p[0].i);
                    ESP_LOGD(TAG, "[servo_%d] setPulse:%ld out:%d", slot_num, params.p[0].i, c.currentPulse);
                }
                break;

            case STDCMD_ENABLE:
                if (params.count > 0){
                    int newState = params.p[0].i ? 1 : 0;
                    if (newState != c.active_state){
                        c.active_state = newState;
                        servo_apply(&c);
                        ESP_LOGD(TAG, "[servo_%d] enable:%d", slot_num, c.active_state);
                        /* event/enable публикуем явно - авто-рассылки нет */
                        stdreport_enable(slot_num, c.active_state);
                    }
                }
                break;

            default:
                break;
        }
    }
}

void start_PPMservo_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_PPMservo_%d", slot_num);
    xTaskCreate(PPMservo_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-5, NULL);

    ESP_LOGD(TAG, "PPMservo task created for slot: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_PPMservo()
{
    return manifesto;
}
