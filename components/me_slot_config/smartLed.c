#include "smartLed.h"
//#include "led_strip.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"

#include "reporter.h"
#include "stateConfig.h"
#include "executor.h"

#include "esp_log.h"
#include "me_slot_config.h"

#include "rgbHsv.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LED_STRIP_RMT_DEFAULT_MEM_BLOCK_SYMBOLS 48

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SMART_LED";

extern const uint8_t gamma_8[256];

void led_strip_set_pixel(uint8_t *pixel_array, int pos, int r, int g, int b){
    pixel_array[pos * 3 + 0]= (uint8_t)g;
    pixel_array[pos * 3 + 1]= (uint8_t)r;
    pixel_array[pos * 3 + 2]= (uint8_t)b;
}

void setAllLed_color(uint8_t *pixel_array, RgbColor color, uint16_t bright, uint16_t num_of_led){
    float bbright = (float)bright/255;
    uint8_t R = color.r*bbright;
    uint8_t G = color.g*bbright;
    uint8_t B = color.b*bbright;
    for(int i=0; i<num_of_led; i++){
        led_strip_set_pixel(pixel_array, i, R,G,B);
    }
}

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    switch (led_encoder->state) {
    case 0: // send RGB data
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    // fall-through
    case 1: // send reset code
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET; // back to the initial encoding session
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    rmt_led_strip_encoder_t *led_encoder = NULL;
    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    led_encoder = calloc(1, sizeof(rmt_led_strip_encoder_t));
    ESP_GOTO_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for led strip encoder");
    led_encoder->base.encode = rmt_encode_led_strip;
    led_encoder->base.del = rmt_del_led_strip_encoder;
    led_encoder->base.reset = rmt_led_strip_encoder_reset;
    // different led strip might have its own timing requirements, following parameter is for WS2812
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * config->resolution / 1000000, // T0H=0.3us
            .level1 = 0,
            .duration1 = 0.9 * config->resolution / 1000000, // T0L=0.9us
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * config->resolution / 1000000, // T1H=0.9us
            .level1 = 0,
            .duration1 = 0.3 * config->resolution / 1000000, // T1L=0.3us
        },
        .flags.msb_first = 1 // WS2812 transfer bit order: G7...G0R7...R0B7...B0
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder), err, TAG, "create bytes encoder failed");
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder), err, TAG, "create copy encoder failed");

    uint32_t reset_ticks = config->resolution / 1000000 * 50 / 2; // reset code duration defaults to 50us
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

void smartLed_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    
    uint16_t num_of_led=24;
    if (strstr(me_config.slot_options[slot_num], "num_of_led") != NULL) {
		num_of_led = get_option_int_val(slot_num, "num_of_led");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
	}

    uint8_t inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "led_inverse")!=NULL){
		inverse=1;
	}

    uint8_t state=0;
    if (strstr(me_config.slot_options[slot_num], "default_state") != NULL) {
		state = get_option_int_val(slot_num, "default_state");
		ESP_LOGD(TAG, "Set def_state:%d for slot:%d",state, slot_num);
	}

    uint16_t fade_increment = 255;
    if (strstr(me_config.slot_options[slot_num], "fade_increment") != NULL) {
		fade_increment = get_option_int_val(slot_num, "fade_increment");
		ESP_LOGD(TAG, "Set fade_increment:%d for slot:%d",fade_increment, slot_num);
	}

    uint16_t max_bright = 255;
    if (strstr(me_config.slot_options[slot_num], "max_bright") != NULL) {
		max_bright = get_option_int_val(slot_num, "max_bright");
        if(max_bright>255)max_bright=255;
		ESP_LOGD(TAG, "Set max_bright:%d for slot:%d",max_bright, slot_num);
	}

    uint16_t min_bright = 0;
    if (strstr(me_config.slot_options[slot_num], "min_bright") != NULL) {
		min_bright = get_option_int_val(slot_num, "min_bright");
        if(min_bright>255)min_bright=255;
		ESP_LOGD(TAG, "Set min_bright:%d for slot:%d",min_bright, slot_num);
	}

    uint16_t refreshRate_ms = 30;
    if (strstr(me_config.slot_options[slot_num], "refreshRate_ms") != NULL) {
		refreshRate_ms = get_option_int_val(slot_num, "refreshRate_ms");
		ESP_LOGD(TAG, "Set refreshRate_ms:%d for slot:%d",refreshRate_ms, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=250
    };
    HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGB_color") != NULL) {
        char strDup[strlen(me_config.slot_options[slot_num])];
        strcpy(strDup, me_config.slot_options[slot_num]);
        char* tmp=NULL;
    	tmp = strstr(strDup, "RGB_color");
        char *rest;
        targetRGB.r = atoi(strtok_r(strDup," ",&rest));
        targetRGB.g = atoi(strtok_r(rest," ",&rest));
        targetRGB.b = atoi(strtok_r(rest," ",&rest));
		HSV = RgbToHsv(targetRGB);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", targetRGB.r, targetRGB.g, targetRGB.b, slot_num);

    uint8_t ledMode = DEFAULT;
    if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "ledMode");
        ledMode = modeToEnum(tmp);
    }
    uint8_t led_strip_pixels[num_of_led * 3];
    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // no transfer loop
    };
    rmt_channel_handle_t led_chan = NULL;
        // led_chan->channel_id = slot_num;
        // led_chan.gpio_num = pin_num;
        // led_chan.group.group_id = 0;

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
        .gpio_num = pin_num,
        .mem_block_symbols = 48,//64, // increase the block size can make the LED less flickering
        .resolution_hz = 10000000 , // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
        .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
    };

    if(rmt_new_tx_channel(&tx_chan_config, &led_chan)!= ESP_OK){
        me_state.action_topic_list[slot_num] = "none";
        ESP_LOGE(TAG, "RMT TX channel fail for slot:%d", slot_num);
        goto EXIT;
    }

    ESP_LOGI(TAG, "Install led strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = tx_chan_config.resolution_hz,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));
  
    if (strstr(me_config.slot_options[slot_num], "smartLed_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "smartLed_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/smartLed_0")+3];
		sprintf(t_str, "%s/smartLed_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 


    ESP_LOGD(TAG, "Smart led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));


    // for (int i = 0; i < 24; i++) {
    //      led_strip_set_pixel(led_strip_pixels, i, 250, 250, 250);
    // }
    // rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
    // //rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
    // //led_strip_refresh(led_strip);

    // vTaskDelay(pdMS_TO_TICKS(1000));
    // for (int i = 0; i < 24; i++) {
    //      led_strip_set_pixel(led_strip_pixels, i, 0, 0, 0);
    // }
    // rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
    // ESP_ERROR_CHECK(led_strip_clear(led_strip));



    uint16_t currentBright=0;
    uint16_t targetBright=min_bright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };

    
    uint8_t aniSwitch=0;

    while (1) {
        startTick = xTaskGetTickCount();

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            if(strlen(cmd)==strlen(me_state.action_topic_list[slot_num])){
                state = atoi(payload);
                ESP_LOGD(TAG, "Change state to:%d", state);
            }else{
                cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
                if(strstr(cmd, "setRGB")!=NULL){
                    parseRGB(&targetRGB, payload);
                }else if(strstr(cmd, "setMode")!=NULL){
                    ledMode = modeToEnum(payload);
                }else if(strstr(cmd, "setFadeIncrement")!=NULL){
                    fade_increment = atoi(payload);
                    ESP_LOGD(TAG, "Set fade increment:%d", fade_increment);
                }
            }
        }

        if(state==0){
            targetBright = min_bright; 
            checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
            setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
        }else{
            if (ledMode==DEFAULT){
                targetBright = max_bright; 
                checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }else if(ledMode==FLASH){
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d", currentBright, targetBright); 
                if(currentBright==min_bright){
                    targetBright=max_bright;
                    //ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
                }else if(currentBright==max_bright){
                    targetBright=min_bright;
                    //ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
                }
                checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }else if(ledMode==GLITCH){
                
            }
        }

        
        rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);

        uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d state:%d, currentBright:%d", delay, state, currentBright); 
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    EXIT:
    vTaskDelete(NULL);
}


void start_smartLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreate(smartLed_task, "smartLed_task", 1024*4, &slot_num,12, NULL);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}