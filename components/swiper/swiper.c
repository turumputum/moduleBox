#include <stdio.h>
#include "swiper.h"

#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdcommand.h"
#include "stdreport.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "driver/i2c.h"
#include "apds9960.h"

#include "rgbHsv.h"
#include "driver/rmt_tx.h"
#include "math.h"

#include <generated_files/gen_swiper.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SWIPER";

typedef struct {
    int up_down_disable;
    int active_state;
    int swipeReport;
    STDCOMMANDS cmds;
} swiper_ctx_t;

/*
    Жестовый датчик APDS9960 по I2C - свайпы up down left right
    slots: 0-5
*/
void configure_swiper(swiper_ctx_t *ctx, int slot_num) {
    stdcommand_init(&ctx->cmds, slot_num);

    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    ctx->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "Initial active_state:%d for slot:%d", ctx->active_state, slot_num);

    /* Отключить вертикальные жесты up-down
    */
    ctx->up_down_disable = get_option_flag_val(slot_num, "upDownDisable");
    ESP_LOGD(TAG, "Set upDownDisable:%d for slot:%d", ctx->up_down_disable, slot_num);

    {
        char t_str[strlen(me_config.deviceName)+strlen("/swiper_0")+3];
        sprintf(t_str, "%s/swiper_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* === EVENTS === */

    /* Жест свайпа - up down left right
    */
    ctx->swipeReport = stdreport_register(RPTT_string, slot_num, "", "event/swipe");

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ctx->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

void swiper_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;

    uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    swiper_ctx_t ctx = {0};
    configure_swiper(&ctx, slot_num);

    // todo: check port
    i2c_port_t i2c_port = I2C_NUM_0;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_pin,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = scl_pin,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(i2c_port, &conf);
    i2c_driver_install(i2c_port, conf.mode, 0, 0, 0);

    sensor_start(i2c_port);

    if(!ctx.up_down_disable){
        disableVerticalAxis(i2c_port);
    }

    gesture_data_type gData;
    gData.state = WAITING;

    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, ctx.active_state);

    while(1){
        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        if (cmd == STDCMD_ENABLE && params.count > 0) {
            ctx.active_state = params.p[0].i ? 1 : 0;
            ESP_LOGD(TAG, "enable:%d slot:%d", ctx.active_state, slot_num);
            stdreport_enable(slot_num, ctx.active_state);
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);

        if (readSensor(i2c_port, &gData) != ESP_OK){
            ESP_LOGE(TAG, "Error reading sensor");
            continue;
        }
        if(gData.state == GESTURE_END){
            if(processGesture(&gData)==ESP_OK){
                const char *dir = NULL;
                if((gData.gesture == SWIPE_UP)&&(!ctx.up_down_disable)){
                    dir = "up";
                }else if((gData.gesture == SWIPE_DOWN)&&(!ctx.up_down_disable)){
                    dir = "down";
                }else if(gData.gesture == SWIPE_LEFT){
                    dir = "left";
                }else if(gData.gesture == SWIPE_RIGHT){
                    dir = "right";
                }
                if(dir != NULL && ctx.active_state){
                    stdreport_s(ctx.swipeReport, dir);
                }
                gData.state = WAITING;
                ESP_LOGD(TAG, "Gesture:%d duration:%ldms size:%d", gData.gesture, gData.duration/1000, gData.size);
            }
        }
    }
}

void start_swiper_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_swiper_%d", slot_num);
    xTaskCreatePinnedToCore(swiper_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES-12, NULL, 1);

    ESP_LOGD(TAG, "swiper task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_swiper()
{
    return manifesto;
}
