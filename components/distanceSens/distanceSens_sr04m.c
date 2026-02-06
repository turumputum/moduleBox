// ***************************************************************************
// TITLE
//      Ultrasonic Distance Sensor Module
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include "distanceSens.h"

#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"


#include <generated_files/gen_distanceSens_sr04m.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DISTANCE_SENS";

extern void distanceSens_config(distanceSens_t *distanceSens, uint8_t slot_num);
extern void distanceSens_report(distanceSens_t *distanceSens, uint8_t slot_num);

typedef struct {
    uint32_t level : 1;
    int64_t time;
} interrupt_data_t;

static QueueHandle_t interrupt_queue;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    interrupt_data_t interrupt_data;
    interrupt_data.level = gpio_get_level((gpio_num_t)arg);
    interrupt_data.time = esp_timer_get_time();
    xQueueSendFromISR(interrupt_queue, &interrupt_data, NULL);
}

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

/* 
    Модуль ультразвукового датчика расстояния sr04m
    Поддерживает измерение расстояния до 400см
*/
void configure_sr04m(distanceSens_t *distanceSens, uint8_t slot_num)
{
    // --- Distance sensor config ---
    
    /* Мертвая зона - минимальное изменение значения для отправки рапорта
    Числовое значение 1-4096, по умолчанию 10
    */
    if (strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
        distanceSens->deadBand = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4096);
        if (distanceSens->deadBand <= 0) {
            ESP_LOGD(TAG, "Ultrasonic dead_band wrong format, set default slot:%d", distanceSens->deadBand);
            distanceSens->deadBand = 1;
        } else {
            ESP_LOGD(TAG, "Ultrasonic set dead_band:%d for slot:%d", distanceSens->deadBand, slot_num);
        }
    }

    /* Флаг включает вывод значений в формате float (0.0-1.0)
    Без параметров
    */
    if (strstr(me_config.slot_options[slot_num], "floatOutput") != NULL) {
        distanceSens->flag_float_output = 1;
        ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
    }

    /* Максимальное значение диапазона измерений
    Числовое значение 1-4096, по умолчанию 400см
    */
    if (strstr(me_config.slot_options[slot_num], "maxVal") != NULL) {
        distanceSens->maxVal = get_option_int_val(slot_num, "maxVal", "", 400, 1, 4096);
        ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", distanceSens->maxVal, slot_num);
    }

    /* Минимальное значение диапазона измерений
    Числовое значение 1-4096, по умолчанию 0
    */
    if (strstr(me_config.slot_options[slot_num], "minVal") != NULL) {
        distanceSens->minVal = get_option_int_val(slot_num, "minVal", "", 0, 0, 4096);
        ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", distanceSens->minVal, slot_num);
    }

    /* Задержка между отправкой рапортов (антидребезг)
    Числовое значение 1-4096мс, по умолчанию 0
    */
    if (strstr(me_config.slot_options[slot_num], "debounceGap") != NULL) {
        distanceSens->debounceGap = get_option_int_val(slot_num, "debounceGap", "", 10, 1, 4096);
        ESP_LOGD(TAG, "Set debounceGap:%ld. Slot:%d", distanceSens->debounceGap, slot_num);
    }

    /* Порог для бинарного выхода (вкл/выкл)
    При значении 0 работает как аналоговый датчик
    Числовое значение 0-4096, по умолчанию 0
    */
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        distanceSens->threshold = get_option_int_val(slot_num, "threshold", "", 0, 0, 4096);
        if (distanceSens->threshold <= 0) {
            ESP_LOGE(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
            distanceSens->threshold = 0;
        } else {
            ESP_LOGD(TAG, "threshold:%d. Slot:%d", distanceSens->threshold, slot_num);
        }
    }

    /* Время cooldown после срабатывания порога (мс)
    Числовое значение 0-60000мс, по умолчанию 0
    */
    if (strstr(me_config.slot_options[slot_num], "cooldownTime") != NULL) {
        distanceSens->cooldownTime = pdMS_TO_TICKS(get_option_int_val(slot_num, "cooldownTime", "", 0, 0, 60000));
        if (distanceSens->cooldownTime > 0) {
            ESP_LOGD(TAG, "cooldownTime:%ld ms. Slot:%d", pdTICKS_TO_MS(distanceSens->cooldownTime), slot_num);
        }
    }

    /* Коэффициент фильтра сглаживания (0.0-1.0)
    При значении 1.0 фильтр отключен
    Числовое значение с плавающей точкой, по умолчанию 1.0
    */
    if (strstr(me_config.slot_options[slot_num], "filterK") != NULL) {
        distanceSens->k = get_option_float_val(slot_num, "filterK", 1);
        ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", distanceSens->k, slot_num);
    }

    /* Флаг инверсии выходного значения
    Без параметров
    */
    if (strstr(me_config.slot_options[slot_num], "inverse") != NULL) {
        distanceSens->inverse = 1;
    }

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
        custom_topic = get_option_string_val(slot_num, "topic", "/distanceSens_0");
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
        ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
        char t_str[strlen(me_config.deviceName)+strlen("/distanceSens_0")+3];
        sprintf(t_str, "%s/distanceSens_%d",me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
        ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);  
    }

    // --- LEDC config for visual indication ---
    distanceSens->ledc_chan.channel = get_next_ledc_channel();
    if (distanceSens->ledc_chan.channel < 0) {
        char errorString[50];
        sprintf(errorString, "slot num:%d ___ LEDC channels has ended", slot_num);
        ESP_LOGE(TAG, "%s", errorString);
        vTaskDelay(20);
        vTaskDelete(NULL);
    }

    if (distanceSens->ledc_chan.channel == 0) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_MODE,
            .timer_num = LEDC_TIMER,
            .duty_resolution = LEDC_DUTY_RES,
            .freq_hz = LEDC_FREQUENCY,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        ESP_LOGD(TAG, "LEDC timer inited");
    }

    distanceSens->ledc_chan.speed_mode = LEDC_MODE;
    distanceSens->ledc_chan.timer_sel = LEDC_TIMER;
    distanceSens->ledc_chan.intr_type = LEDC_INTR_DISABLE;
    distanceSens->ledc_chan.gpio_num = SLOTS_PIN_MAP[slot_num][2];
    distanceSens->ledc_chan.duty = 0;
    distanceSens->ledc_chan.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&distanceSens->ledc_chan));

    // --- Report registration ---
    
    /* Рапортует текущее значение расстояния от ультразвукового датчика
    В режиме threshold отправляет 0/1
    В режиме аналогового датчика отправляет расстояние в см или float (0.0-1.0)
    */
    if (distanceSens->flag_float_output) {
        distanceSens->distanceReport = stdreport_register(RPTT_ratio, slot_num, "percent", "distance", 0.0f, 1.0f);
    } else {
        distanceSens->distanceReport = stdreport_register(RPTT_int, slot_num, "cm", "distance", 0, distanceSens->maxVal);
    }
}

void sr04m_task(void* arg) {
    int slot_num = *(int*)arg;

    distanceSens_t distanceSens = DISTANCE_SENS_DEFAULT();
    distanceSens.maxVal = 400;
    configure_sr04m(&distanceSens, slot_num);

    gpio_num_t trigger_pin = (gpio_num_t)SLOTS_PIN_MAP[slot_num][1];
    gpio_num_t echo_pin = (gpio_num_t)SLOTS_PIN_MAP[slot_num][0];

    gpio_config_t trigger_conf = {
        .pin_bit_mask = (1ULL << trigger_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&trigger_conf);

    gpio_config_t echo_conf = {
        .pin_bit_mask = (1ULL << echo_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&echo_conf);

    interrupt_queue = xQueueCreate(50, sizeof(interrupt_data_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(echo_pin, gpio_isr_handler, (void*)echo_pin);

    interrupt_data_t interrupt_data;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t last_trigger_time = 0;

    waitForWorkPermit(slot_num);

    while (1) {
        if(esp_timer_get_time()-last_trigger_time>100000){
            gpio_set_level(trigger_pin, 1);
            esp_rom_delay_us(10);
            last_trigger_time=esp_timer_get_time();
        }
        
        gpio_set_level(trigger_pin, 0);

        if (xQueueReceive(interrupt_queue, &interrupt_data, pdMS_TO_TICKS(0)) == pdTRUE) {
            if (interrupt_data.level == 0) {
                start_time = interrupt_data.time;
            } else if (interrupt_data.level == 1 && start_time > 0) {
                end_time = interrupt_data.time;
                distanceSens.currentPos = (end_time - start_time) / 58;
                distanceSens_report(&distanceSens, slot_num);
                start_time = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void start_sr04m_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(sr04m_task, "sr04m_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "sr04m_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_distanceSens_sr04m()
{
	return manifesto;
}