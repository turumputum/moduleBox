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

#include <generated_files/gen_button_swiperLed.h>

static const char *TAG = "BUTTON_LEDS";
#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

typedef enum {
    WAITING = 0,
    SWIPE_UP,
    SWIPE_DOWN,
    SWIPE_LEFT,
    SWIPE_RIGHT
} LED_EFFECT;

typedef enum {
    LED_STOP = 0,
    LED_RUN = 2
} LED_STATE;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// --- button_swiperLed ---
typedef enum
{
    SWIPERLED_default = 0,
    SWIPERLED_toggleLedState,
    SWIPERLED_setRGB,
    SWIPERLED_swipe,
} SWIPERLED_CMD;

/* 
    Модуль кнопка со свайп-подсветкой
    slots: 0-5
*/
void configure_button_swiperLed(PMODULE_CONTEXT ctx, int slot_num)
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

    // --- Swiper LED logic config ---
    /* Количество светодиодов в кольце. По умолчанию 16. 1-1024.
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 16, 1, 1024);

    /* Максимальное свечение. По умолчанию 255. 0-255.
    */
    ctx->led.maxBright = (float)get_option_int_val(slot_num, "maxBright", "", 255, 0, 255)/255;

    /* Минимальное свечение. По умолчанию 0. 0-255.
    */
    ctx->led.minBright = (float)get_option_int_val(slot_num, "minBright", "", 0, 0, 255)/255;

    /* Период обновления состояния светодиода в мс, по умолчанию 25 Гц
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 25, 1, 1024));

    /* Начальный цвет в формате R G B. По умолчанию 0 0 255 (синий).
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    /* Состояние при запуске. По умолчанию 0 (выключено).
    */
    ctx->led.inverse = 0;
    ctx->led.state = (get_option_int_val(slot_num, "ledDefaultState", "", 0, 0, 1) != 0 ? 1 : 0) ^ ctx->led.inverse;

    /* Смещение светового эффекта. По умолчанию 0.
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

    {
		char t_str[strlen(me_config.deviceName)+strlen("/led_0")+3];
		sprintf(t_str, "%s/led_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	}



    /* Команда меняет текущее состояние светодиода на противоположное. Без параметров.
    */
    stdcommand_register(&ctx->led.cmds, SWIPERLED_toggleLedState, "action/toggleLedState", PARAMT_none);

    /* Команда задаёт цвет подсветки. Три параметра R G B 0-255.
    */
   stdcommand_register(&ctx->led.cmds, SWIPERLED_setRGB, "action/setRGB", PARAMT_int, PARAMT_int, PARAMT_int);


    /* Команда запускает световой эффект свайпа. Значения - up, down, left, right.
    */
    stdcommand_register_enum(&ctx->led.cmds, SWIPERLED_swipe, "action/swipe", "up", "down", "left", "right");

    /* === COMMANDS === */

    /* Включить (1) или выключить (0) модуль. */
    stdcommand_register(&ctx->led.cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

}

static void setMinBright(swiper_handle_t *swiperLed) {
	swiperLed->HSV = RgbToHsv(swiperLed->RGB);
	swiperLed->HSV.v = 255*swiperLed->minBright;
	RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
	for (int i = 0; i < swiperLed->num_led; i++) {
        led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
	}
}

static void processLedEffect(swiper_handle_t *swiperLed) {
	swiperLed->tick++;
	if (swiperLed->tick >= swiperLed->tickLenght) {
		swiperLed->state = LED_STOP;
		swiperLed->effect = WAITING;
		setMinBright(swiperLed);
	}
	if ((swiperLed->effect >= SWIPE_UP) && (swiperLed->effect <= SWIPE_RIGHT)) { // SWIPES
		int tmp = swiperLed->effectBuf[swiperLed->effectBufLen - 1];
		for (int i = 0; i < swiperLed->effectBufLen - 1; i++) {
			swiperLed->effectBuf[swiperLed->effectBufLen - i - 1] = swiperLed->effectBuf[swiperLed->effectBufLen - i - 2];
		}
		swiperLed->effectBuf[0] = tmp;
		for (int i = 0; i < swiperLed->num_led / 2; i++) {
			swiperLed->ledBrightMass[i] = swiperLed->effectBuf[swiperLed->ledsCoordinate[i]];
			swiperLed->ledBrightMass[swiperLed->num_led - 1 - i] = swiperLed->effectBuf[swiperLed->ledsCoordinate[i]];
		}
		int rotateNum= 0;
		if (swiperLed->effect == SWIPE_DOWN) {
			rotateNum = 0;
		} else if (swiperLed->effect == SWIPE_LEFT) {
			rotateNum = swiperLed->num_led / 4;
		} else if (swiperLed->effect == SWIPE_UP) {
			rotateNum = swiperLed->num_led / 2;
		} else if (swiperLed->effect == SWIPE_RIGHT) {
			rotateNum = swiperLed->num_led * 3 / 4;
		}
		rotateNum = (rotateNum + swiperLed->offset) % swiperLed->num_led;
		for (int y = 0; y < rotateNum; y++) {
			uint8_t tmp = swiperLed->ledBrightMass[swiperLed->num_led - 1];
			for (int i = 1; i < swiperLed->num_led + 1; i++) {
				swiperLed->ledBrightMass[swiperLed->num_led - i] = swiperLed->ledBrightMass[swiperLed->num_led - 1 - i];
			}
			swiperLed->ledBrightMass[0] = tmp;
		}
		for (int i = 0; i < swiperLed->num_led; i++) {
			swiperLed->HSV.v = swiperLed->ledBrightMass[i];
			RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		}
	}
}

void update_led_swiper(PLEDCONFIG c, swiper_handle_t *swiper, rmt_led_heap_t *rmt_heap, int slot_num, uint8_t *prevState)
{
    bool changed = false;
    if(c->state == 1){
        if(c->state != *prevState){
            *prevState = c->state;
            swiper->HSV.v = 255 * swiper->maxBright;
            RgbColor tmpRGB = HsvToRgb(swiper->HSV);
            for(int i=0; i<swiper->num_led; i++){
                led_strip_set_pixel(swiper->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            }
            changed = true;
        }
    }else{
        if(c->state != *prevState){
            *prevState = c->state;
            if(swiper->state == LED_RUN){
                swiper->state = LED_STOP;
                swiper->effect = WAITING;
            }
            setMinBright(swiper);
            changed = true;
        }else if(swiper->state == LED_RUN){
            processLedEffect(swiper);
            changed = true;
        }
    }
    if(changed){
        rmt_createAndSend(rmt_heap, swiper->pixelBuffer, swiper->num_led * 3, slot_num);
    }
}

void button_swiperLed_task(void *arg)
{
    int slot_num = (int)(intptr_t)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    if (!ctx) { mblog(E, "swiperLed ctx alloc fail slot:%d", slot_num); vTaskDelete(NULL); }
    setup_button_hw(slot_num, ctx);
    configure_button_swiperLed(ctx, slot_num);

    if (rmt_semaphore == NULL) rmt_semaphore = xSemaphoreCreateCounting(1, 1);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    // Pixel buffer in PSRAM (RMT here is non-DMA, so PSRAM is safe).
    size_t pixels_size = ctx->led.num_of_led * 3;
    uint8_t *pixels = heap_caps_calloc(1, pixels_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) pixels = calloc(1, pixels_size);
    if (pixels == NULL) { mblog(E, "swiperLed pixels alloc fail slot:%d", slot_num); free(ctx); vTaskDelete(NULL); }

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    swiper_handle_t swiper = {0};
    swiper.num_led = ctx->led.num_of_led;
    swiper.pixelBuffer = pixels;
    swiper.maxBright = ctx->led.maxBright;
    swiper.minBright = ctx->led.minBright;
    swiper.RGB = ctx->led.targetRGB;
    swiper.HSV = RgbToHsv(ctx->led.targetRGB);
    swiper.ledBrightMass = malloc(swiper.num_led);
    swiper.ledAngleRadian = 2 * M_PI / swiper.num_led;
    swiper.ledsCoordinate = malloc(swiper.num_led / 2 * sizeof(uint16_t));
    if (swiper.ledBrightMass == NULL || swiper.ledsCoordinate == NULL) {
        mblog(E, "swiperLed alloc fail slot:%d", slot_num);
        free(swiper.ledBrightMass); free(swiper.ledsCoordinate); free(pixels); free(ctx); vTaskDelete(NULL);
    }
    swiper.state = LED_STOP;
    swiper.offset = ctx->led.offset;

    setMinBright(&swiper);
    rmt_createAndSend(&rmt_heap, pixels, ctx->led.num_of_led * 3, slot_num);

    uint8_t prevState = 0;
    int prev_button_state = -1;

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);
        if(cmd>=0)ESP_LOGD(TAG, "Button Swiper LED Slot %d: received command %d param:%ld", slot_num, cmd, params.p[0].i);

        switch (cmd) {
            case STDCMD_ENABLE:
                if (params.count > 0) {
                    // enable controls the LED on/off, not the button
                    ctx->led.state = params.p[0].i ? 1 : 0;
                    ESP_LOGD(TAG, "[button_swiperLed_%d] ledEnable:%d", slot_num, ctx->led.state);
                }
                break;
            case SWIPERLED_toggleLedState:
                ctx->led.state = !ctx->led.state;
                ESP_LOGD(TAG, "Swiper LED Slot %d: toggled state to %d", slot_num, ctx->led.state);
                break;
            case SWIPERLED_swipe:
                swiper.state = LED_RUN;
                swiper.tick = 0;
                swiper.tickLenght = 30;
                swiper.effect = SWIPE_UP + params.enumResult; // Map to SWIPE_UP...
                // Simplified effect init
                swiper.frontLen = swiper.tickLenght / 2;
                swiper.diameter = swiper.tickLenght - swiper.frontLen;
                swiper.effectBufLen = swiper.tickLenght + swiper.frontLen;
                if(swiper.effectBuf) free(swiper.effectBuf);
                swiper.effectBuf = malloc(swiper.effectBufLen);
                memset(swiper.effectBuf, 0, swiper.effectBufLen);
                for(int i=0; i<swiper.frontLen; i++) swiper.effectBuf[i] = 255 * (sin(i * M_PI / swiper.frontLen) * (swiper.maxBright - swiper.minBright) + swiper.minBright);
                uint16_t q = swiper.num_led / 4;
                for(int t=0; t<q; t++) swiper.ledsCoordinate[t] = swiper.frontLen + (swiper.diameter/2 - swiper.diameter/2 * cos(swiper.ledAngleRadian/2 + swiper.ledAngleRadian*t));
                for(int t=0; t<q; t++) swiper.ledsCoordinate[q+t] = swiper.frontLen + (swiper.diameter/2) + (swiper.frontLen + swiper.diameter/2 - swiper.ledsCoordinate[q-t-1]);
                ESP_LOGD(TAG, "Swiper LED Slot %d: started swipe effect %d", slot_num, swiper.effect);
                break;
            case SWIPERLED_setRGB:
                swiper.RGB.r = params.p[0].i;
                swiper.RGB.g = params.p[1].i;
                swiper.RGB.b = params.p[2].i;
                swiper.HSV = RgbToHsv(swiper.RGB);
                ESP_LOGD(TAG, "Swiper LED Slot %d: set RGB to %d %d %d", slot_num, swiper.RGB.r, swiper.RGB.g, swiper.RGB.b);
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

        update_led_swiper(&ctx->led, &swiper, &rmt_heap, slot_num, &prevState);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_swiperLed_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_swiperLed_%d", slot_num);
    xTaskCreate(button_swiperLed_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_swiperLed()
{
	return manifesto;
}