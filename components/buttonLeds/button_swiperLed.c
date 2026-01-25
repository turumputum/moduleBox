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
#include "me_slot_config.h"
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
*/
void configure_button_swiperLed(PMODULE_CONTEXT ctx, int slot_num)
{
    stdcommand_init(&ctx->led.cmds, slot_num);

    // --- Button logic config ---
    /* Флаг определяет инверсию кнопки
    */
    ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");

    /* Глубина фильтра от дребезга
    */
    ctx->button.debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "", 10, 1, 4096);

    /* Продолжительность длинного нажатия
    */
    ctx->button.longPressTime 	= get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 65535);

    /* Длительность промежутка между нажатиями для регистрации двойного нажатия
    */
    ctx->button.doubleClickTime = get_option_int_val(slot_num, "doubleClickTime", "ms", 0, 0, 65535);

    /* Флаг задаёт фильтрацию совытий при активных
    */
    ctx->button.event_filter = get_option_flag_val(slot_num, "eventFilter");

    /* Период обновления
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 40, 1, 4096));

    if (strstr(me_config.slot_options[slot_num], "buttonTopic") != NULL) 
    {
        /* Топик для событий кнопки
        */
        char * custom_topic = get_option_string_val(slot_num, "buttonTopic", "/button_0");
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
    }else{
        char t_str[strlen(me_config.deviceName)+strlen("/button_0")+3];
        sprintf(t_str, "%s/button_%d",me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
    }

	/* Рапортует при изменении состояния кнопки
	*/
	ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "unit", nil, 0, 1);

	/* Рапортует при регистрации длинного нажатия
	*/
	ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "unit", "longPress", 0, 1);

	/* Рапортует при регистрации двойного нажатия
	*/
	ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "unit", "doubleClick", 0, 1);

    // --- Swiper LED logic config ---
    /* Количенство светодиодов
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 16, 1, 1024);

    /* Максимальное значение яркости
    */
    ctx->led.maxBright = (float)get_option_int_val(slot_num, "maxBright", "", 255, 0, 255)/255;

    /* Минимальное значение яркости
    */
    ctx->led.minBright = (float)get_option_int_val(slot_num, "minBright", "", 0, 0, 255)/255;

    /* Период обновления 
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 25, 1, 1024));

    /* Начальный цвет
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    /* Состояние по умолчанию
    */
    ctx->led.inverse = 0;
    ctx->led.state = (get_option_int_val(slot_num, "defaultState", "", 0, 0, 1) != 0 ? 1 : 0) ^ ctx->led.inverse;

    /* Смещение эффекта
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledTopic", "/swiperLed_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/swiperLed_%d")+3];
		sprintf(t_str, "%s/swiperLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	} 


    /* задаёт текущее состояние светодиода (вкл/выкл)
    Числовое значение 0-1
    */
    stdcommand_register(&ctx->led.cmds, SWIPERLED_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, SWIPERLED_toggleLedState, "toggleLedState", PARAMT_none);

    /* Установить новый целевой цвет. 
    Цвет задаётся десятичными значениями R G B через пробел
    */
   stdcommand_register(&ctx->led.cmds, SWIPERLED_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);


    /*Команда запускает световой эффект в видде свайпа
    Доступные значения: up, down, left, right
    */
    stdcommand_register_enum(&ctx->led.cmds, SWIPERLED_swipe, "swipe", "up", "down", "left", "right");
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
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    setup_button_hw(slot_num, ctx);
    configure_button_swiperLed(ctx, slot_num);

    if (rmt_semaphore == NULL) rmt_semaphore = xSemaphoreCreateCounting(1, 1);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t pixels[ctx->led.num_of_led * 3];
    memset(pixels, 0, sizeof(pixels));

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
            case SWIPERLED_default:
                ctx->led.state = ((params.p[0].i != 0) ? 1 : 0) ^ ctx->led.inverse;
                ESP_LOGD(TAG, "Swiper LED Slot %d: set state to %d", slot_num, ctx->led.state);
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

        uint8_t msg;
        int button_raw = gpio_get_level(pin_in);
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &msg, 0) == pdPASS) {
            if (ctx->button.debounce_gap > 0) vTaskDelay(ctx->button.debounce_gap);
            button_raw = gpio_get_level(pin_in);
        }
        int button_state = (ctx->button.button_inverse ? !button_raw : button_raw);
        button_logic_update(&ctx->button, button_state, slot_num, &prev_button_state);

        update_led_swiper(&ctx->led, &swiper, &rmt_heap, slot_num, &prevState);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_swiperLed_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_swiperLed_%d", slot_num);
    xTaskCreate(button_swiperLed_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_swiperLed()
{
	return manifesto;
}