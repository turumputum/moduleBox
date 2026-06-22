#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdcommand.h"
#include "stdreport.h"

#include <generated_files/gen_dwin.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DWIN_UART";

#define DWIN_BUF_SIZE 256

typedef enum {
    DWIN_CMD_setPage = 0,
} dwin_cmd_t;

typedef struct {
    STDCOMMANDS cmds;
    int active_state;
} dwin_ctx_t;

// Читает один кадр DWIN (0x5A 0xA5 LEN DATA) в message_buffer, возвращает длину DATA
static int read_dwin_message(uart_port_t uart_num, uint8_t *message_buffer) {
    uint8_t first_byte;
    int len_read = uart_read_bytes(uart_num, &first_byte, 1, 5 / portTICK_RATE_MS);
    if (len_read <= 0 || first_byte != 0x5A) {
        return ESP_FAIL;
    }

    uint8_t second_byte;
    len_read = uart_read_bytes(uart_num, &second_byte, 1, 0);
    if (len_read <= 0 || second_byte != 0xA5) {
        return ESP_FAIL;
    }

    uint8_t length_byte = 0;
    len_read = uart_read_bytes(uart_num, &length_byte, 1, 0);
    if (len_read <= 0) {
        return ESP_FAIL;
    }
    if (length_byte > DWIN_BUF_SIZE) {
        length_byte = DWIN_BUF_SIZE;
    }

    len_read = uart_read_bytes(uart_num, message_buffer, length_byte, 0);
    if (len_read != length_byte) {
        return ESP_FAIL;
    }

    return length_byte;
}

/* Дисплей DWIN по UART - читает изменения регистров VP и шлёт их событиями event/VP_xxxx, принимает смену страницы
slots: 0-5
*/
void configure_dwin(dwin_ctx_t *ctx, int slot_num) {
    stdcommand_init(&ctx->cmds, slot_num);

    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    ctx->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "Initial active_state:%d for slot:%d", ctx->active_state, slot_num);

    {
        char t_str[strlen(me_config.deviceName) + strlen("/dwin_0") + 3];
        sprintf(t_str, "%s/dwin_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === COMMANDS === */

    /* Перейти на страницу экрана по номеру
    */
    stdcommand_register(&ctx->cmds, DWIN_CMD_setPage, "action/setPage", PARAMT_int);

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ctx->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");

    // Изменения VP публикуются динамически как event/VP_<адрес> в задаче -
    // адрес произвольный (задаётся проектом дисплея), статически не регистрируется
}

void dwinUart_task(void* arg) {
    int slot_num = (int)(intptr_t)arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    dwin_ctx_t ctx = {0};
    configure_dwin(&ctx, slot_num);

    int uart_num = UART_NUM_1;
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
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
    uart_driver_install(uart_num, DWIN_BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGD(TAG, "UART initialized for slot:%d uart_num:%d rx_pin:%d tx_pin:%d", slot_num, uart_num, rx_pin, tx_pin);

    uint8_t rawByte[DWIN_BUF_SIZE];
    char str[64];
    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, ctx.active_state);

    while (1) {
        int len_read = read_dwin_message(uart_num, rawByte);
        if (len_read > 0 && ctx.active_state) {
            // 0x83 - ответ дисплея с прочитанным значением регистра VP
            if (rawByte[0] == 0x83) {
                uint16_t value = (rawByte[4] << 8) | rawByte[5];
                sprintf(str, "/event/VP_%.2x%.2x:%d", rawByte[1], rawByte[2], value);
                report(str, slot_num);
            }
        }

        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        switch (cmd) {
            case STDCMD_ENABLE:
                if (params.count > 0) {
                    ctx.active_state = params.p[0].i ? 1 : 0;
                    ESP_LOGD(TAG, "enable:%d slot:%d", ctx.active_state, slot_num);
                    stdreport_enable(slot_num, ctx.active_state);
                }
                break;

            case DWIN_CMD_setPage:
                if (ctx.active_state && params.count > 0) {
                    uint8_t page = (uint8_t)params.p[0].i;
                    uint8_t buf[10] = {0x5a, 0xa5, 0x07, 0x82, 0x00, 0x84, 0x5a, 0x01, 0x00, page};
                    uart_write_bytes(uart_num, (const char *)buf, 10);
                    ESP_LOGD(TAG, "setPage:%d slot:%d", page, slot_num);
                }
                break;

            default:
                break;
        }
    }
}

void start_dwinUart_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(dwinUart_task, "dwinUart_task", 1024 * 4, (void*)(intptr_t)slot_num, 5, NULL);
    ESP_LOGD(TAG, "dwinUart_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_dwin() {
    return manifesto;
}
