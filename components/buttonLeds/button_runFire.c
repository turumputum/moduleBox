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

#include <generated_files/gen_button_runFire.h>

static const char *TAG = "BUTTON_LEDS";
#undef  LOG_LOCAL_LEVEL
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

// --- button_runFire ---
typedef enum
{
    RUNFIRE_default = 0,
    RUNFIRE_toggleLedState,
    RUNFIRE_setRGB,
} RUNFIRE_CMD;

typedef enum
{
    RUNFIRE_MODE_rainbow = 0,
    RUNFIRE_MODE_sin,
    RUNFIRE_MODE_sinAbs,
} RUNFIRE_MODE;

/*
    Модуль кнопка с бегущим световым эффектом
    slots: 0-5
*/
void configure_button_runFire(PMODULE_CONTEXT ctx, int slot_num)
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
    ctx->button.longPressTime = get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 10000);

    /* Длительность промежутка между нажатиями для регистрации двойного нажатия. По умолчанию 0, функция не активна
    */
    ctx->button.doubleClickTime = get_option_int_val(slot_num, "doubleClickTime", "ms", 0, 0, 10000);

    /* Подавляет короткое событие press, когда сработало длинное или двойное нажатие
       Выключен (0, по умолчанию) - короткие события шлются всегда
    */
    ctx->button.event_filter = get_option_flag_val(slot_num, "eventFilter");

    /* Период обновления потока кнопки в мс, по умолчанию 25 (40 Гц)
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "hz", 40, 1, 100));

    {
        char t_str[strlen(me_config.deviceName)+strlen("/button_0")+3];
        sprintf(t_str, "%s/button_%d",me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
    }

    /* Рапортует при изменении состояния кнопки. 0-1.
    */
    ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "state", "event/press", 0, 1);

    /* Рапортует при регистрации длинного нажатия. 0-1.
    */
    ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "state", "event/longPress", 0, 1);

    /* Рапортует при регистрации двойного нажатия. 0-1.
    */
    ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "state", "event/doubleClick", 0, 1);

    // --- RunFire LED logic config ---
    /* Количество светодиодов в ленте. По умолчанию 24. 1-1024.
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

    /* Флаг принудительной отправки буфера каждый цикл
    */
    ctx->led.periodicUpdate = get_option_flag_val(slot_num, "periodicUpdate");

    /* Максимальное свечение. По умолчанию 255. 0-255.
    */
    ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 255);
    if(ctx->led.maxBright>255)ctx->led.maxBright=255;
    if(ctx->led.maxBright<0)ctx->led.maxBright=0;

    /* Минимальное свечение. По умолчанию 0. 0-255.
    */
    ctx->led.minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 255);
    if(ctx->led.minBright<0)ctx->led.minBright=0;
    if(ctx->led.minBright>255)ctx->led.minBright=255;

    /* Величина приращения яркости за цикл - скорость плавного затухания. По умолчанию 5. 1-255.
    */
    ctx->led.increment = get_option_int_val(slot_num, "increment", "", 5, 1, 255);
    if(ctx->led.increment<1)ctx->led.increment=1;
    if(ctx->led.increment>255)ctx->led.increment=255;

    /* Период обновления состояния светодиода в мс, по умолчанию 30 Гц
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 30, 1, 1024));

    /* Состояние при запуске. По умолчанию 0 (выключено).
    */
    ctx->led.state = get_option_int_val(slot_num, "ledDefaultState", "", 0, 0, 1);

    /* Инверсия направления эффекта. Без флага движение идёт от 0 к numOfLed, с флагом - от конца к началу
    */
    ctx->led.dir = get_option_flag_val(slot_num, "ledInverse") ? -1 : 1;

    /* Количество светодиодов в одном логическом пикселе - один расчёт цвета применяется к группе. По умолчанию 1. 1-256.
    */
    ctx->led.ledsPerPixel = get_option_int_val(slot_num, "ledsPerPixel", "", 1, 1, 256);

    /* Длина светового эффекта для sin, sinAbs - число пикселей в одном периоде синусоиды. По умолчанию четверть ленты. 1-4096.
    */
    ctx->led.effectLen = get_option_int_val(slot_num, "effectLen", "", ctx->led.num_of_led / 4, 1, 4096);

    /* Смещение светового эффекта. По умолчанию 0.
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

    /* Начальный цвет в формате R G B. По умолчанию 0 0 255 (синий).
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    /* Задаёт режим анимации. По умолчанию 'rainbow'
    - rainbow — бегущая радуга, каждый пиксель имеет свой оттенок HSV
    - sin — синусоидальная яркость, отрицательная часть = minBright
    - sinAbs — синусоидальная яркость с отражением отрицательной части
    */
    if ((ctx->led.ledMode = get_option_enum_val(slot_num, "animMode", "rainbow", "sin", "sinAbs", NULL)) < 0)
    {
        ESP_LOGE(TAG, "animMode: unrecognized value");
    }

    {
        char t_str[strlen(me_config.deviceName)+strlen("/led_0")+3];
        sprintf(t_str, "%s/led_%d",me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num]=strdup(t_str);
    }


    /* Команда меняет текущее состояние светодиода на противоположное. Без параметров.
    */
    stdcommand_register(&ctx->led.cmds, RUNFIRE_toggleLedState, "action/toggleLedState", PARAMT_none);

    /* Команда задаёт цвет подсветки. Три параметра R G B 0-255.
    */
    stdcommand_register(&ctx->led.cmds, RUNFIRE_setRGB, "action/setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* === COMMANDS === */

    /* Включить (1) или выключить (0) модуль. */
    stdcommand_register(&ctx->led.cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

}

static void ledUpdate(uint8_t *currentMass, uint8_t *targetMass, uint16_t size, uint8_t increment, bool periodicUpdate, rmt_led_heap_t *rmt_heap, uint8_t slot_num) {
    uint16_t sum = 0;
    for(int i=0; i<size; i++){
        if(currentMass[i] != targetMass[i]){
            sum ++;
            if(currentMass[i] < targetMass[i]){
                int16_t temp = currentMass[i] + increment;
                if(temp >= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] += increment;
                }
            }else{
                int16_t temp = currentMass[i] - increment;
                if(temp <= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] -= increment;
                }
            }
        }
    }
    if(sum != 0 || periodicUpdate){
        rmt_createAndSend(rmt_heap, currentMass, size, slot_num);
    }
}

void update_runFire(PLEDCONFIG c, uint8_t *current_pixels, uint8_t *target_pixels, rmt_led_heap_t *rmt_heap, int slot_num, int *phase, uint8_t *prevState)
{
    float fMaxB = (float)c->maxBright / 255.0f;
    float fMinB = (float)c->minBright / 255.0f;

    if (c->state == 1) {
        // --- Активное состояние: сдвигаем массив и рассчитываем новый пиксель ---
        if (c->state != *prevState) {
            *prevState = c->state;
            ESP_LOGD(TAG, "RunFire Slot %d: state ON", slot_num);
        }

        // Сдвигаем массив на ledsPerPixel светодиодов
        uint16_t shiftLeds = c->ledsPerPixel;
        if (shiftLeds > c->num_of_led - 1) shiftLeds = c->num_of_led - 1;
        uint16_t shiftBytes = shiftLeds * 3;
        if (c->dir > 0) {
            // Направление 0 -> numOfLed: сдвигаем вправо, новые пиксели в начале
            memmove(target_pixels + shiftBytes, target_pixels, (c->num_of_led - shiftLeds) * 3);
        } else {
            // Направление numOfLed -> 0: сдвигаем влево, новые пиксели в конце
            memmove(target_pixels, target_pixels + shiftBytes, (c->num_of_led - shiftLeds) * 3);
        }

        // Рассчитываем цвет нового пикселя в зависимости от режима
        uint8_t r, g, b;
        switch (c->ledMode) {
            case RUNFIRE_MODE_rainbow: {
                // Каждый новый пиксель имеет следующий оттенок HSV.h
                HsvColor hsv;
                hsv.h = (uint8_t)((*phase) & 0xFF);
                hsv.s = 255;
                hsv.v = c->maxBright;
                RgbColor rgb = HsvToRgb(hsv);
                r = rgb.r;
                g = rgb.g;
                b = rgb.b;
                (*phase)++;
                break;
            }
            case RUNFIRE_MODE_sin: {
                // Синусоидальная яркость, отрицательная часть = minBright
                float angle = 2.0f * M_PI * (*phase) / c->effectLen;
                float sinVal = sinf(angle);
                float bright;
                if (sinVal > 0) {
                    bright = fMinB + sinVal * (fMaxB - fMinB);
                } else {
                    bright = fMinB;
                }
                r = (uint8_t)(c->targetRGB.r * bright);
                g = (uint8_t)(c->targetRGB.g * bright);
                b = (uint8_t)(c->targetRGB.b * bright);
                (*phase)++;
                break;
            }
            case RUNFIRE_MODE_sinAbs: {
                // Синусоидальная яркость, отрицательная часть отражена (abs)
                float angle = 2.0f * M_PI * (*phase) / c->effectLen;
                float sinVal = fabsf(sinf(angle));
                float bright = fMinB + sinVal * (fMaxB - fMinB);
                r = (uint8_t)(c->targetRGB.r * bright);
                g = (uint8_t)(c->targetRGB.g * bright);
                b = (uint8_t)(c->targetRGB.b * bright);
                (*phase)++;
                break;
            }
            default: {
                r = (uint8_t)(c->targetRGB.r * fMaxB);
                g = (uint8_t)(c->targetRGB.g * fMaxB);
                b = (uint8_t)(c->targetRGB.b * fMaxB);
                (*phase)++;
                break;
            }
        }

        // Вставляем новый пиксель (ledsPerPixel диодов) с учётом offset
        uint8_t gr = gamma_8[g];
        uint8_t rr = gamma_8[r];
        uint8_t br = gamma_8[b];
        for (int p = 0; p < shiftLeds; p++) {
            int newPixelIdx;
            if (c->dir > 0) {
                newPixelIdx = (c->offset + p) % c->num_of_led;
            } else {
                newPixelIdx = (c->num_of_led - 1 - p + c->offset) % c->num_of_led;
            }
            target_pixels[newPixelIdx * 3]     = gr;
            target_pixels[newPixelIdx * 3 + 1] = rr;
            target_pixels[newPixelIdx * 3 + 2] = br;
        }

    } else {
        // --- Неактивное состояние: плавно гасим до minBright ---
        if (c->state != *prevState) {
            *prevState = c->state;
            ESP_LOGD(TAG, "RunFire Slot %d: state OFF", slot_num);
            uint8_t minR = gamma_8[(uint8_t)(c->targetRGB.r * fMinB)];
            uint8_t minG = gamma_8[(uint8_t)(c->targetRGB.g * fMinB)];
            uint8_t minB = gamma_8[(uint8_t)(c->targetRGB.b * fMinB)];
            for (int i = 0; i < c->num_of_led; i++) {
                target_pixels[i * 3]     = minG;
                target_pixels[i * 3 + 1] = minR;
                target_pixels[i * 3 + 2] = minB;
            }
        }
    }

    ledUpdate(current_pixels, target_pixels, c->num_of_led * 3, c->increment, c->periodicUpdate, rmt_heap, slot_num);
}

void button_runFire_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    if (!ctx) { mblog(E, "runFire ctx alloc fail slot:%d", slot_num); vTaskDelete(NULL); }
    setup_button_hw(slot_num, ctx);
    configure_button_runFire(ctx, slot_num);

    if (rmt_semaphore == NULL) rmt_semaphore = xSemaphoreCreateCounting(1, 1);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    // Pixel buffers in PSRAM (RMT here is non-DMA, so PSRAM is safe).
    uint16_t pixel_buf_size = ctx->led.num_of_led * 3;
    uint8_t *current_pixels = heap_caps_calloc(1, pixel_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (current_pixels == NULL) current_pixels = calloc(pixel_buf_size, sizeof(uint8_t));
    uint8_t *target_pixels  = heap_caps_calloc(1, pixel_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (target_pixels == NULL) target_pixels = calloc(pixel_buf_size, sizeof(uint8_t));
    if (current_pixels == NULL || target_pixels == NULL) {
        mblog(E, "runFire pixels alloc fail slot:%d", slot_num);
        free(current_pixels); free(target_pixels); free(ctx); vTaskDelete(NULL);
    }

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    int prev_button_state = -1;
    int phase = 0;
    uint8_t prevState = 255; // force update on first cycle

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);

        switch (cmd) {
            case STDCMD_ENABLE:
                if (params.count > 0) {
                    // enable controls the LED on/off, not the button
                    ctx->led.state = params.p[0].i ? 1 : 0;
                    ESP_LOGD(TAG, "[button_runFire_%d] ledEnable:%d", slot_num, ctx->led.state);
                }
                break;
            case RUNFIRE_toggleLedState:
                ctx->led.state = !ctx->led.state;
                break;
            case RUNFIRE_setRGB:
                ctx->led.targetRGB.r = params.p[0].i;
                ctx->led.targetRGB.g = params.p[1].i;
                ctx->led.targetRGB.b = params.p[2].i;
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

        update_runFire(&ctx->led, current_pixels, target_pixels, &rmt_heap, slot_num, &phase, &prevState);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_runFire_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_runFire_%d", slot_num);
    xTaskCreate(button_runFire_task, tmpString, 1024*6, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_runFire()
{
    return manifesto;
}
