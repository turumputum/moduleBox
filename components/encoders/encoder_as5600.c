// ***************************************************************************
// TITLE
//      Absolute Magnetic Encoder Module (AS5600 I2C)
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_system.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include "stdcommand.h"
#include <mbdebug.h>

#include <generated_files/gen_encoder_as5600.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ENCODER_AS5600";

#define INCREMENTAL 0
#define ABSOLUTE 1

typedef struct __tag_AS5600CONFIG
{
    uint8_t                 encoderMode;
    uint8_t                 floatOutput;
    uint8_t                 dirInverse;
    int                     active_state;
    uint8_t                 zeroShift;
    int32_t                 minVal;
    int32_t                 maxVal;
    uint16_t                refreshPeriod;
    float                   filterK;
    uint16_t                deadZone;
    uint16_t                numOfPos;
    int                     pole;
    uint16_t                divider;

    int                     report;

    STDCOMMANDS             cmds;
} AS5600CONFIG, * PAS5600CONFIG;

/*
    Абсолютный магнитный энкодер AS5600 по I2C - 12 бит, 4096 позиций на окружности
    slots: 0-5
*/
void configure_encoderAS5600(PAS5600CONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Флаг - стартовать в выключенном состоянии до прихода action/enable 1 */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[encoder_%d] initial active_state:%d", slot_num, c->active_state);

    /* Флаг - абсолютный режим - По умолчанию инкрементальный */
    c->encoderMode = get_option_flag_val(slot_num, "absolute");
    ESP_LOGD(TAG, "[encoder_%d] mode:%s", slot_num, c->encoderMode ? "absolute" : "incremental");

    /* Флаг - вывод в формате float от 0 до 1 - По умолчанию целое */
    c->floatOutput = get_option_flag_val(slot_num, "floatOutput");
    ESP_LOGD(TAG, "[encoder_%d] floatOutput:%d", slot_num, c->floatOutput);

    /* Флаг - инверсия направления счета */
    c->dirInverse = get_option_flag_val(slot_num, "dirInverse");
    ESP_LOGD(TAG, "[encoder_%d] dirInverse:%d", slot_num, c->dirInverse);

    /* Зона нечувствительности в единицах сырого угла - По умолчанию 0 */
    c->deadZone = get_option_int_val(slot_num, "deadZone", "", 0, 0, INT16_MAX);
    ESP_LOGD(TAG, "[encoder_%d] deadZone:%d", slot_num, c->deadZone);

    /* Смещение нуля в единицах сырого угла - По умолчанию 0 */
    c->zeroShift = get_option_int_val(slot_num, "zeroShift", "", 0, INT16_MIN, INT16_MAX);
    ESP_LOGD(TAG, "[encoder_%d] zeroShift:%d", slot_num, c->zeroShift);

    /* Коэффициент фильтра скользящее среднее от 0 до 1 - 1 без фильтрации - По умолчанию 1 */
    c->filterK = get_option_float_val(slot_num, "filterK", 1.0);
    ESP_LOGD(TAG, "[encoder_%d] filterK:%f", slot_num, c->filterK);

    c->minVal = 0;
    c->maxVal = 4095;
    c->pole = c->maxVal - c->minVal;

    /* Количество сегментов на окружности - По умолчанию 24 */
    c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", 24, 2, 4095);
    ESP_LOGD(TAG, "[encoder_%d] numOfPos:%d", slot_num, c->numOfPos);

    /* Период опроса значений в Гц - По умолчанию 20, максимум 100 */
    c->refreshPeriod = 1000 / (get_option_int_val(slot_num, "refreshRate", "fps", 20, 1, 100));
    ESP_LOGD(TAG, "[encoder_%d] refreshPeriod:%d", slot_num, c->refreshPeriod);

    {
        char t_str[strlen(me_config.deviceName) + strlen("/encoder_0") + 3];
        sprintf(t_str, "%s/encoder_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === REPORTS === */

    /* Текущее значение - абс позиция, инкр приращение или float по настройкам */
    c->report = stdreport_register(RPTT_string, slot_num, "", "event/val");

    /* Состояние модуля - активен 1 или спит 0 */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);
}

// AS5600 I2C registers
#define AS5600_I2C_ADDRESS 0x36
#define AS5600_RAW_ANGLE_REG 0x0C
#define AS5600_ANGLE_REG 0x0E
#define AS5600_STATUS_REG 0x0B
#define AS5600_AGC_REG 0x1A
#define AS5600_MAGNITUDE_REG 0x1B

#define I2C_MASTER_TIMEOUT_MS 100

esp_err_t checkMagnetStatus(int i2c_num, int slot_num)
{
    // Test AS5600 connection by reading status register
    esp_err_t ret = ESP_OK;
    uint8_t status;
    ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS,
                                        &(uint8_t){AS5600_STATUS_REG}, 1,
                                        &status, 1,
                                        I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK) {
        char tmpStr[100];
        sprintf(tmpStr, "AS5600 not found on slot:%d task terminated", slot_num);
        ESP_LOGW(TAG, "%s", tmpStr);
        mblog(W, tmpStr);
        vTaskDelete(NULL);
    }

    // Check magnetic field status
    bool magnet_detected = (status & 0x20) != 0;  // Bit 5: MD
    bool magnet_too_low = (status & 0x10) != 0;   // Bit 4: ML
    bool magnet_too_high = (status & 0x08) != 0;  // Bit 3: MH

    if (!magnet_detected) {
        char tmpStr[100];
        if (magnet_too_low) {
            sprintf(tmpStr, "AS5600 slot:%d - magnet field too weak! task terminated", slot_num);
        } else if (magnet_too_high) {
            printf(tmpStr, "AS5600 slot:%d - magnet field too strong! task terminated", slot_num);
        } else {
            printf(tmpStr, "AS5600 slot:%d - magnet not found! task terminated", slot_num);
        }
        ESP_LOGE(TAG, "%s", tmpStr);
        mblog(0, tmpStr);
        vTaskDelete(NULL);
    }
    return ret;
}

void encoderAS5600_task(void *arg)
{
    AS5600CONFIG c = {0};
    int slot_num = (int)(intptr_t)arg;

    configure_encoderAS5600(&c, slot_num);

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    char str[255];

    // I2C initialization
    uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];
    gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][2];

    esp_rom_gpio_pad_select_gpio(led_pin);
    gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(led_pin, 1);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    char tmpStr[100];

    int i2c_num = me_state.free_i2c_num;
    me_state.free_i2c_num++;
    if (i2c_num == I2C_NUM_MAX) {
        sprintf(tmpStr, "No free I2C driver for slot:%d task terminated", slot_num);
        ESP_LOGW(TAG, "%s", tmpStr);
        mblog(W, tmpStr);
        vTaskDelete(NULL);
    }

    i2c_param_config(i2c_num, &conf);
    esp_err_t ret = i2c_driver_install(i2c_num, conf.mode, 0, 0, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C_%d initialized for AS5600 slot:%d", i2c_num, slot_num);
    } else {
        sprintf(tmpStr, "Failed to initialize I2C_%d for slot:%d", i2c_num, slot_num);
        ESP_LOGE(TAG, "%s", tmpStr);
        mblog(0, tmpStr);
        vTaskDelete(NULL);
    }

    float pos_length = (float)c.pole / c.numOfPos;
    uint16_t raw_angle;
    int current_pos = 0, prev_pos = -1;

    checkMagnetStatus(i2c_num, slot_num);

    // Read magnetic field magnitude
    uint8_t magnitude_data[2];
    ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS,
                                       &(uint8_t){AS5600_MAGNITUDE_REG}, 1,
                                       magnitude_data, 2,
                                       I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret == ESP_OK) {
        uint16_t magnitude = ((uint16_t)magnitude_data[0] << 8) | magnitude_data[1];
        magnitude &= 0x0FFF; // Mask to 12 bits
        ESP_LOGI(TAG, "AS5600 slot:%d - сила магнитного поля: %d", slot_num, magnitude);
    }

    float filtredVal = 0;
    float prew_filtredVal = -1;
    STDCOMMAND_PARAMS params = {0};
    TickType_t lastWakeTime = xTaskGetTickCount();
    int checkTick = 0;

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c.active_state);

    while (1) {
        // --- Обработка команд (включая action/enable) ---
        int cmd = stdcommand_receive(&c.cmds, &params, 0);
        if (cmd == STDCMD_ENABLE) {
            if (params.count > 0) {
                c.active_state = params.p[0].i ? 1 : 0;
                ESP_LOGD(TAG, "enable:%d slot:%d", c.active_state, slot_num);
                stdreport_enable(slot_num, c.active_state);
            }
        }

        if (checkTick > 1000) {
            checkMagnetStatus(i2c_num, slot_num);
            checkTick = 0;
        } else {
            checkTick++;
        }

        // Read raw angle from AS5600
        uint8_t angle_reg = AS5600_RAW_ANGLE_REG;
        uint8_t angle_data[2];

        ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS,
                                           &angle_reg, 1,
                                           angle_data, 2,
                                           I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

        // Combine bytes and convert to 12-bit value (0-4095)
        raw_angle = ((uint16_t)angle_data[0] << 8) | angle_data[1];
        raw_angle &= 0x0FFF; // Mask to 12 bits

        // Apply offset
        int16_t adjusted_angle = raw_angle + c.zeroShift;
        if (adjusted_angle < 0) {
            adjusted_angle += 4096;
        }
        if (adjusted_angle >= 4096) {
            adjusted_angle -= 4096;
        }

        // Apply filtering
        if ((c.filterK < 1) && (abs(adjusted_angle - prew_filtredVal) < 2048)) {
            filtredVal = filtredVal * (1 - c.filterK) + adjusted_angle * c.filterK;
        } else {
            filtredVal = adjusted_angle;
        }

        // Check dead zone
        if (abs(filtredVal - prew_filtredVal) > c.deadZone) {
            prew_filtredVal = filtredVal;
            gpio_set_level(led_pin, (int)filtredVal % 2);

            // Calculate position based on filtered value
            current_pos = filtredVal / pos_length;
            if (current_pos >= c.numOfPos) {
                current_pos -= c.numOfPos;
            }

            if (c.dirInverse) {
                current_pos = c.numOfPos - current_pos;
            }
        }

        // Report changes - единый event/val, представление по mode/floatOutput
        if (current_pos != prev_pos) {
            if (c.active_state) {
                if (c.encoderMode == ABSOLUTE) {
                    if (c.floatOutput) {
                        sprintf(str, "%f", (float)current_pos / (c.numOfPos - 1));
                    } else {
                        sprintf(str, "%d", current_pos);
                    }
                } else { // INCREMENTAL
                    int delta = abs(current_pos - prev_pos);
                    if (delta < (c.numOfPos / 2)) {
                        if (current_pos < prev_pos) {
                            sprintf(str, "-%d", delta);
                        } else {
                            sprintf(str, "+%d", delta);
                        }
                    } else {
                        delta = c.numOfPos - delta;
                        if (current_pos < prev_pos) {
                            sprintf(str, "+%d", delta);
                        } else {
                            sprintf(str, "-%d", delta);
                        }
                    }
                }
                stdreport_s(c.report, str);
            }
            prev_pos = current_pos;
        }

        if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(c.refreshPeriod)) == pdFALSE) {
            ESP_LOGE(TAG, "AS5600 delay missed! Adjusting wake time slot:%d", slot_num);
            lastWakeTime = xTaskGetTickCount();
        }
    }
}

void start_encoderAS5600_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(encoderAS5600_task, "task_encAS5600", 1024 * 4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 10, NULL);
    ESP_LOGD(TAG, "AS5600 encoder init ok: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_encoder_as5600()
{
    return manifesto;
}
