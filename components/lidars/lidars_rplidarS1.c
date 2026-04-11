// ***************************************************************************
// TITLE
//      RPLIDAR S1 Lidar Module
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include "lidars.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "driver/uart.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include "stdcommand.h"
#include <mbdebug.h>

#include <generated_files/gen_lidars_rplidarS1.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "LIDARS";

// RPLIDAR protocol constants
#define RPLIDAR_SYNC_BYTE       0xA5
#define RPLIDAR_SYNC_BYTE2      0x5A
#define RPLIDAR_CMD_STOP        0x25
#define RPLIDAR_CMD_SCAN        0x20
#define RPLIDAR_CMD_RESET       0x40
#define RPLIDAR_RESP_DESC_LEN   7
#define RPLIDAR_SCAN_PKT_LEN    5

#define MOTOR_PWM_FREQ          25000
#define MOTOR_PWM_DUTY          153     // ~60% of 255
#define RPLIDAR_MAX_RETRIES     5

// Command IDs
enum {
    LIDAR_CMD_ENABLE = 0,
};

// ---------------------------------------------------------------------------
// -------------------------------- HELPERS ----------------------------------
// ---------------------------------------------------------------------------

static void rplidar_send_cmd(int uart_num, uint8_t cmd) {
    uint8_t buf[2] = { RPLIDAR_SYNC_BYTE, cmd };
    uart_write_bytes(uart_num, (const char*)buf, 2);
}

static int rplidar_read_descriptor(int uart_num) {
    uint8_t desc[RPLIDAR_RESP_DESC_LEN];
    int len = uart_read_bytes(uart_num, desc, RPLIDAR_RESP_DESC_LEN, pdMS_TO_TICKS(2000));
    if (len != RPLIDAR_RESP_DESC_LEN) {
        ESP_LOGE(TAG, "Descriptor read timeout (got %d bytes)", len);
        return -1;
    }
    if (desc[0] != RPLIDAR_SYNC_BYTE || desc[1] != RPLIDAR_SYNC_BYTE2) {
        ESP_LOGE(TAG, "Invalid descriptor sync: 0x%02X 0x%02X", desc[0], desc[1]);
        return -2;
    }
    return 0;
}

static int rplidar_start_scan(int uart_num, lidars_t *lidar, uint8_t slot_num) {
    for (int attempt = 1; attempt <= RPLIDAR_MAX_RETRIES; attempt++) {
        ESP_LOGD(TAG, "slot:%d scan start attempt %d/%d", slot_num, attempt, RPLIDAR_MAX_RETRIES);

        rplidar_send_cmd(uart_num, RPLIDAR_CMD_STOP);
        vTaskDelay(pdMS_TO_TICKS(100));
        uart_flush(uart_num);

        rplidar_send_cmd(uart_num, RPLIDAR_CMD_SCAN);
        if (rplidar_read_descriptor(uart_num) == 0) {
            ESP_LOGD(TAG, "slot:%d scan started on attempt %d", slot_num, attempt);
            return 0;
        }

        // Reset and retry
        ESP_LOGW(TAG, "slot:%d attempt %d failed, resetting", slot_num, attempt);
        rplidar_send_cmd(uart_num, RPLIDAR_CMD_RESET);
        vTaskDelay(pdMS_TO_TICKS(2000));
        uart_flush(uart_num);
    }

    // All attempts failed
    ESP_LOGE(TAG, "slot:%d RPLIDAR not responding after %d attempts", slot_num, RPLIDAR_MAX_RETRIES);
    stdreport_s(lidar->errorReport, "RPLIDAR not responding");
    return -1;
}

static int angle_in_range(lidars_t *lidar, uint16_t angle) {
    if (lidar->angleMinVal == 0 && lidar->angleMaxVal == 360) {
        return 1; // Full circle
    }
    if (lidar->angleMinVal <= lidar->angleMaxVal) {
        // Normal range
        return (angle >= lidar->angleMinVal && angle <= lidar->angleMaxVal);
    } else {
        // Wrap-around range (e g 350..10 crosses zero)
        return (angle >= lidar->angleMinVal || angle <= lidar->angleMaxVal);
    }
}

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

/* 
    Модуль лидара RPLIDAR S1 через UART
    Измеряет расстояние до ближайшего обьекта в заданном секторе углов
    Дальность до 40м, разрешение 360 градусов
    pin[0]=TX, pin[1]=RX, pin[2]=MOTOR_PWM
    slots: 0-5
*/
static void configure_rplidarS1(lidars_t *lidar, uint8_t slot_num)
{
    // --- Command system init ---
    stdcommand_init(&lidar->cmds, slot_num);

    /* Минимальное значение дистанции в мм
    Значения меньше этого порога игнорируются
    Числовое значение 0-40000, по умолчанию 0
    */
    lidar->distMinVal = get_option_int_val(slot_num, "distMinVal", "mm", 0, 0, 40000);
    ESP_LOGD(TAG, "distMinVal:%d slot:%d", lidar->distMinVal, slot_num);

    /* Максимальное значение дистанции в мм
    Значения больше этого порога игнорируются
    Числовое значение 0-40000, по умолчанию 40000
    */
    lidar->distMaxVal = get_option_int_val(slot_num, "distMaxVal", "mm", 40000, 0, 40000);
    ESP_LOGD(TAG, "distMaxVal:%d slot:%d", lidar->distMaxVal, slot_num);

    /* Минимальный угол рабочего сектора в градусах
    Измерения вне сектора angleMinVal-angleMaxVal игнорируются
    Числовое значение 0-360, по умолчанию 0
    */
    lidar->angleMinVal = get_option_int_val(slot_num, "angleMinVal", "deg", 0, 0, 360);
    ESP_LOGD(TAG, "angleMinVal:%d slot:%d", lidar->angleMinVal, slot_num);

    /* Максимальный угол рабочего сектора в градусах
    Измерения вне сектора angleMinVal-angleMaxVal игнорируются
    Числовое значение 0-360, по умолчанию 360
    */
    lidar->angleMaxVal = get_option_int_val(slot_num, "angleMaxVal", "deg", 360, 0, 360);
    ESP_LOGD(TAG, "angleMaxVal:%d slot:%d", lidar->angleMaxVal, slot_num);

    /* Смещение нулевого угла в градусах
    Прибавляется к измеренному углу (с переносом через 360)
    Числовое значение 0-360, по умолчанию 0
    */
    lidar->angleOffset = get_option_int_val(slot_num, "angleOffset", "deg", 0, 0, 360);
    ESP_LOGD(TAG, "angleOffset:%d slot:%d", lidar->angleOffset, slot_num);

    /* Порог дистанции для дискретного режима в мм
    При значении больше 0 модуль работает в бинарном режиме (0/1)
    Числовое значение 0-40000, по умолчанию 0
    */
    lidar->distThreshold = get_option_int_val(slot_num, "distThreshold", "mm", 0, 0, 40000);
    ESP_LOGD(TAG, "distThreshold:%d slot:%d", lidar->distThreshold, slot_num);

    /* Инвертирует выход порогового режима
    Без параметров
    */
    lidar->thresholdInverse = get_option_flag_val(slot_num, "thresholdInverse");

    /* Гистерезис порога в мм
    Предотвращает дребезг при значениях вблизи порога
    Числовое значение 0-10000, по умолчанию 0
    */
    lidar->thresholdHysteresis = get_option_int_val(slot_num, "thresholdHysteresis", "mm", 0, 0, 10000);
    ESP_LOGD(TAG, "thresholdHysteresis:%d slot:%d", lidar->thresholdHysteresis, slot_num);

    /* Коэффициент фильтра сглаживания (от 0 до 1)
    При значении 1 фильтр отключен
    Числовое значение с плавающей точкой, по умолчанию 1
    */
    lidar->filterK = get_option_float_val(slot_num, "filterK", 1.0f);
    ESP_LOGD(TAG, "filterK:%f slot:%d", lidar->filterK, slot_num);

    /* Флаг включает режим рапортирования только дистанции без угла
    Без параметров
    */
    lidar->flag_distance_only = get_option_flag_val(slot_num, "distanceReport");

    /* Мертвая зона - минимальное изменение дистанции для отправки рапорта
    Числовое значение 0-4096, по умолчанию 0
    */
    lidar->deadBand = get_option_int_val(slot_num, "deadBand", "mm", 0, 0, 4096);

    /* Задержка между отправкой рапортов (антидребезг)
    Числовое значение 0-60000 мс, по умолчанию 0
    */
    lidar->debounceGap = pdMS_TO_TICKS(get_option_int_val(slot_num, "debounceGap", "ms", 0, 0, 60000));

    /* Состояние мотора при включении устройства
    0 - мотор выключен (по умолчанию), 1 - мотор включен
    Числовое значение 0-1, по умолчанию 0
    */
    lidar->defaultState = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1);
    lidar->motorEnabled = lidar->defaultState;

    // --- Topic setup ---
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        /* Определяет топик для MQTT сообщений */
        custom_topic = get_option_string_val(slot_num, "topic", "/lidar_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s slot:%d", custom_topic, slot_num);
    } else {
        char t_str[strlen(me_config.deviceName) + 20];
        sprintf(t_str, "%s/lidar_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard topic:%s slot:%d", t_str, slot_num);
    }

    // --- Command registration ---

    /* Включает/выключает мотор лидара
    Значение 0 - выключить, 1 - включить
    */
    stdcommand_register(&lidar->cmds, LIDAR_CMD_ENABLE, "enable", PARAMT_int);

    // --- Report registration ---

    /* Рапортует расстояние до ближайшего обьекта в мм
    */
    lidar->distanceReport = stdreport_register(RPTT_int, slot_num, "mm", "distance", 0, lidar->distMaxVal);

    /* Рапортует угол ближайшего обьекта в градусах
    */
    lidar->angleReport = stdreport_register(RPTT_int, slot_num, "deg", "angle", 0, 360);

    /* Рапортует состояние порогового режима 0/1
    */
    lidar->stateReport = stdreport_register(RPTT_int, slot_num, "bool", "threshold", 0, 1);

    /* Рапортует текст ошибки при сбое подключения к лидару
    */
    lidar->errorReport = stdreport_register(RPTT_string, slot_num, "string", "error");
}

// ---------------------------------------------------------------------------
// ---------------------------------- TASK -----------------------------------
// ---------------------------------------------------------------------------

void rplidarS1_task(void* arg) {
    int slot_num = *(int*)arg;

    // --- Find free UART ---
    int uart_num = UART_NUM_1;
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            char errorString[60];
            sprintf(errorString, "slot num:%d ___ No free UART driver", slot_num);
            ESP_LOGE(TAG, "%s", errorString);
            mblog(E, errorString);
            vTaskDelay(200);
            vTaskDelete(NULL);
        }
    }

    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];
    uint8_t motor_pin = SLOTS_PIN_MAP[slot_num][2];

    // --- UART config (256000 baud for RPLIDAR S1) ---
    uart_config_t uart_config = {
        .baud_rate = 256000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(uart_num, &uart_config);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    #define LIDAR_BUF_SIZE 1024
    uart_driver_install(uart_num, LIDAR_BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGD(TAG, "slot:%d UART%d installed TX:%d RX:%d", slot_num, uart_num, tx_pin, rx_pin);

    // --- Init context ---
    lidars_t lidar = LIDARS_DEFAULT();
    configure_rplidarS1(&lidar, slot_num);

    // --- Command queue ---
    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    // --- Motor PWM setup (25kHz on pin[2]) ---
    ledc_timer_config_t motor_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = MOTOR_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&motor_timer));

    int motor_channel = get_next_ledc_channel();
    if (motor_channel < 0) {
        char errorString[60];
        sprintf(errorString, "slot num:%d ___ LEDC channels has ended", slot_num);
        ESP_LOGE(TAG, "%s", errorString);
        vTaskDelay(20);
        vTaskDelete(NULL);
    }

    ledc_channel_config_t motor_chan = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = motor_channel,
        .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = motor_pin,
        .duty = lidar.motorEnabled ? MOTOR_PWM_DUTY : 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&motor_chan));
    ESP_LOGD(TAG, "slot:%d Motor PWM on pin:%d ch:%d", slot_num, motor_pin, motor_channel);

    // --- Wait for work permit ---
    waitForWorkPermit(slot_num);

    // --- Start motor and begin scan (only if defaultState=1) ---
    if (lidar.motorEnabled) {
        ESP_LOGD(TAG, "slot:%d Waiting for motor spin-up", slot_num);
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (rplidar_start_scan(uart_num, &lidar, slot_num) != 0) {
            // Failed after all retries - disable motor, stay in loop waiting for enable cmd
            lidar.motorEnabled = 0;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_channel, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_channel);
        } else {
            ESP_LOGD(TAG, "slot:%d RPLIDAR scan started", slot_num);
        }
    } else {
        ESP_LOGD(TAG, "slot:%d Motor off (defaultState=0), waiting for enable command", slot_num);
    }

    // --- Main scan loop ---
    uint16_t bestDist = UINT16_MAX;
    uint16_t bestAngle = 0;
    uint8_t hasMeasurement = 0;

    while (1) {
        // --- Check commands (non-blocking) ---
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&lidar.cmds, &params, 0);
        if (cmd == LIDAR_CMD_ENABLE) {
            uint8_t newEnable = params.p[0].i ? 1 : 0;
            if (newEnable != lidar.motorEnabled) {
                lidar.motorEnabled = newEnable;
                if (newEnable) {
                    // Enable motor and restart scan
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_channel, MOTOR_PWM_DUTY);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_channel);
                    vTaskDelay(pdMS_TO_TICKS(2000));

                    if (rplidar_start_scan(uart_num, &lidar, slot_num) != 0) {
                        lidar.motorEnabled = 0;
                        ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_channel, 0);
                        ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_channel);
                    }
                    bestDist = UINT16_MAX;
                    hasMeasurement = 0;
                    ESP_LOGD(TAG, "slot:%d Motor enabled", slot_num);
                } else {
                    // Stop scan and motor
                    rplidar_send_cmd(uart_num, RPLIDAR_CMD_STOP);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_channel, 0);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_channel);
                    uart_flush(uart_num);
                    ESP_LOGD(TAG, "slot:%d Motor disabled", slot_num);
                }
            }
        }

        // --- If motor is off, idle ---
        if (!lidar.motorEnabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- Read one scan packet (5 bytes) ---
        uint8_t pkt[RPLIDAR_SCAN_PKT_LEN];
        int len = uart_read_bytes(uart_num, pkt, RPLIDAR_SCAN_PKT_LEN, pdMS_TO_TICKS(50));
        if (len != RPLIDAR_SCAN_PKT_LEN) {
            continue;
        }

        // --- Validate packet ---
        uint8_t S = pkt[0] & 0x01;
        uint8_t notS = (pkt[0] >> 1) & 0x01;
        uint8_t C = pkt[1] & 0x01;
        uint8_t quality = pkt[0] >> 2;

        if ((S == notS) || (C != 1)) {
            // Lost sync - flush and restart scan
            ESP_LOGW(TAG, "slot:%d Lost sync, restarting scan", slot_num);
            if (rplidar_start_scan(uart_num, &lidar, slot_num) != 0) {
                lidar.motorEnabled = 0;
                ledc_set_duty(LEDC_LOW_SPEED_MODE, motor_channel, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, motor_channel);
            }
            bestDist = UINT16_MAX;
            hasMeasurement = 0;
            continue;
        }

        // --- New scan round (start of 360-degree rotation) ---
        if (S) {
            if (hasMeasurement) {
                lidar.currentDist = bestDist;
                lidar.currentAngle = bestAngle;
                lidars_report(&lidar, slot_num);
            }
            bestDist = UINT16_MAX;
            bestAngle = 0;
            hasMeasurement = 0;
        }

        // --- Extract angle and distance ---
        uint16_t angle_q6 = ((uint16_t)(pkt[1] >> 1)) | ((uint16_t)pkt[2] << 7);
        uint16_t dist_q2 = ((uint16_t)pkt[4] << 8) | pkt[3];

        uint16_t angle_deg = angle_q6 / 64;
        uint16_t dist_mm = dist_q2 / 4;

        // Apply angle offset
        if (lidar.angleOffset > 0) {
            angle_deg = (angle_deg + lidar.angleOffset) % 360;
        }

        // Skip invalid measurements (distance=0 or quality=0)
        if (dist_mm == 0 || quality == 0) continue;

        // --- Filter by angle range ---
        if (!angle_in_range(&lidar, angle_deg)) continue;

        // --- Filter by distance range ---
        if (dist_mm < lidar.distMinVal || dist_mm > lidar.distMaxVal) continue;

        // --- Track closest object in sector ---
        if (dist_mm < bestDist) {
            bestDist = dist_mm;
            bestAngle = angle_deg;
            hasMeasurement = 1;
        }
    }
}

void start_rplidarS1_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(rplidarS1_task, "rplidarS1_task", 1024 * 5, &slot_num, 5, NULL);
    ESP_LOGD(TAG, "rplidarS1_task init ok: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_lidars_rplidarS1()
{
	return manifesto;
}
