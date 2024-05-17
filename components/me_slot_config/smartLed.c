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

#include "math.h"

#include "rgbHsv.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
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

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state){
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

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder){
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder){
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder){
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
    uint8_t flag_ledUpdate = 0;

	me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));
    
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

    uint16_t increment = 52;
    if (strstr(me_config.slot_options[slot_num], "increment") != NULL) {
		increment = get_option_int_val(slot_num, "increment");
		ESP_LOGD(TAG, "Set increment:%d for slot:%d",increment, slot_num);
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
    //HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGB_color") != NULL) {
        char strDup[strlen(me_config.slot_options[slot_num])];
        strcpy(strDup, me_config.slot_options[slot_num]);
        char* payload=NULL;
        char* cmd = strtok_r(strDup, ":", &payload);
        //ESP_LOGD(TAG, "Set cmd:%s RGB_color:%s for slot:%d", cmd,payload, slot_num);
        if(strstr(payload, ",")!= NULL) {
            payload = strtok(payload, ",");
        }
        parseRGB(&targetRGB, payload);
		//HSV = RgbToHsv(targetRGB);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", targetRGB.r, targetRGB.g, targetRGB.b, slot_num);

    uint8_t ledMode = DEFAULT;
    if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "ledMode");
        ledMode = modeToEnum(tmp);
    }
    uint8_t led_strip_pixels[num_of_led * 3];
    //ESP_LOGI(TAG, "Create RMT TX channel");
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

    //ESP_LOGI(TAG, "Install led strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = tx_chan_config.resolution_hz,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    //ESP_LOGI(TAG, "Enable RMT TX channel");
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


    uint16_t currentBright=0;
    uint16_t targetBright=min_bright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };

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
                cmd = cmd + strlen(me_state.action_topic_list[slot_num])+1;
                if(strstr(cmd, "setRGB")!=NULL){
                    parseRGB(&targetRGB, payload);
                }else if(strstr(cmd, "setMode")!=NULL){
                    ESP_LOGD(TAG, "Slot_num:%d Set ledMode:%s",slot_num, payload);
                    ledMode = modeToEnum(payload);
                }else if(strstr(cmd, "setIncrement")!=NULL){
                    increment = atoi(payload);
                    ESP_LOGD(TAG, "Set fade increment:%d", increment);
                }
            }
        }

        if(state==0){
            targetBright = min_bright; 
            flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
            setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
        }else{
            if (ledMode==DEFAULT){
                targetBright = max_bright; 
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
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
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }else if(ledMode==RAINBOW){
                targetBright = max_bright;
                HsvColor hsv=RgbToHsv(currentRGB);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d H:%d S:%d V:%d",currentBright, targetBright, hsv.h, hsv.s, hsv.v);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, currentRGB.r, currentRGB.g, currentRGB.b);
                //ESP_LOGD(TAG, "hsv before:%d %d %d", hsv.h, hsv.s, hsv.v);
                hsv.h+=increment;
                hsv.s = 255;
                hsv.v = max_bright;
                //ESP_LOGD(TAG, "hsv after:%d %d %d", hsv.h, hsv.s, hsv.v);
                targetRGB = HsvToRgb(hsv);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, targetRGB.r, targetRGB.g, targetRGB.b);
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }
        }


        if(flag_ledUpdate){
            flag_ledUpdate = false;
            esp_err_t err = rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
            if(err!=ESP_OK){
                ESP_LOGE(TAG, "RMT TX error:%d", err);
            }
        }

        //uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d state:%d, currentBright:%d", delay, state, currentBright); 
        vTaskDelay(pdMS_TO_TICKS(refreshRate_ms));
    }
    EXIT:
    vTaskDelete(NULL);
}


void start_smartLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(smartLed_task, "smartLed_task", 1024*10, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------LED RING-----------------------------

typedef struct {
	uint16_t num_led;

	uint16_t frontLen;
	uint16_t diameter;
	uint16_t *ledsCoordinate;
	uint16_t effectBufLen;
	uint8_t *effectBuf;
	double ledAngleRadian;

    uint8_t *pixelBuffer;
    
    uint8_t max_bright, min_bright;

	uint16_t tick;
	uint16_t tickLenght;

	uint8_t effect;

    uint8_t *ledBrightMass;
	uint16_t brightMassLen;

	uint8_t state;

	RgbColor RGB;
	HsvColor HSV;
} ledRing_HandleTypeDef;

int calcSin(ledRing_HandleTypeDef *ledRing, int tick, int lenght) {
	float phase = tick * M_PI / lenght;
	int delta = ledRing->max_bright - ledRing->min_bright;
	//int value = sin(3.14 - phase) * delta / 2 + delta / 2;
	int value = (float)sin(phase) * (float)delta;
	value = value + ledRing->min_bright;
	//ESP_LOGD(TAG, "tick:%d phase:%f value:%d sin:%f delta:%d", tick, phase, value, sin(phase), delta);
    if (value > ledRing->max_bright)
		value = ledRing->max_bright;
	return value;
}

void setMinBright(ledRing_HandleTypeDef *ledRing) {
	ledRing->HSV = RgbToHsv(ledRing->RGB);
	ledRing->HSV.v = ledRing->min_bright;
	RgbColor tmpRGB = HsvToRgb(ledRing->HSV);
	for (int i = 0; i < ledRing->num_led; i++) {
        //led_strip_set_pixel(ledRing->pixelBuffer, i, ledRing->RGB.r, ledRing->RGB.g, ledRing->RGB.b);
        led_strip_set_pixel(ledRing->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		//PwmLed_setPixel_gammaCorrection(ledRing->pwmLed, ledRing->RGB.r, ledRing->RGB.g, ledRing->RGB.b, i);
	}
    //ESP_LOGD(TAG, "setMinBright RGB:%d,%d,%d", ledRing->RGB.r, ledRing->RGB.g, ledRing->RGB.b);
}

void setLedEffect(ledRing_HandleTypeDef *ledRing, uint8_t effect, uint16_t lenght) {
	ESP_LOGI(TAG, "start effect %d", effect);
    
    if ((effect == FADE_UP) || (effect == FADE_DOWN)||(effect == FLUSH)) {
		ledRing->state = LED_RUN;
		ledRing->tick = 0;
		ledRing->effect = effect;
		ledRing->tickLenght = lenght;
		ledRing->HSV = RgbToHsv(ledRing->RGB);
	}

	if ((effect == SWIPE_DOWN) || (effect == SWIPE_UP) || (effect == SWIPE_LEFT) || (effect == SWIPE_RIGHT)) {
		ledRing->state = LED_RUN;
		ledRing->effect = effect;
		ledRing->tick = 0;
		ledRing->tickLenght = lenght;
		ledRing->HSV = RgbToHsv(ledRing->RGB);
		ledRing->frontLen = lenght / 2;
		ledRing->diameter = lenght - ledRing->frontLen;
		ledRing->effectBufLen = lenght + ledRing->frontLen;

		//free(ledRing->effectBuf);
		ledRing->effectBuf = (uint8_t*) malloc(ledRing->effectBufLen * sizeof(uint8_t));
		memset(ledRing->effectBuf, ledRing->min_bright, ledRing->effectBufLen);

		//printf("Front:");
		for (int i = 0; i < ledRing->frontLen; i++) {
			ledRing->effectBuf[i] = calcSin(ledRing, i, ledRing->frontLen);
			//printf("%d-", ledRing->effectBuf[i]);
		}
		//printf("\r\n");

		//calc first quarter
		uint16_t quarterNum = ledRing->num_led / 4;
		for (int t = 0; t < quarterNum; t++) {
			ledRing->ledsCoordinate[t] = ledRing->frontLen + (ledRing->diameter / 2 - ledRing->diameter / 2 * cos(ledRing->ledAngleRadian / 2 + ledRing->ledAngleRadian * t));

		}
		//mirror to second quarter
		for (int t = 0; t < quarterNum; t++) {
			ledRing->ledsCoordinate[quarterNum + t] = ledRing->frontLen + (ledRing->diameter / 2)+ (ledRing->frontLen + ledRing->diameter / 2 - ledRing->ledsCoordinate[quarterNum - t - 1]);
		}
		//printf("ledsCoordinate:");
		for (int t = 0; t < ledRing->num_led / 2; t++) {
			//printf("%d-", ledRing->ledsCoordinate[t]);
		}
		//printf("\r\n");
	}
    //ESP_LOGI(TAG, "start effect end");
}

void processLedEffect(ledRing_HandleTypeDef *ledRing) {
    

	ledRing->tick++;

    //ESP_LOGI(TAG, "procces effect tick:%d", ledRing->tick);

	if (ledRing->tick >= ledRing->tickLenght) {
		ledRing->state = LED_STOP;
		ledRing->effect = WAITING;
		setMinBright(ledRing);
		//return LED_STOP;
	}

	if (ledRing->effect == FLUSH) {
		float progres = (float) ledRing->tick / ledRing->tickLenght;
		if (progres < 0.5) {
			ledRing->HSV.v = ledRing->min_bright + 2 * progres * (ledRing->max_bright - ledRing->min_bright);
		} else {
			ledRing->HSV.v = ledRing->max_bright - 2 * (progres - 0.5) * (ledRing->max_bright - ledRing->min_bright);
		}
		RgbColor tmpRGB = HsvToRgb(ledRing->HSV);

        for (int i = 0; i < ledRing->num_led; i++) {
            led_strip_set_pixel(ledRing->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(ledRing->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(ledRing.pwmLed);
	}

	if ((ledRing->effect == FADE_UP) || (ledRing->effect == FADE_DOWN)) {

		float progres = (float) ledRing->tick / ledRing->tickLenght;
		if (ledRing->effect == FADE_UP) {
			ledRing->HSV.v = ledRing->min_bright + progres * (ledRing->max_bright - ledRing->min_bright);
		} else if (ledRing->effect == FADE_DOWN) {
			ledRing->HSV.v = ledRing->max_bright - (progres * (ledRing->max_bright - ledRing->min_bright));
		}
		RgbColor tmpRGB = HsvToRgb(ledRing->HSV);
		for (int i = 0; i < ledRing->num_led; i++) {
            led_strip_set_pixel(ledRing->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(ledRing->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(ledRing.pwmLed);
	}

	if ((ledRing->effect == SWIPE_DOWN) || (ledRing->effect == SWIPE_UP) || (ledRing->effect == SWIPE_LEFT) || (ledRing->effect == SWIPE_RIGHT)) {

		int tmp = ledRing->effectBuf[ledRing->effectBufLen - 1];
		for (int i = 0; i < ledRing->effectBufLen - 1; i++) {
			ledRing->effectBuf[ledRing->effectBufLen - i - 1] = ledRing->effectBuf[ledRing->effectBufLen - i - 2];
		}
		ledRing->effectBuf[0] = tmp;

		/*
		 printf("effectBuf:");
		 for (int i = 0; i < ledRing->effectBufLen; i++) {
		    printf("%d-", ledRing->effectBuf[i]);
		 }
		 printf("\r\n");
		 */

		for (int i = 0; i < ledRing->num_led / 2; i++) {
			ledRing->ledBrightMass[i] = ledRing->effectBuf[ledRing->ledsCoordinate[i]];
			ledRing->ledBrightMass[ledRing->num_led - 1 - i] = ledRing->effectBuf[ledRing->ledsCoordinate[i]];
		}

		int rotateNum= 0;
		if (ledRing->effect == SWIPE_DOWN) {
			rotateNum = 0;
		} else if (ledRing->effect == SWIPE_LEFT) {
			rotateNum = ledRing->num_led / 4;
		} else if (ledRing->effect == SWIPE_UP) {
			rotateNum = ledRing->num_led / 2;
		} else if (ledRing->effect == SWIPE_RIGHT) {
			rotateNum = ledRing->num_led * 3 / 4;
		}

		for (int y = 0; y < rotateNum; y++) {
			uint8_t tmp = ledRing->ledBrightMass[ledRing->num_led - 1];
			for (int i = 1; i < ledRing->num_led + 1; i++) {
				ledRing->ledBrightMass[ledRing->num_led - i] = ledRing->ledBrightMass[ledRing->num_led - 1 - i];
			}
			ledRing->ledBrightMass[0] = tmp;
		}

		//printf("Tick:%d LedBrigtMass-", ledRing->tick);
		for (int i = 0; i < ledRing->num_led; i++) {
			ledRing->HSV.v = ledRing->ledBrightMass[i];
			RgbColor tmpRGB = HsvToRgb(ledRing->HSV);
			//printf("%d-", ledRing->ledBrightMass[i]);
			//todo
            led_strip_set_pixel(ledRing->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            //printf("led:%d r:%d g:%d b:%d", i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            // PwmLed_setPixel_gammaCorrection(ledRing->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		//printf("\r\n");

		///PwmLed_light(ledRing.pwmLed);

	}

	//return ledRing.state;
}


void ledRing_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    
    uint16_t num_of_led=16;
    if (strstr(me_config.slot_options[slot_num], "num_of_led") != NULL) {
		num_of_led = get_option_int_val(slot_num, "num_of_led");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
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

    uint16_t refreshRate_ms = 15;
    if (strstr(me_config.slot_options[slot_num], "refreshRate_ms") != NULL) {
		refreshRate_ms = get_option_int_val(slot_num, "refreshRate_ms");
		ESP_LOGD(TAG, "Set refreshRate_ms:%d for slot:%d",refreshRate_ms, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=250
    };
    //HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGB_color") != NULL) {
        char strDup[strlen(me_config.slot_options[slot_num])];
        strcpy(strDup, me_config.slot_options[slot_num]);
        char* payload=NULL;
        char* cmd = strtok_r(strDup, ":", &payload);
        //ESP_LOGD(TAG, "Set cmd:%s RGB_color:%s for slot:%d", cmd,payload, slot_num);
        if(strstr(payload, ",")!= NULL) {
            payload = strtok(payload, ",");
        }
        parseRGB(&targetRGB, payload);
		//HSV = RgbToHsv(targetRGB);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", targetRGB.r, targetRGB.g, targetRGB.b, slot_num);

    uint8_t led_strip_pixels[num_of_led * 3];
    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // no transfer loop
    };
    rmt_channel_handle_t led_chan = NULL;

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
  
    if (strstr(me_config.slot_options[slot_num], "ledRing_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledRing_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/ledRing_0")+3];
		sprintf(t_str, "%s/ledRing_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    ledRing_HandleTypeDef ledRing;
    ledRing.num_led = num_of_led;
    ledRing.pixelBuffer = led_strip_pixels;
    ledRing.max_bright = max_bright;
    ledRing.min_bright = min_bright;
    ledRing.RGB.r = targetRGB.r;
    ledRing.RGB.g = targetRGB.g;
    ledRing.RGB.b = targetRGB.b;
    ledRing.HSV= RgbToHsv((RgbColor)targetRGB);

    ledRing.ledBrightMass = (uint8_t*) malloc(ledRing.num_led * sizeof(uint8_t));
	ledRing.ledAngleRadian = 2 * M_PI / ledRing.num_led;
	ledRing.ledsCoordinate = (uint16_t*) malloc(ledRing.num_led / 2 * sizeof(uint16_t));
	ledRing.state = LED_STOP;

    setMinBright(&ledRing);

    ESP_LOGD(TAG, "led ring task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));


    #define LED_EFFECT_LENGTH 30

    while (1) {
        startTick = pdTICKS_TO_MS(xTaskGetTickCount());

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]+1);

            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            
            if(strstr(cmd, "swipe")!=NULL){
                if(strstr(payload, "up")!=NULL){
                    setLedEffect(&ledRing, SWIPE_UP, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "down")!=NULL){
                    setLedEffect(&ledRing, SWIPE_DOWN, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "left")!=NULL){
                    setLedEffect(&ledRing, SWIPE_LEFT, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "right")!=NULL){
                    setLedEffect(&ledRing, SWIPE_RIGHT, LED_EFFECT_LENGTH);
                }
            } 
        }
    
        if(ledRing.state == LED_RUN){
            processLedEffect(&ledRing);
        }
          
        rmt_transmit(led_chan, led_encoder, ledRing.pixelBuffer, sizeof(led_strip_pixels), &tx_config);

        // uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount())-startTick;
        // if(delay> refreshRate_ms){
        //     ESP_LOGD(TAG, "gopa delay :%d refreshRate_ms:%d startTick:%ld tick:%ld", delay, refreshRate_ms, startTick, pdTICKS_TO_MS(xTaskGetTickCount())); 
        //     delay = refreshRate_ms;
        // }
        //
        vTaskDelay(pdMS_TO_TICKS(refreshRate_ms));
    }
    EXIT:
    vTaskDelete(NULL);
}

void start_ledRing_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(ledRing_task, "ledRing_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"ledRing_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}