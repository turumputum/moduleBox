#include <stdio.h>
#include "rtp_play.h"

#include "sdkconfig.h"
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>
#include "esp_netif.h"
#include "reporter.h"
#include "stateConfig.h"
#include "board.h"
#include "esp_peripherals.h"
#include "esp_system.h"
#include "esp_log.h"
#include "i2s_stream.h"
#include "filter_resample.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "raw_stream.h"
#include "rtp_client_stream.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "AUDIO_LAN";

void audioLAN_task(void *arg){
	int slot_num = *(int *)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (me_state.LAN_init_res != ESP_OK){
		vTaskDelay(pdMS_TO_TICKS(100));
	}
    ESP_LOGI(TAG, "Lan inited lets configure the slot");

    gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][3];
    esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, 1);

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t rtp_stream_reader, i2s_stream_writer;

    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_NULL_CHECK(TAG, pipeline, return);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.task_core=1;
    //i2s_cfg.buffer_len = 36000;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);
    AUDIO_NULL_CHECK(TAG, i2s_stream_writer, return);

    ESP_LOGI(TAG, "[2.2] Create rtp client stream to read data");
    rtp_stream_cfg_t rtp_cfg = RTP_STREAM_CFG_DEFAULT();
    rtp_cfg.type = AUDIO_STREAM_READER;
    rtp_cfg.port = 7777;
    rtp_cfg.host = "239.0.7.1";
    rtp_cfg.task_core=1;
    rtp_stream_reader = rtp_stream_init(&rtp_cfg);
    AUDIO_NULL_CHECK(TAG, rtp_stream_reader, return);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, rtp_stream_reader, "rtp");

    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link it together raw-->opus-->i2s");
    audio_pipeline_link(pipeline, (const char *[]) {"rtp", "i2s"}, 2);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    // ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    // audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    i2s_stream_set_clk(i2s_stream_writer, 48000, 16, 2);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
         //   break;
        }
    }

}

void start_audioLAN_task(int slot_num){

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
    char tmpString[60];
	sprintf(tmpString, "task_audioLAN_%d", slot_num);
	//xTaskCreatePinnedToCore(button_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-5, NULL, 0);
	xTaskCreate(audioLAN_task, tmpString, 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "audioLAN_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

