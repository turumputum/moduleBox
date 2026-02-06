#include <stdio.h>
#include "distanceSens.h"

#include <stdint.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include <mbdebug.h>

#include <generated_files/gen_distanceSens_tofxxxf_uart.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DISTANCE_SENS";

extern uint16_t crc16_modbus(uint8_t *data, uint8_t length);
extern void distanceSens_config(distanceSens_t *distanceSens, uint8_t slot_num);
extern void distanceSens_report(distanceSens_t *distanceSens, uint8_t slot_num);

/*  Модуль датчика расстояния TOFxxxF через UART
    Модель TOF050F измеряет расстояния в миллиметрах до 500мм
    Модель TOF200F измеряет расстояния в миллиметрах до 2000мм
    Модель TOF400F измеряет расстояния в миллиметрах до 4000мм
*/
void configure_TOFxxxF(distanceSens_t *distanceSens, uint8_t slot_num)
{
    // --- Distance sensor config ---
    
    /* Мертвая зона - минимальное изменение значения для отправки рапорта
    Числовое значение 1-4096, по умолчанию 10
    */
    if (strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
        distanceSens->deadBand = get_option_int_val(slot_num, "deadBand", "", 0, 0, 4096);
        if (distanceSens->deadBand <= 0) {
            ESP_LOGD(TAG, "TOF dead_band wrong format, set default slot:%d", distanceSens->deadBand);
            distanceSens->deadBand = 1;
        } else {
            ESP_LOGD(TAG, "TOF set dead_band:%d for slot:%d", distanceSens->deadBand, slot_num);
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
    Числовое значение 1-4096, по умолчанию 4096
    */
    distanceSens->maxVal = get_option_int_val(slot_num, "maxVal", "", 4096, 1, 4096);
    ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", distanceSens->maxVal, slot_num);


    /* Минимальное значение диапазона измерений
    Числовое значение 1-65536, по умолчанию 0
    */
    distanceSens->minVal = get_option_int_val(slot_num, "minVal", "", 0, 0, 4095);
    ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", distanceSens->minVal, slot_num);


    /* Задержка между отправкой рапортов (антидребезг)
    Числовое значение 1-4096мс, по умолчанию 0
    */
    distanceSens->debounceGap = get_option_int_val(slot_num, "debounceGap", "", 10, 1, 4096);
    ESP_LOGD(TAG, "Set debounceGap:%ld. Slot:%d", distanceSens->debounceGap, slot_num);
    

    /* Порог для бинарного выхода (вкл/выкл)
    При значении 0 работает как аналоговый датчик
    Числовое значение 0-65536, по умолчанию 0
    */
    if (strstr(me_config.slot_options[slot_num], "threshold") != NULL) {
        distanceSens->threshold = get_option_int_val(slot_num, "threshold", "", 0, 0, 65536);
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
    
    /* Рапортует текущее значение расстояния
    В режиме threshold отправляет 0/1
    В режиме аналогового датчика отправляет расстояние в мм или float (0.0-1.0)
    */
    if (distanceSens->flag_float_output) {
        distanceSens->distanceReport = stdreport_register(RPTT_ratio, slot_num, "percent", "distance", 0.0f, 1.0f);
    } else {
        distanceSens->distanceReport = stdreport_register(RPTT_int, slot_num, "mm", "distance", 0, distanceSens->maxVal);
    }
}

void TOFxxxF_task(void* arg) {
    int slot_num = *(int*)arg;
    
    int uart_num = UART_NUM_1;
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            char errorString[50];
            sprintf(errorString,  "slot num:%d ___ No free UART driver", slot_num);
            ESP_LOGE(TAG, "%s", errorString);
            mblog(E, errorString);
            vTaskDelay(200);
            vTaskDelete(NULL);
        }
    }

    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    #define BUF_SIZE 256
    uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0);

    distanceSens_t distanceSens=DISTANCE_SENS_DEFAULT();
    configure_TOFxxxF(&distanceSens, slot_num);

    uint8_t rawByte[10];
    uint8_t f_msg_start = 0;
    uint8_t f_report = 0;
    uint8_t index = 0;

    waitForWorkPermit(slot_num);
    //ESP_LOGD(TAG, "TOFxxxF_task started for slot:%d maxVal%d", slot_num, distanceSens.maxVal);
    while (1) {
        uint16_t len;
        char str[255];

        len = uart_read_bytes(uart_num, &rawByte[index], 1, portMAX_DELAY);
        if (len > 0) {
            if (f_msg_start == 0) {
                if (rawByte[index] == 0x01) {
                    index++;
                    f_msg_start = 1;
                }
            } else if (index == 1) {
                if (rawByte[index] == 0x03) {
                    index++;
                } else {
                    index = 0;
                    f_msg_start = 0;
                }
            } else {
                index++;
                if (index == 7) {
                    if (crc16_modbus(&rawByte,5) == (rawByte[6] << 8 | rawByte[5])) {
                        int16_t receivedDistance = (rawByte[3] << 8) | rawByte[4];
                        if(receivedDistance>0){
                            distanceSens.currentPos = abs((rawByte[3] << 8) | rawByte[4]);
                        }else{
                            distanceSens.currentPos = distanceSens.maxVal;
                        }
                        // if(distanceSens.currentPos>65536){
                        //     distanceSens.currentPos=65536;
                        // }
                        distanceSens_report(&distanceSens, slot_num);
                    }
                    index = 0;
                    f_msg_start = 0;
                }
            }
        }
    }
}

void start_tofxxxfuart_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(TOFxxxF_task, "TOFxxxF_task", 1024 * 4, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "TOFxxxF_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_distanceSens_tofxxxf_uart()
{
	return manifesto;
}