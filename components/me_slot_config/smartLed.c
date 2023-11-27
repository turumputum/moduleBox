#include "smartLed.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "reporter.h"
#include "stateConfig.h"
#include "executor.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SMART_LED";

extern const uint8_t gamma_8[256];

RgbColor HsvToRgb(HsvColor hsv) {
	RgbColor rgb;
	unsigned char region, remainder, p, q, t;

	if (hsv.s == 0) {
		rgb.r = hsv.v;
		rgb.g = hsv.v;
		rgb.b = hsv.v;
		return rgb;
	}

	region = hsv.h / 43;
	remainder = (hsv.h - (region * 43)) * 6;

	p = (hsv.v * (255 - hsv.s)) >> 8;
	q = (hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8;
	t = (hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8;

	switch (region) {
	case 0:
		rgb.r = hsv.v;
		rgb.g = t;
		rgb.b = p;
		break;
	case 1:
		rgb.r = q;
		rgb.g = hsv.v;
		rgb.b = p;
		break;
	case 2:
		rgb.r = p;
		rgb.g = hsv.v;
		rgb.b = t;
		break;
	case 3:
		rgb.r = p;
		rgb.g = q;
		rgb.b = hsv.v;
		break;
	case 4:
		rgb.r = t;
		rgb.g = p;
		rgb.b = hsv.v;
		break;
	default:
		rgb.r = hsv.v;
		rgb.g = p;
		rgb.b = q;
		break;
	}

	return rgb;
}
HsvColor RgbToHsv(RgbColor rgb) {
	HsvColor hsv;
	unsigned char rgbMin, rgbMax;

	rgbMin = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b) : (rgb.g < rgb.b ? rgb.g : rgb.b);
	rgbMax = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b) : (rgb.g > rgb.b ? rgb.g : rgb.b);

	hsv.v = rgbMax;
	if (hsv.v == 0) {
		hsv.h = 0;
		hsv.s = 0;
		return hsv;
	}

	hsv.s = 255 * (rgbMax - rgbMin) / hsv.v;
	if (hsv.s == 0) {
		hsv.h = 0;
		return hsv;
	}

	if (rgbMax == rgb.r)
		hsv.h = 0 + 43 * (rgb.g - rgb.b) / (rgbMax - rgbMin);
	else if (rgbMax == rgb.g)
		hsv.h = 85 + 43 * (rgb.b - rgb.r) / (rgbMax - rgbMin);
	else
		hsv.h = 171 + 43 * (rgb.r - rgb.g) / (rgbMax - rgbMin);

	return hsv;
}

void checkColorAndBright(RgbColor *currentRGB, RgbColor *targetRGB, uint16_t *currentBright, uint16_t *targetBright, uint16_t fade_increment){
    if(currentRGB!=targetRGB){
        if(currentRGB->r < targetRGB->r){
            if((targetRGB->r - currentRGB->r) < fade_increment){
               currentRGB->r = targetRGB->r;
            }else{
                currentRGB->r += fade_increment;
            }
        }else if(currentRGB->r > targetRGB->r){
            if((currentRGB->r - targetRGB->r) < fade_increment){
               currentRGB->r = targetRGB->r;
            }else{
                currentRGB->r -= fade_increment;
            }
        }

        if(currentRGB->g < targetRGB->g){
            if((targetRGB->g - currentRGB->g) < fade_increment){
               currentRGB->g = targetRGB->g;
            }else{
                currentRGB->g += fade_increment;
            }
        }else if(currentRGB->g > targetRGB->g){
            if((currentRGB->g - targetRGB->g) < fade_increment){
               currentRGB->g = targetRGB->g;
            }else{
                currentRGB->g -= fade_increment;
            }
        }

        if(currentRGB->b < targetRGB->b){
            if((targetRGB->b - currentRGB->b) < fade_increment){
               currentRGB->b = targetRGB->b;
            }else{
                currentRGB->b += fade_increment;
            }
        }else if(currentRGB->b > targetRGB->b){
            if((currentRGB->b - targetRGB->b) < fade_increment){
               currentRGB->b = targetRGB->b;
            }else{
                currentRGB->b -= fade_increment;
            }
        }

    }
    if(currentBright!=targetBright){
        if(currentBright < targetBright){
            if((targetBright - currentBright) < fade_increment){
              currentBright = targetBright;
            }else{
                currentBright += fade_increment;
            }
        }else if(currentBright > targetBright){
            if((currentBright - targetBright) < fade_increment){
               currentBright = targetBright;
            }else{
                currentBright -= fade_increment;
            }
        }
    }
}

void setAllLed_color(led_strip_handle_t led_strip, RgbColor color, uint16_t num_of_led){
    for(int i=0; i<num_of_led; i++){
        led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
    }
}

void smartLed_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    uint8_t state = 0;

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    //vQueueAddToRegistry( xQueue, "AMeaningfulName" );
    
    uint16_t num_of_led=24;
    if (strstr(me_config.slot_options[slot_num], "num_of_led") != NULL) {
		num_of_led = get_option_int_val(slot_num, "num_of_led");
		ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",num_of_led, slot_num);
	}

    uint8_t inverse = 0;
    if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
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
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "RGB_color");
        char *rest;
        targetRGB.r = atoi(strtok_r(tmp," ",&rest));
        targetRGB.g = atoi(strtok_r(rest," ",&rest));
        targetRGB.b = atoi(strtok_r(rest," ",&rest));
		HSV = RgbToHsv(targetRGB);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", targetRGB.r, targetRGB.g, targetRGB.b, slot_num);

    char* ledMode;
    if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "ledMode");
		ledMode=strdup(tmp);
		ESP_LOGD(TAG, "Custom ledMode:%s", ledMode);
    }else{
        ledMode=strdup("default");
    }

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

    // LED strip general initialization, according to your led board design
    led_strip_config_t strip_config = {
        .strip_gpio_num = pin_num,   // The GPIO that connected to the LED strip's data line
        .max_leds = num_of_led,        // The number of LEDs in the strip,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB, // Pixel format of your LED strip
        .led_model = LED_MODEL_WS2812,            // LED strip model
        .flags.invert_out = false,                // whether to invert the output signal
    };

    // LED strip backend configuration: RMT
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
        .resolution_hz = (10 * 1000 * 1000), //Hz RMT counter clock frequency
        .flags.with_dma = true,               // DMA feature is available on ESP target like ESP32-S3
    };

    // LED Strip object handle
    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGD(TAG, "Smart led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));


    // for (int i = 0; i < 24; i++) {
    //     ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, i, 250, 250, 250));
    // }
    // ESP_ERROR_CHECK(led_strip_refresh(led_strip));

    // vTaskDelay(pdMS_TO_TICKS(1000));
    // ESP_ERROR_CHECK(led_strip_clear(led_strip));

    uint8_t currentBright=0;
    uint8_t targetBright=min_bright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };
    while (1) {
        startTick = xTaskGetTickCount();

        command_message_t msg;
        // if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
        //     //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
        // }

        if (strstr(ledMode, "default") != NULL){
            checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
            setAllLed_color(led_strip, currentRGB, num_of_led);
            led_strip_refresh(led_strip);
        }

        uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d", delay); 
        vTaskDelay(pdMS_TO_TICKS(delay));
    }

}


void start_smartLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreate(smartLed_task, "smartLed_task", 1024*4, &slot_num,12, NULL);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}