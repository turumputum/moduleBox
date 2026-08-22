// ***************************************************************************
// TITLE
//     PWMgenerator - ШИМ заданной частоты и скважности на пине слота
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "pwmGen.h"
#include "executor.h"
#include "me_slot_config.h"
#include "stateConfig.h"
#include <stdcommand.h>
#include <stdreport.h>
#include <mbdebug.h>

#include <generated_files/gen_PWMgenerator.h>

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

/* Таймер общий с PPMservo. Таймеров LEDC у esp32s3 всего четыре, нулевой
   намертво занят светодиодными модулями с их 5 kHz, и плодить ещё один ради
   генератора не стали.

   Плата за это: частоту задаёт ТАЙМЕР, а не канал, поэтому все потребители
   этого таймера делят одну частоту. Слот с PPMservo и слот с PWMgenerator
   одновременно работать не могут - генератор перебьёт сервовые 50 Гц. Модуль
   проверяет конфигурацию на старте и ругается в лог, если наткнулся на серву. */
#define PWMGEN_LEDC_TIMER       LEDC_TIMER_1
#define PWMGEN_LEDC_MODE        LEDC_LOW_SPEED_MODE

/* Потолок разрешения LEDC у esp32s3 в low-speed режиме. */
#define PWMGEN_MAX_RES_BITS     14

/* Источник тактирования LEDC по умолчанию. Разрешение сверху ограничено
   величиной log2(clk / freq): на 25 kHz это 11 бит, на 100 kHz - 9. */
#define PWMGEN_SRC_CLK_HZ       80000000UL

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_PWMGENCONFIG
{
    uint32_t                frequency;      /* Гц                            */
    uint8_t                 resBits;        /* разрядность duty              */
    uint32_t                dutyFull;       /* значение duty для 100 %       */

    float                   duty;           /* текущая скважность, проценты  */
    uint8_t                 inverse;        /* аппаратная инверсия выхода    */

    int                     active_state;
    int                     ledc_channel;

    int                     dutyReport;

    STDCOMMANDS             cmds;

} PWMGENCONFIG, * PPWMGENCONFIG;

typedef enum
{
    pwmgenCMD_setDuty = 0
} pwmgenCMD;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

static const char *TAG = "PWM_GEN";

/* Частота, на которую общий таймер уже настроен генератором. Второй слот с
   другой частотой перебьёт первый - молча это делать нельзя. */
static uint32_t s_timerFreq = 0;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

/* Максимальное разрешение, при котором LEDC ещё вытянет запрошенную частоту. */
static uint8_t pwmgen_resolution(uint32_t freq)
{
    int res = PWMGEN_MAX_RES_BITS;

    while ((res > 1) && ((PWMGEN_SRC_CLK_HZ >> res) < freq))
        res--;

    return (uint8_t)res;
}

/* Выдать текущую скважность в железо. Спящий модуль держит выход в покое:
   duty 0, а при inverse аппаратная инверсия сама поднимет пин в единицу. */
static void pwmgen_apply(PPWMGENCONFIG c)
{
    uint32_t duty = 0;

    if (c->active_state)
    {
        duty = (uint32_t)lroundf(c->duty * (float)c->dutyFull / 100.0f);

        if (duty > c->dutyFull)
            duty = c->dutyFull;
    }

    if (ledc_set_duty(PWMGEN_LEDC_MODE, (ledc_channel_t)c->ledc_channel, duty) != ESP_OK)
    {
        ESP_LOGE(TAG, "ledc_set_duty failed, duty:%lu of %lu", (unsigned long)duty, (unsigned long)c->dutyFull);
        return;
    }

    ledc_update_duty(PWMGEN_LEDC_MODE, (ledc_channel_t)c->ledc_channel);
}

static void pwmgen_setDuty(PPWMGENCONFIG c, float percent)
{
    if (percent < 0.0f)   percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;

    c->duty = percent;

    pwmgen_apply(c);
}

/* Серва на другом слоте делит с нами таймер и потеряет свои 50 Гц. */
static void pwmgen_warnAboutServo(int slot_num)
{
    for (int i = 0; i < NUM_OF_SLOTS; i++)
    {
        if (me_config.slot_mode[i] && !strcmp(me_config.slot_mode[i], "PPMservo"))
        {
            ESP_LOGE(TAG, "[pwm_%d] slot_%d is PPMservo - both share LEDC timer 1, servo will lose its 50 Hz", slot_num, i);
            mblog(E, "PWMgenerator slot_%d and PPMservo slot_%d share LEDC timer, servo timing is broken", slot_num, i);
        }
    }
}

/*
    Генератор ШИМ - частота и скважность задаются конфигурацией и командами
    Выход на втором пине слота, тот же пин, что у PPMservo
    Делит таймер LEDC с сервоприводами, поэтому в одной конфигурации с PPMservo не работает
    slots: 0-5
*/
void configure_PWMgenerator(PPWMGENCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* === OPTIONS === */

    /* Частота ШИМ в герцах, По умолчанию 1000
    */
    c->frequency = get_option_int_val(slot_num, "frequency", "Hz", 1000, 10, 100000);
    ESP_LOGD(TAG, "[pwm_%d] frequency:%lu", slot_num, (unsigned long)c->frequency);

    /* Инверсия выхода - единица на выходе становится нулём, По умолчанию выключена
    */
    c->inverse = get_option_flag_val(slot_num, "inverse");
    ESP_LOGD(TAG, "[pwm_%d] inverse:%d", slot_num, c->inverse);

    /* Скважность при старте в процентах, По умолчанию 0
    */
    int startDuty = get_option_int_val(slot_num, "startDuty", "percent", 0, 0, 100);
    c->duty = (float)startDuty;
    ESP_LOGD(TAG, "[pwm_%d] startDuty:%d", slot_num, startDuty);

    /* Состояние модуля при старте - 1 активен, 0 спит до action/enable 1, По умолчанию 1
    */
    c->active_state = get_option_int_val(slot_num, "defaultState", "", 1, 0, 1);
    ESP_LOGD(TAG, "[pwm_%d] defaultState:%d", slot_num, c->active_state);

    /* Разрешение duty падает с ростом частоты - это свойство LEDC, а не выбор. */
    c->resBits  = pwmgen_resolution(c->frequency);
    c->dutyFull = 1UL << c->resBits;

    /* Топик по умолчанию: <deviceName>/pwm_<slot_num>.
       Суффиксы /action/ и /event/ добавляются при приёме команд и публикации.
       Заполняем обе таблицы - без trigger_topic_list репортер не публикует. */
    {
        char t_str[strlen(me_config.deviceName) + strlen("/pwm_0") + 3];
        sprintf(t_str, "%s/pwm_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]  = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart pwm topic:%s", me_state.action_topic_list[slot_num]);
    }

    /* === COMMANDS === */

    /* Задать скважность в процентах, значение 0-100, дробные значения допустимы
    */
    stdcommand_register(&c->cmds, pwmgenCMD_setDuty, "action/setDuty", PARAMT_float);

    /* Включить 1 или выключить 0 модуль - выключенный держит выход в покое
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");

    /* Текущая скважность в процентах
    */
    c->dutyReport = stdreport_register(RPTT_int, slot_num, "percent", "event/duty");
}

void PWMgenerator_task(void *arg)
{
    PWMGENCONFIG            c       = { 0 };
    STDCOMMAND_PARAMS       params  = { 0 };

    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    configure_PWMgenerator(&c, slot_num);

    pwmgen_warnAboutServo(slot_num);

    /* Второй генератор с другой частотой перенастроит общий таймер и уведёт
       частоту у первого - предупреждаем, но выполняем то, что попросили. */
    if ((s_timerFreq != 0) && (s_timerFreq != c.frequency))
    {
        ESP_LOGE(TAG, "[pwm_%d] LEDC timer is already running at %lu Hz, retuning to %lu Hz affects other PWM slots",
                 slot_num, (unsigned long)s_timerFreq, (unsigned long)c.frequency);
        mblog(E, "PWMgenerator slot_%d retunes shared LEDC timer to %lu Hz", slot_num, (unsigned long)c.frequency);
    }

    ledc_timer_config_t ledc_timer = {
        .speed_mode         = PWMGEN_LEDC_MODE,
        .timer_num          = PWMGEN_LEDC_TIMER,
        .duty_resolution    = (ledc_timer_bit_t)c.resBits,
        .freq_hz            = c.frequency,
        .clk_cfg            = LEDC_AUTO_CLK };

    if (ledc_timer_config(&ledc_timer) != ESP_OK)
    {
        ESP_LOGE(TAG, "[pwm_%d] LEDC timer config failed: %lu Hz, %d bit", slot_num, (unsigned long)c.frequency, c.resBits);
        mblog(E, "PWMgenerator slot_%d: LEDC timer config failed for %lu Hz", slot_num, (unsigned long)c.frequency);
        vTaskDelete(NULL);
        return;
    }

    s_timerFreq = c.frequency;

    c.ledc_channel = get_next_ledc_channel();

    if (c.ledc_channel < 0)
    {
        ESP_LOGE(TAG, "[pwm_%d] LEDC channel has ended", slot_num);
        vTaskDelete(NULL);
        return;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = PWMGEN_LEDC_MODE,
        .channel        = (ledc_channel_t)c.ledc_channel,
        .timer_sel      = PWMGEN_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SLOTS_PIN_MAP[slot_num][1],
        .duty           = 0,
        .hpoint         = 0,
        .flags.output_invert = c.inverse ? 1 : 0 };

    if (ledc_channel_config(&ledc_channel) != ESP_OK)
    {
        ESP_LOGE(TAG, "[pwm_%d] LEDC channel config failed", slot_num);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[pwm_%d] ready: pin %d, %lu Hz, %d bit (%lu steps), inverse %d, startDuty %d%%",
             slot_num, SLOTS_PIN_MAP[slot_num][1], (unsigned long)c.frequency,
             c.resBits, (unsigned long)c.dutyFull, c.inverse, (int)c.duty);

    waitForWorkPermit(slot_num);

    /* Стартовый рапорт состояния модуля (Конституция §6) */
    stdreport_enable(slot_num, c.active_state);

    /* Стартовая скважность уезжает только сейчас - до разрешения работы молчим */
    pwmgen_apply(&c);

    while (1)
    {
        switch (stdcommand_receive(&c.cmds, &params, portMAX_DELAY))
        {
            case -1: // none
                break;

            case pwmgenCMD_setDuty:
                if (params.count > 0)
                {
                    /* Параметр объявлен float, но целое тоже принимается -
                       из кросслинка значения приходят целыми. */
                    float percent = (params.p[0].type == PARAMT_float)
                                    ? params.p[0].f
                                    : (float)params.p[0].i;

                    pwmgen_setDuty(&c, percent);

                    if (c.dutyReport >= 0)
                        stdreport_i(c.dutyReport, (int)lroundf(c.duty));

                    ESP_LOGD(TAG, "[pwm_%d] setDuty:%d%%", slot_num, (int)lroundf(c.duty));
                }
                break;

            case STDCMD_ENABLE:
                if (params.count > 0)
                {
                    int newState = params.p[0].i ? 1 : 0;

                    if (newState != c.active_state)
                    {
                        c.active_state = newState;
                        pwmgen_apply(&c);
                        ESP_LOGD(TAG, "[pwm_%d] enable:%d", slot_num, c.active_state);
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

void start_PWMgenerator_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];

    sprintf(tmpString, "task_PWMgenerator_%d", slot_num);
    xTaskCreate(PWMgenerator_task, tmpString, 1024 * 4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 5, NULL);

    ESP_LOGD(TAG, "PWMgenerator task created for slot: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_PWMgenerator()
{
    return manifesto;
}
