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
#include "freertos/semphr.h"

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

SemaphoreHandle_t rmt_semaphore=NULL;

#define MAX_CHANNELS 4
uint8_t rmt_chan_counter = 0;



void led_strip_set_pixel(uint8_t *pixel_array, int pos, int r, int g, int b){
    pixel_array[pos * 3 + 0]= (uint8_t)g;
    pixel_array[pos * 3 + 1]= (uint8_t)r;
    pixel_array[pos * 3 + 2]= (uint8_t)b;
}

void setAllLed_color(uint8_t *pixel_array, RgbColor color, float bright, uint16_t num_of_led){
    uint8_t R = color.r*bright;
    uint8_t G = color.g*bright;
    uint8_t B = color.b*bright;
    //ESP_LOGD(TAG, "setAllLed_color RGB:%d,%d,%d", R,G,B);
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

uint8_t rmt_createAndSend(rmt_led_heap_t *rmt_slot_heap, uint8_t *led_strip_pixels, uint16_t size, uint8_t slot_num){
    if (xSemaphoreTake(rmt_semaphore, portMAX_DELAY) == pdTRUE) {
        //ESP_LOGD(TAG, "rmt_slot_heap->->resolution_hz:%ld, rmt_slot_heap->->trans_queue_depth:%d", rmt_slot_heap->tx_chan_config.resolution_hz,rmt_slot_heap->tx_chan_config.trans_queue_depth);
        //ESP_LOGD(TAG, "sizeof(led_strip_pixels):%d", sizeof(led_strip_pixels));
        //rmt_slot_heap->led_chan->channel_id = uxSemaphoreGetCount(rmt_semaphore);
        
        if(rmt_new_tx_channel(&rmt_slot_heap->tx_chan_config, &rmt_slot_heap->led_chan)!= ESP_OK){
            ESP_LOGE(TAG, "RMT TX channel fail for slot:%d", slot_num);
            ESP_LOGD(TAG, "rmt_semaphore_count:%d", uxSemaphoreGetCount(rmt_semaphore));
        }else{
            rmt_enable(rmt_slot_heap->led_chan);
            esp_err_t err = rmt_transmit(rmt_slot_heap->led_chan, rmt_slot_heap->led_encoder, led_strip_pixels, size, &rmt_slot_heap->tx_config);
            if(err!=ESP_OK){
                ESP_LOGE(TAG, "RMT TX error:%d", err);
            }
            rmt_tx_wait_all_done(rmt_slot_heap->led_chan, portMAX_DELAY);
            rmt_disable(rmt_slot_heap->led_chan);
            rmt_del_channel(rmt_slot_heap->led_chan);
            gpio_set_direction(rmt_slot_heap->tx_chan_config.gpio_num, GPIO_MODE_OUTPUT);
            gpio_set_level(rmt_slot_heap->tx_chan_config.gpio_num, 0);
        }
        
    }
    xSemaphoreGive(rmt_semaphore);
    return 0;
}




//---------------------LED_STRIP----------------------------
void smartLed_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    uint8_t flag_ledUpdate = 0;

	me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));
    
    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }

    uint16_t num_of_led=24;
    if (strstr(me_config.slot_options[slot_num], "numOfLed") != NULL) {
		num_of_led = get_option_int_val(slot_num, "numOfLed");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
	}

    uint8_t inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "ledInverse")!=NULL){
		inverse=1;
	}

    uint8_t state=0;
    if (strstr(me_config.slot_options[slot_num], "defaultState") != NULL) {
		state = get_option_int_val(slot_num, "defaultState");
		ESP_LOGD(TAG, "Set def_state:%d for slot:%d",state, slot_num);
	}

    float increment = 1.0;
    if (strstr(me_config.slot_options[slot_num], "increment") != NULL) {
		increment = get_option_float_val(slot_num, "increment");
		ESP_LOGD(TAG, "Set increment:%f for slot:%d  configString:%s",increment, slot_num, me_config.slot_options[slot_num]);
	}

    float maxBright = 1.0;
    if (strstr(me_config.slot_options[slot_num], "maxBright") != NULL) {
		maxBright = get_option_float_val(slot_num, "maxBright");
        if(maxBright>1.0)maxBright=1.0;
		ESP_LOGD(TAG, "Set maxBright:%f for slot:%d",maxBright, slot_num);
	}

    float minBright = 0.0;
    if (strstr(me_config.slot_options[slot_num], "minBright") != NULL) {
		minBright = get_option_float_val(slot_num, "minBright");
        if(minBright<0.0)minBright=0.0;
		ESP_LOGD(TAG, "Set minBright:%f for slot:%d",minBright, slot_num);
	}

    uint16_t refreshPeriod = 40;
    if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=255
    };
    //HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGBcolor") != NULL) {
        char *tmpPtr = strstr(me_config.slot_options[slot_num], "RGBcolor");
        char strDup[strlen(tmpPtr)+1];
        strcpy(strDup, tmpPtr);
        char* payload=NULL;
        char* cmd = strtok_r(strDup, ":", &payload);
        ESP_LOGD(TAG, "Set cmd:%s RGB_color:%s for slot:%d", cmd,payload, slot_num);
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
        ESP_LOGD(TAG, "Set ledMode:%d for slot:%d",ledMode, slot_num);
    }
    uint8_t led_strip_pixels[num_of_led * 3];

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/smartLed_0")+3];
		sprintf(t_str, "%s/smartLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    ESP_LOGD(TAG, "Smart led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));

    float currentBright=0;
    float targetBright=minBright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };
    //state = inverse;

    while (1) {
        startTick = xTaskGetTickCount();

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            if(strlen(cmd)==strlen(me_state.action_topic_list[slot_num])){
                state = inverse ? !atoi(payload) : atoi(payload);
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
                    ESP_LOGD(TAG, "Set fade increment:%f", increment);
                }
            }
        }

        if(state==0){
            targetBright =fabs(inverse-minBright); 
            currentRGB.b=targetRGB.b;
            currentRGB.g=targetRGB.g;
            currentRGB.r=targetRGB.r;
            flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
            setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
        }else{
            if (ledMode==DEFAULT){
                targetBright = fabs(inverse-maxBright);  
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
                //ESP_LOGD(TAG, "DEFAULT currentBright:%f targetBright:%f increment:%f", currentBright, targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }else if(ledMode==FLASH){
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d", currentBright, targetBright); 
                if(currentBright==minBright){
                    targetBright=fabs(inverse-maxBright);
                    //ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
                }else if(currentBright==maxBright){
                    targetBright=fabs(inverse-minBright);
                    //ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
                }
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }else if(ledMode==RAINBOW){
                targetBright = maxBright;
                HsvColor hsv=RgbToHsv(targetRGB);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d H:%d S:%d V:%d",currentBright, targetBright, hsv.h, hsv.s, hsv.v);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, currentRGB.r, currentRGB.g, currentRGB.b);
                //ESP_LOGD(TAG, "hsv before:%d %d %d", hsv.h, hsv.s, hsv.v);
                hsv.h+=(uint8_t)(increment*255);
                //hsv.s = 255;
                //hsv.v = (uint8_t)(maxBright*255);
                //ESP_LOGD(TAG, "hsv after:%d %d %d", hsv.h, hsv.s, hsv.v);
                targetRGB = HsvToRgb(hsv);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, targetRGB.r, targetRGB.g, targetRGB.b);
                flag_ledUpdate = checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, num_of_led);
            }
        }


        if(flag_ledUpdate){
            flag_ledUpdate = false;
            //ESP_LOGD(TAG, "sizeof(led_strip_pixels):%d", sizeof(led_strip_pixels));
            rmt_createAndSend(&rmt_slot_heap, led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
        }

        uint16_t delay = refreshPeriod - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d state:%d, currentBright:%d", delay, state, currentBright); 
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    //EXIT:
    //vTaskDelete(NULL);
}

void start_smartLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(smartLed_task, "smartLed_task", 1024*10, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------SWIPER LED-----------------------------
typedef struct {
	uint16_t num_led;

	uint16_t frontLen;
	uint16_t diameter;
	uint16_t *ledsCoordinate;
	uint16_t effectBufLen;
	uint8_t *effectBuf;
	double ledAngleRadian;

    uint8_t *pixelBuffer;
    
    float maxBright, minBright;

	uint16_t tick;
	uint16_t tickLenght;

	uint8_t effect;

    uint8_t *ledBrightMass;
	uint16_t brightMassLen;

	uint8_t state;

	RgbColor RGB;
	HsvColor HSV;
} swiperLed_HandleTypeDef;

float calcSin(swiperLed_HandleTypeDef *swiperLed, int tick, int lenght) {
	float phase = tick * M_PI / lenght;
	float delta = swiperLed->maxBright - swiperLed->minBright;
	//int value = sin(3.14 - phase) * delta / 2 + delta / 2;
	float value = (float)sin(phase) * delta;
	value = value + swiperLed->minBright;
	//ESP_LOGD(TAG, "tick:%d phase:%f value:%d sin:%f delta:%d", tick, phase, value, sin(phase), delta);
    if (value > swiperLed->maxBright)
		value = swiperLed->maxBright;
	return value;
}

void setMinBright(swiperLed_HandleTypeDef *swiperLed) {
	swiperLed->HSV = RgbToHsv(swiperLed->RGB);
	swiperLed->HSV.v = 255*swiperLed->minBright;
	RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
	for (int i = 0; i < swiperLed->num_led; i++) {
        //led_strip_set_pixel(swiperLed->pixelBuffer, i, swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b);
        led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		//PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b, i);
	}
    //ESP_LOGD(TAG, "setMinBright RGB:%d,%d,%d", swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b);
}

void setLedEffect(swiperLed_HandleTypeDef *swiperLed, uint8_t effect, uint16_t lenght) {
	ESP_LOGI(TAG, "start effect %d", effect);
    
    if ((effect == FADE_UP) || (effect == FADE_DOWN)||(effect == FLUSH)) {
		swiperLed->state = LED_RUN;
		swiperLed->tick = 0;
		swiperLed->effect = effect;
		swiperLed->tickLenght = lenght;
		swiperLed->HSV = RgbToHsv(swiperLed->RGB);
	}

	if ((effect == SWIPE_DOWN) || (effect == SWIPE_UP) || (effect == SWIPE_LEFT) || (effect == SWIPE_RIGHT)) {
		swiperLed->state = LED_RUN;
		swiperLed->effect = effect;
		swiperLed->tick = 0;
		swiperLed->tickLenght = lenght;
		swiperLed->HSV = RgbToHsv(swiperLed->RGB);
		swiperLed->frontLen = lenght / 2;
		swiperLed->diameter = lenght - swiperLed->frontLen;
		swiperLed->effectBufLen = lenght + swiperLed->frontLen;

		//free(swiperLed->effectBuf);
		swiperLed->effectBuf = (uint8_t*) malloc(swiperLed->effectBufLen * sizeof(uint8_t));
		memset(swiperLed->effectBuf, 255*swiperLed->minBright, swiperLed->effectBufLen);

		//printf("Front:");
		for (int i = 0; i < swiperLed->frontLen; i++) {
			swiperLed->effectBuf[i] = 255*calcSin(swiperLed, i, swiperLed->frontLen);
			//printf("%d-", swiperLed->effectBuf[i]);
		}
		//printf("\r\n");

		//calc first quarter
		uint16_t quarterNum = swiperLed->num_led / 4;
		for (int t = 0; t < quarterNum; t++) {
			swiperLed->ledsCoordinate[t] = swiperLed->frontLen + (swiperLed->diameter / 2 - swiperLed->diameter / 2 * cos(swiperLed->ledAngleRadian / 2 + swiperLed->ledAngleRadian * t));
		}
		//mirror to second quarter
		for (int t = 0; t < quarterNum; t++) {
			swiperLed->ledsCoordinate[quarterNum + t] = swiperLed->frontLen + (swiperLed->diameter / 2)+ (swiperLed->frontLen + swiperLed->diameter / 2 - swiperLed->ledsCoordinate[quarterNum - t - 1]);
		}
		//printf("ledsCoordinate:");
		for (int t = 0; t < swiperLed->num_led / 2; t++) {
			//printf("%d-", swiperLed->ledsCoordinate[t]);
		}
		//printf("\r\n");
	}
    //ESP_LOGI(TAG, "start effect end");
}

void processLedEffect(swiperLed_HandleTypeDef *swiperLed) {
	swiperLed->tick++;

    //ESP_LOGI(TAG, "procces effect tick:%d", swiperLed->tick);

	if (swiperLed->tick >= swiperLed->tickLenght) {
		swiperLed->state = LED_STOP;
		swiperLed->effect = WAITING;
		setMinBright(swiperLed);
		//return LED_STOP;
	}

	if (swiperLed->effect == FLUSH) {
		float progres = (float) swiperLed->tick / swiperLed->tickLenght;
		if (progres < 0.5) {
			swiperLed->HSV.v = 255*(swiperLed->minBright + 2 * progres * (swiperLed->maxBright - swiperLed->minBright));
		} else {
			swiperLed->HSV.v = 255*(swiperLed->maxBright - 2 * (progres - 0.5) * (swiperLed->maxBright - swiperLed->minBright));
		}
		RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);

        for (int i = 0; i < swiperLed->num_led; i++) {
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(swiperLed.pwmLed);
	}

	if ((swiperLed->effect == FADE_UP) || (swiperLed->effect == FADE_DOWN)) {

		float progres = (float) swiperLed->tick / swiperLed->tickLenght;
		if (swiperLed->effect == FADE_UP) {
			swiperLed->HSV.v = 255*(swiperLed->minBright + progres * (swiperLed->maxBright - swiperLed->minBright));
		} else if (swiperLed->effect == FADE_DOWN) {
			swiperLed->HSV.v = 255*(swiperLed->maxBright - (progres * (swiperLed->maxBright - swiperLed->minBright)));
		}
		RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
		for (int i = 0; i < swiperLed->num_led; i++) {
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(swiperLed.pwmLed);
	}

	if ((swiperLed->effect == SWIPE_DOWN) || (swiperLed->effect == SWIPE_UP) || (swiperLed->effect == SWIPE_LEFT) || (swiperLed->effect == SWIPE_RIGHT)) {

		int tmp = swiperLed->effectBuf[swiperLed->effectBufLen - 1];
		for (int i = 0; i < swiperLed->effectBufLen - 1; i++) {
			swiperLed->effectBuf[swiperLed->effectBufLen - i - 1] = swiperLed->effectBuf[swiperLed->effectBufLen - i - 2];
		}
		swiperLed->effectBuf[0] = tmp;

		/*
		 printf("effectBuf:");
		 for (int i = 0; i < swiperLed->effectBufLen; i++) {
		    printf("%d-", swiperLed->effectBuf[i]);
		 }
		 printf("\r\n");
		 */

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

		for (int y = 0; y < rotateNum; y++) {
			uint8_t tmp = swiperLed->ledBrightMass[swiperLed->num_led - 1];
			for (int i = 1; i < swiperLed->num_led + 1; i++) {
				swiperLed->ledBrightMass[swiperLed->num_led - i] = swiperLed->ledBrightMass[swiperLed->num_led - 1 - i];
			}
			swiperLed->ledBrightMass[0] = tmp;
		}

		//printf("Tick:%d LedBrigtMass-", swiperLed->tick);
		for (int i = 0; i < swiperLed->num_led; i++) {
			swiperLed->HSV.v = swiperLed->ledBrightMass[i];
			RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
			//printf("%d-", swiperLed->ledBrightMass[i]);
			//todo
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            //printf("led:%d r:%d g:%d b:%d", i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            // PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		//printf("\r\n");

		///PwmLed_light(swiperLed.pwmLed);

	}

	//return swiperLed.state;
}


void swiperLed_task(void *arg){
    
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }
    
    uint16_t num_of_led=16;
    if (strstr(me_config.slot_options[slot_num], "numOfLed") != NULL) {
		num_of_led = get_option_int_val(slot_num, "numOfLed");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
	}

    float maxBright = 1.0;
    if (strstr(me_config.slot_options[slot_num], "maxBright") != NULL) {
		maxBright = get_option_float_val(slot_num, "maxBright");
        if(maxBright>1.0)maxBright=1.0;
		ESP_LOGD(TAG, "Set maxBright:%f for slot:%d",maxBright, slot_num);
	}

    float minBright = 0.0;
    if (strstr(me_config.slot_options[slot_num], "minBright") != NULL) {
		minBright = get_option_float_val(slot_num, "minBright");
        if(minBright<0.0)minBright=0.0;
		ESP_LOGD(TAG, "Set minBright:%f for slot:%d",minBright, slot_num);
	}

    uint16_t refreshPeriod = 25;
    if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=250
    };
    //HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGBcolor") != NULL) {
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

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);
  
    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "actionTopic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/swiperLed_0")+3];
		sprintf(t_str, "%s/swiperLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    swiperLed_HandleTypeDef swiperLed;
    swiperLed.num_led = num_of_led;
    swiperLed.pixelBuffer = led_strip_pixels;
    swiperLed.maxBright = maxBright;
    swiperLed.minBright = minBright;
    swiperLed.RGB.r = targetRGB.r;
    swiperLed.RGB.g = targetRGB.g;
    swiperLed.RGB.b = targetRGB.b;
    swiperLed.HSV= RgbToHsv((RgbColor)targetRGB);

    swiperLed.ledBrightMass = (uint8_t*) malloc(swiperLed.num_led * sizeof(uint8_t));
	swiperLed.ledAngleRadian = 2 * M_PI / swiperLed.num_led;
	swiperLed.ledsCoordinate = (uint16_t*) malloc(swiperLed.num_led / 2 * sizeof(uint16_t));
	swiperLed.state = LED_STOP;

    setMinBright(&swiperLed);

    ESP_LOGD(TAG, "swiper task config end. Slot_num:%d", slot_num);


    #define LED_EFFECT_LENGTH 30
    TickType_t lastWakeTime = xTaskGetTickCount(); 

    while (1) {
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]+1);

            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            
            if(strstr(cmd, "swipe")!=NULL){
                if(strstr(payload, "up")!=NULL){
                    setLedEffect(&swiperLed, SWIPE_UP, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "down")!=NULL){
                    setLedEffect(&swiperLed, SWIPE_DOWN, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "left")!=NULL){
                    setLedEffect(&swiperLed, SWIPE_LEFT, LED_EFFECT_LENGTH);
                }else if(strstr(payload, "right")!=NULL){
                    setLedEffect(&swiperLed, SWIPE_RIGHT, LED_EFFECT_LENGTH);
                }
            } 
        }
    
        if(swiperLed.state == LED_RUN){
            processLedEffect(&swiperLed);
        }
          
        rmt_createAndSend(&rmt_slot_heap, led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
        //vTaskDelay(pdMS_TO_TICKS(refreshPeriod));
    }
}

void start_swiperLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(swiperLed_task, "swiperLed_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"swiperLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//---------------------LED RING-----------------------------
void calcRingBrightness(float *brightArray, uint16_t num_of_led, float currentPos, uint16_t effectLen, uint16_t numOfPos, float maxBright, float minBright) {
    // Convert position to LED scale
    //ESP_LOGD(TAG, "calcRingBrightness: currentPos:%f effectLen:%d numOfPos:%d num_of_led:%d", currentPos, effectLen, numOfPos, num_of_led);
    
    float positionScale = (float)num_of_led / numOfPos;
    float centrPos = (currentPos * positionScale);
    
    // Calculate half effect length
    uint16_t halfEffect = effectLen/2;
    
    for(int i = 0; i < num_of_led; i++) {
        // Calculate shortest distance considering ring topology
        float distance = i - centrPos;
        if(distance > num_of_led/2) distance -= num_of_led;
        if(distance < -num_of_led/2) distance += num_of_led;
        distance = fabs(distance);
        
        if(distance <= halfEffect) {
            // Linear interpolation between maxBright and minBright
            float ratio = distance / halfEffect;
            brightArray[i] = maxBright - (maxBright - minBright) * ratio;
        } else {
            brightArray[i] = minBright;
        }
        //printf(" %f", brightArray[i]);
    }
    //printf("\n");
}


void ledRing_task(void *arg){
    
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }
    
    uint16_t num_of_led=16;
    if (strstr(me_config.slot_options[slot_num], "numOfLed") != NULL) {
		num_of_led = get_option_int_val(slot_num, "numOfLed");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
	}

    float maxBright = 1.0;
    if (strstr(me_config.slot_options[slot_num], "maxBright") != NULL) {
		maxBright = get_option_int_val(slot_num, "maxBright");
        if(maxBright>1.0)maxBright=1.0;
		ESP_LOGD(TAG, "Set maxBright:%f for slot:%d",maxBright, slot_num);
	}

    float minBright = 0.0;
    if (strstr(me_config.slot_options[slot_num], "minBright") != NULL) {
		minBright = get_option_int_val(slot_num, "minBright");
        if(minBright<0.0)minBright=0.0;
		ESP_LOGD(TAG, "Set minBright:%f for slot:%d",minBright, slot_num);
	}

    float increment = 1.0;
    if (strstr(me_config.slot_options[slot_num], "increment") != NULL) {
		increment = get_option_float_val(slot_num, "increment");
		ESP_LOGD(TAG, "Set increment:%f for slot:%d  configString:%s",increment, slot_num, me_config.slot_options[slot_num]);
	}

    uint16_t refreshPeriod = 40;
    if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

    uint16_t numOfPos = num_of_led;
    if (strstr(me_config.slot_options[slot_num], "numOfPos") != NULL) {
		numOfPos = (get_option_int_val(slot_num, "numOfPos"));
		ESP_LOGD(TAG, "Set numOfPos:%d for slot:%d",refreshPeriod, slot_num);
	}

    uint16_t effectLen = num_of_led/4;
    if (strstr(me_config.slot_options[slot_num], "effectLen") != NULL) {
		effectLen = (get_option_int_val(slot_num, "effectLen"));
		ESP_LOGD(TAG, "Set effectLen:%d for slot:%d", effectLen, slot_num);
	}

    uint8_t state=0;
    if (strstr(me_config.slot_options[slot_num], "defaultState") != NULL) {
		state = get_option_int_val(slot_num, "defaultState");
		ESP_LOGD(TAG, "Set def_state:%d for slot:%d",state, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=250,
        .g=0,
        .b=250
    };
    //HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGBcolor") != NULL) {
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

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);
  
    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "actionTopic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ledRing_0")+3];
		sprintf(t_str, "%s/ledRing_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    float currentBrightness[num_of_led];
    for(int i=0;i<num_of_led;i++){
        currentBrightness[i]= (state==0) ? minBright : maxBright;
    }

    float currentPos=0;
    int16_t targetPos=numOfPos-1;


    
    //vTaskDelay(pdMS_TO_TICKS(1000));
    TickType_t lastWakeTime = xTaskGetTickCount(); 
    while(1){
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
                }else if(strstr(cmd, "setPos")!=NULL){
                    targetPos = atoi(payload);
                }
            }
        }
        
        if(fabs(currentPos-targetPos)>increment/2){
            ESP_LOGD(TAG, "currentPos:%f targetPos:%d", currentPos, targetPos);
            if(fabs(currentPos-targetPos)<numOfPos/2){
                currentPos = (currentPos>targetPos) ? currentPos-increment : currentPos+increment;
            }else{
                currentPos = (currentPos>targetPos) ? currentPos+increment : currentPos-increment;
            }
            if(currentPos<0){
                currentPos=currentPos+(numOfPos);
            }else if(currentPos>numOfPos-1){
                currentPos=currentPos-(numOfPos);;
            }

            calcRingBrightness(currentBrightness, num_of_led, currentPos, effectLen, numOfPos, maxBright, minBright);
            for(int i=0; i<num_of_led; i++){
                uint8_t R = targetRGB.r*currentBrightness[i];
                uint8_t G = targetRGB.g*currentBrightness[i];
                uint8_t B = targetRGB.b*currentBrightness[i];
                led_strip_set_pixel(&led_strip_pixels, i, R,G,B);
            }
            rmt_createAndSend(&rmt_slot_heap, &led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
        }
        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
    }
}

void start_ledRing_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(ledRing_task, "ledRing_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"ledRing_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}