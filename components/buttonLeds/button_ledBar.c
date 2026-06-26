// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "buttonLeds.h"
#include "button_logic.h"
#include "led_types.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "me_slot_config.h"
#include <mbdebug.h>
#include "stateConfig.h"
#include "executor.h"
#include "stdcommand.h"
#include "stdreport.h"
#include "math.h"
#include <arsenal.h>

#include <generated_files/gen_button_ledBar.h>

static const char *TAG = "BUTTON_LEDS";
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern SemaphoreHandle_t rmt_semaphore;
extern const uint8_t gamma_8[256];
// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// --- button_ledBar ---
typedef enum
{
    LEDBAR_default = 0,
    LEDBAR_toggleLedState,
    LEDBAR_setRGB,
    LEDBAR_setPos,
} LEDBAR_CMD;

/* 
    Модуль кнопка со шкалой заполнения
    slots: 0-5
*/
void configure_button_ledBar(PMODULE_CONTEXT ctx, int slot_num)
{
    stdcommand_init(&ctx->led.cmds, slot_num);
    // --- Button logic config ---
    /* Флаг определяет инверсию кнопки
    */
    ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");

    /* Глубина фильтра от дребезга контактов в мс. По умолчанию 10, 1-4096
    */
    ctx->button.debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "ms", 10, 1, 4096);

    /* Продолжительность длинного нажатия. По умолчанию 0, функция не активна
    */
    ctx->button.longPressTime 	= get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 65535);

    /* Длительность промежутка между нажатиями для регистрации двойного нажатия. По умолчанию 0, функция не активна
    */
    ctx->button.doubleClickTime = get_option_int_val(slot_num, "doubleClickTime", "ms", 0, 0, 65535);

    /* Подавляет короткое событие press, когда сработало длинное или двойное нажатие
       Выключен (0, по умолчанию) - короткие события шлются всегда
    */
    ctx->button.event_filter = get_option_flag_val(slot_num, "eventFilter");

    /* Период обновления потока кнопки в мс, по умолчанию 25 (40 Гц)
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 40, 1, 4096));

    {
        char t_str[strlen(me_config.deviceName)+strlen("/button_0")+3];
        sprintf(t_str, "%s/button_%d",me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
    }

	/* Рапортует при изменении состояния кнопки. 0-1.
	*/
	ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "unit", "event/press", 0, 1);

	/* Рапортует при регистрации длинного нажатия. 0-1.
	*/
	ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "unit", "event/longPress", 0, 1);

	/* Рапортует при регистрации двойного нажатия. 0-1.
	*/
	ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "unit", "event/doubleClick", 0, 1);

    // --- LED Bar logic config ---
    /* Количество светодиодов в ленте. По умолчанию 24. 1-1024.
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

    /* Флаг принудительной отправки буфера каждый цикл
    */
    ctx->led.periodicUpdate = get_option_flag_val(slot_num, "periodicUpdate");

    /* Величина приращения яркости за цикл. По умолчанию 255. 1-255.
    */
    ctx->led.increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(ctx->led.increment<1)ctx->led.increment=1;
    if(ctx->led.increment>255)ctx->led.increment=255;

    /* Максимальное свечение. По умолчанию 255. 0-255.
    */
    ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(ctx->led.maxBright>255)ctx->led.maxBright=255;
    if(ctx->led.maxBright<0)ctx->led.maxBright=0;

    /* Минимальное свечение. По умолчанию 0. 0-255.
    */
    ctx->led.minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(ctx->led.minBright<0)ctx->led.minBright=0;
    if(ctx->led.minBright>255)ctx->led.minBright=255;

    /* Период обновления состояния светодиода в мс, по умолчанию 30 Гц
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 1000/30, 1, 4096));
    ESP_LOGD(TAG, "Calculated refresh period: %d ms for slot %d", ctx->led.refreshPeriod, slot_num); 	
    /* Количество позиций светового эффекта. По умолчанию равно числу светодиодов. 1-4096.
    */        
    ctx->led.numOfPos = get_option_int_val(slot_num, "numOfPos", "", ctx->led.num_of_led, 1, 4096);

    /* Состояние при запуске. По умолчанию 0 (выключено).
    */
    ctx->led.state = get_option_int_val(slot_num, "ledDefaultState", "", 0, 0, 1) ^ ctx->led.inverse;

    /* Инверсия направления эффекта. Без флага заполнение идёт от 0 к numOfLed, с флагом - от конца к началу
    */
    ctx->led.dir = get_option_flag_val(slot_num, "dirInverse") ? -1 : 1;

    /* Смещение светового эффекта. По умолчанию 0.
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

    /* Начальный цвет в формате R G B. По умолчанию 0 0 255 (синий).
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    {
		char t_str[strlen(me_config.deviceName)+strlen("/led_0")+3];
		sprintf(t_str, "%s/led_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	}
   
        /* === COMMANDS === */

    /* Включить (1) или выключить (0) модуль. */
    stdcommand_register(&ctx->led.cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное. Без параметров.
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_toggleLedState, "action/toggleLedState", PARAMT_none);

    /* Команда задаёт цвет подсветки. Три параметра R G B 0-255.
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_setRGB, "action/setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Команда задаёт положение светового эффекта. Один параметр - номер позиции.
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_setPos, "action/setPos", PARAMT_int);



}

static uint8_t colorChek(uint8_t currentColor, uint8_t targetColor, uint8_t increment){
    if(currentColor<targetColor){
        if(targetColor-currentColor>increment) return currentColor+increment;
        else return targetColor;
    }else if(currentColor>targetColor){
        if(currentColor-targetColor>increment) return currentColor-increment;
        else return targetColor;
    }else return targetColor;
}

void update_led_bar(PLEDCONFIG c, uint8_t *pixels, uint8_t *current_bright_mass, uint8_t *target_bright_mass, rmt_led_heap_t *rmt_heap, int slot_num, RgbColor *currentRGB, int *targetPos, uint8_t *prevState)
{
    bool flag_ledUpdate = false;
    bool needRecalc = false;

    // Пересчёт target_bright_mass при изменении state или targetPos
    if (c->state != *prevState) {
        *prevState = c->state;
        needRecalc = true;
        ESP_LOGD(TAG, "slot%d state changed to %d", slot_num, c->state);
    }

    if (c->state == 0) {
        // state=0: все светодиоды на minBright
        if (needRecalc) {
            for (int i = 0; i < c->num_of_led; i++) {
                target_bright_mass[i] = c->minBright;
            }
        }
    } else {
        // state=1: шкала заполнена от начала до targetPos
        static int prevTargetPos[10] = {[0 ... 9] = -1};
        if (needRecalc || *targetPos != prevTargetPos[slot_num]) {
            prevTargetPos[slot_num] = *targetPos;
            float ledToPosRatio = (float)c->num_of_led / c->numOfPos;
            float ledPos = ledToPosRatio * (*targetPos);
            ESP_LOGD(TAG, "slot%d recalc targetPos=%d ledPos=%.2f numLed=%d numPos=%d",
                     slot_num, *targetPos, ledPos, c->num_of_led, c->numOfPos);
            for (int i = 0; i < c->num_of_led; i++) {
                if (i < (int)ledPos) {
                    target_bright_mass[i] = c->maxBright;
                } else if (i == (int)ledPos && i > 0) {
                    float ratio = ledPos - (int)ledPos;
                    target_bright_mass[i] = (uint8_t)(c->maxBright * ratio);
                    if (target_bright_mass[i] < c->minBright) target_bright_mass[i] = c->minBright;
                } else {
                    target_bright_mass[i] = c->minBright;
                }
            }
            //ESP_LOG_BUFFER_HEX_LEVEL(TAG, target_bright_mass, c->num_of_led, ESP_LOG_DEBUG);
        }
    }

    // Плавное изменение цвета
    if (memcmp(currentRGB, &c->targetRGB, sizeof(RgbColor))) {
        currentRGB->r = colorChek(currentRGB->r, c->targetRGB.r, c->increment);
        currentRGB->g = colorChek(currentRGB->g, c->targetRGB.g, c->increment);
        currentRGB->b = colorChek(currentRGB->b, c->targetRGB.b, c->increment);
        flag_ledUpdate = true;
    }

    // Прямой проход: плавное нарастание яркости от начала к targetPos
    for (int i = 0; i < c->num_of_led; i++) {
        if (target_bright_mass[i] > current_bright_mass[i]) {
            flag_ledUpdate = true;
            if (i > 0 && current_bright_mass[i-1] != target_bright_mass[i-1]) break;
            if (abs(target_bright_mass[i] - current_bright_mass[i]) < c->increment)
                current_bright_mass[i] = target_bright_mass[i];
            else
                current_bright_mass[i] += c->increment;
            break;
        }
    }

    // Обратный проход: плавное уменьшение яркости от конца к targetPos
    for (int i = c->num_of_led - 1; i >= 0; i--) {
        if (target_bright_mass[i] < current_bright_mass[i]) {
            flag_ledUpdate = true;
            if (i < c->num_of_led - 1 && current_bright_mass[i+1] != target_bright_mass[i+1]) break;
            if (abs(target_bright_mass[i] - current_bright_mass[i]) < c->increment)
                current_bright_mass[i] = target_bright_mass[i];
            else
                current_bright_mass[i] -= c->increment;
            break;
        }
    }

    // Рендеринг пикселей с учётом offset и dirInverse
    if (flag_ledUpdate || c->periodicUpdate) {
        for (int i = 0; i < c->num_of_led; i++) {
            int index;
            if (c->dir < 0)
                index = (c->num_of_led - 1 - i + c->offset) % c->num_of_led;
            else
                index = (i + c->offset) % c->num_of_led;
            float tmpBright = (float)current_bright_mass[i] / 255;
            pixels[index * 3]     = gamma_8[(uint8_t)(currentRGB->r * tmpBright)];
            pixels[index * 3 + 1] = gamma_8[(uint8_t)(currentRGB->g * tmpBright)];
            pixels[index * 3 + 2] = gamma_8[(uint8_t)(currentRGB->b * tmpBright)];
        }
        rmt_createAndSend(rmt_heap, pixels, c->num_of_led * 3, slot_num);
    }
}

void button_ledBar_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    if (!ctx) { mblog(E, "ledBar ctx alloc fail slot:%d", slot_num); vTaskDelete(NULL); }
    setup_button_hw(slot_num, ctx);
    configure_button_ledBar(ctx, slot_num);

    if (rmt_semaphore == NULL) {
        rmt_semaphore = xSemaphoreCreateCounting(1, 1);
    }

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    // Pixel + brightness buffers in PSRAM (RMT here is non-DMA, so PSRAM is
    // safe) to keep the task stack small.
    size_t pixels_size = ctx->led.num_of_led * 3;
    size_t mass_size = ctx->led.num_of_led;
    uint8_t *pixels = heap_caps_calloc(1, pixels_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) pixels = calloc(1, pixels_size);
    uint8_t *current_bright_mass = heap_caps_calloc(1, mass_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (current_bright_mass == NULL) current_bright_mass = calloc(1, mass_size);
    uint8_t *target_bright_mass = heap_caps_calloc(1, mass_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (target_bright_mass == NULL) target_bright_mass = calloc(1, mass_size);
    if (pixels == NULL || current_bright_mass == NULL || target_bright_mass == NULL) {
        mblog(E, "ledBar buffers alloc fail slot:%d", slot_num);
        free(pixels); free(current_bright_mass); free(target_bright_mass); free(ctx); vTaskDelete(NULL);
    }

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    int prev_button_state = -1;
    RgbColor currentRGB = {0, 0, 0};
    int targetPos = 0;
    uint8_t prevState = 255; // force recalc on first cycle

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        switch (stdcommand_receive(&ctx->led.cmds, &params, 0)) {
            case STDCMD_ENABLE:
                if (params.count > 0) {
                    // enable controls the LED on/off, not the button
                    ctx->led.state = params.p[0].i ? 1 : 0;
                    ESP_LOGD(TAG, "[button_ledBar_%d] ledEnable:%d", slot_num, ctx->led.state);
                }
                break;
            case LEDBAR_toggleLedState:
                ctx->led.state = !ctx->led.state;
                break;
            case LEDBAR_setRGB:
                ctx->led.targetRGB.r = params.p[0].i;
                ctx->led.targetRGB.g = params.p[1].i;
                ctx->led.targetRGB.b = params.p[2].i;
                break;
            case LEDBAR_setPos:
                targetPos = params.p[0].i;
                if(targetPos < 0) targetPos = 0;
                if(targetPos > ctx->led.numOfPos) targetPos = ctx->led.numOfPos;
                break;
        }

        // Button is always polled - enable controls only the LED.
        // Drain edge interrupts so the queue does not overflow; the
        // non-blocking debounce filter below decides the accepted level.
        uint8_t msg;
        while (xQueueReceive(me_state.interrupt_queue[slot_num], &msg, 0) == pdPASS) {}

        int button_raw = gpio_get_level(pin_in);
        int button_level = (ctx->button.button_inverse ? !button_raw : button_raw);
        int button_state = button_logic_debounce(&ctx->button, button_level);
        button_logic_update(&ctx->button, button_state, slot_num, &prev_button_state);

        update_led_bar(&ctx->led, pixels, current_bright_mass, target_bright_mass, &rmt_heap, slot_num, &currentRGB, &targetPos, &prevState);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_ledBar_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_ledBar_%d", slot_num);
    xTaskCreate(button_ledBar_task, tmpString, 1024*6, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_ledBar()
{
	return manifesto;
}
