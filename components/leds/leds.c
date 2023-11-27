/*
 * leds.c
 *
 *  Created on: Oct 20, 2020
 *      Author: turum
 */
#include <stdlib.h>
#include <string.h>
#include "stateConfig.h"
#include "leds.h"
#include "esp_log.h"
#include "driver/rmt.h"
//#include "led_strip.h"
#include <math.h>

int effectTick;
float front[LED_COUNT];
int ledBright = 0;

int lightLeds = 0;
int ledStep = 0;

extern stateStruct me_state;
extern configuration me_config;

// RgbColor RGB;
// HsvColor HSV;

int currentBright;

//led_strip_t *strip;

static const char *TAG = "leds";
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// RgbColor HsvToRgb(HsvColor hsv) {
// 	RgbColor rgb;
// 	unsigned char region, remainder, p, q, t;

// 	if (hsv.s == 0) {
// 		rgb.r = hsv.v;
// 		rgb.g = hsv.v;
// 		rgb.b = hsv.v;
// 		return rgb;
// 	}

// 	region = hsv.h / 43;
// 	remainder = (hsv.h - (region * 43)) * 6;

// 	p = (hsv.v * (255 - hsv.s)) >> 8;
// 	q = (hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8;
// 	t = (hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8;

// 	switch (region) {
// 	case 0:
// 		rgb.r = hsv.v;
// 		rgb.g = t;
// 		rgb.b = p;
// 		break;
// 	case 1:
// 		rgb.r = q;
// 		rgb.g = hsv.v;
// 		rgb.b = p;
// 		break;
// 	case 2:
// 		rgb.r = p;
// 		rgb.g = hsv.v;
// 		rgb.b = t;
// 		break;
// 	case 3:
// 		rgb.r = p;
// 		rgb.g = q;
// 		rgb.b = hsv.v;
// 		break;
// 	case 4:
// 		rgb.r = t;
// 		rgb.g = p;
// 		rgb.b = hsv.v;
// 		break;
// 	default:
// 		rgb.r = hsv.v;
// 		rgb.g = p;
// 		rgb.b = q;
// 		break;
// 	}

// 	return rgb;
// }
// HsvColor RgbToHsv(RgbColor rgb) {
// 	HsvColor hsv;
// 	unsigned char rgbMin, rgbMax;

// 	rgbMin = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b) : (rgb.g < rgb.b ? rgb.g : rgb.b);
// 	rgbMax = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b) : (rgb.g > rgb.b ? rgb.g : rgb.b);

// 	hsv.v = rgbMax;
// 	if (hsv.v == 0) {
// 		hsv.h = 0;
// 		hsv.s = 0;
// 		return hsv;
// 	}

// 	hsv.s = 255 * (rgbMax - rgbMin) / hsv.v;
// 	if (hsv.s == 0) {
// 		hsv.h = 0;
// 		return hsv;
// 	}

// 	if (rgbMax == rgb.r)
// 		hsv.h = 0 + 43 * (rgb.g - rgb.b) / (rgbMax - rgbMin);
// 	else if (rgbMax == rgb.g)
// 		hsv.h = 85 + 43 * (rgb.b - rgb.r) / (rgbMax - rgbMin);
// 	else
// 		hsv.h = 171 + 43 * (rgb.r - rgb.g) / (rgbMax - rgbMin);

// 	return hsv;
// }
// //

//void initLeds() {
//	uint32_t startTick = xTaskGetTickCount();
//	uint32_t heapBefore = xPortGetFreeHeapSize();
//
//	RGB.r = me_config.RGB.r;
//	RGB.g = me_config.RGB.g;
//	RGB.b = me_config.RGB.b;
//
//	HSV.h = 0;
//	HSV.s = 255;
//	HSV.v = me_config.brightMax;
//
//	//rmt_config_t config = RMT_DEFAULT_CONFIG_TX(12, RMT_TX_CHANNEL);
//	rmt_config_t config = RMT_DEFAULT_CONFIG_TX(41, RMT_TX_CHANNEL);
//	// set counter clock to 40MHz
//	config.clk_div = 2;
//	config.mem_block_num = 8;
//	ESP_ERROR_CHECK(rmt_config(&config));
//	ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
//
//	// install ws2812 driver
//	led_strip_config_t strip_config =
//	LED_STRIP_DEFAULT_CONFIG(LED_COUNT, (led_strip_dev_t)config.channel);
//	strip = led_strip_new_rmt_ws2812(&strip_config);
//	if (!strip) {
//		ESP_LOGE(TAG, "install WS2812 driver failed");
//	}
//	// Clear LED strip (turn off all LEDs)
//	ESP_ERROR_CHECK(strip->clear(strip, 24));
//
//	for (int t = 0; t < LED_COUNT; t++) {
//		float val = sin((float) (6.28 / LED_COUNT) * (float) t);
//		if (val > 0) {
//			front[t] = val;
//		}
//	}
//	ESP_LOGD(TAG, "Leds init complite. Duration: %d ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
//			xPortGetFreeHeapSize());
//
//}
////
//void refreshLeds() {
//	uint8_t increment = 5;
//
//	if (me_config.rainbow == 1) {
//		if (HSV.h < 255) {
//			HSV.h++;
//		} else {
//			HSV.h = 0;
//		}
//		RGB = HsvToRgb(HSV);
//
//	} else if (me_config.rainbow == 0) {
//		RGB.b = me_config.RGB.b;
//		RGB.r = me_config.RGB.r;
//		RGB.g = me_config.RGB.g;
//	}
//
//	if ((me_state.phoneUp) && (me_config.monofonEnable == 1)) {
//		if (me_config.animate == 1) {
//			for (int i = 0; i < LED_COUNT; i++) {
//				strip->set_pixel(strip, i, (float) RGB.r * front[i], (float) RGB.g * front[i], (float) RGB.b * front[i]);
//
//			}
//			strip->refresh(strip, 1);
//
//			//shift front
//			float tmp = front[0];
//			for (int t = 0; t < (LED_COUNT - 1); t++) {
//				front[t] = front[t + 1];
//			}
//			front[LED_COUNT - 1] = tmp;
//		} else {
//			if (currentBright < me_config.brightMin) {
//				currentBright += increment;
//			} else if (currentBright > me_config.brightMin) {
//				currentBright -= increment;
//			}
//			if (abs(me_config.brightMin - currentBright) < increment) {
//				currentBright = me_config.brightMin;
//			}
//			float tmpBright = ((float) currentBright / 255);
//			for (int i = 0; i < LED_COUNT; i++) {
//
//				strip->set_pixel(strip, i, RGB.r * tmpBright, RGB.g * tmpBright, RGB.b * tmpBright);
//			}
//
//			strip->refresh(strip, 1);
//		}
//
//	} else if (me_config.monofonEnable == 1) {
//		if (currentBright < me_config.brightMax) {
//			currentBright += increment;
//		} else if (currentBright > me_config.brightMax) {
//			currentBright -= increment;
//		}
//		if (abs(me_config.brightMax - currentBright) < increment) {
//			currentBright = me_config.brightMax;
//		}
//		float tmpBright = ((float) currentBright / 255);
//		for (int i = 0; i < LED_COUNT; i++) {
//			strip->set_pixel(strip, i, RGB.r * tmpBright, RGB.g * tmpBright, RGB.b * tmpBright);
//		}
//
//
//		strip->refresh(strip, 1);
//
//	} else if (me_config.monofonEnable == 0) {
//		for (int i = 0; i < LED_COUNT; i++) {
//			strip->set_pixel(strip, i, 0, 0, 0);
//		}
//		strip->refresh(strip, 1);
//		vTaskDelay(pdMS_TO_TICKS(6));
//		//printf("set black\r\n");
//	}
//
//	//ESP_LOGD(TAG, "ReFresh leds complite, Duration: %d ms. Heap usage: %d", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());
//
//}
