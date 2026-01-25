#ifndef LED_TYPES_H
#define LED_TYPES_H

#include <stdint.h>
#include "rgbHsv.h"
#include "stdcommand.h"
#include "driver/rmt_tx.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"

// Common LED config structure
typedef struct __tag_LEDCONFIG
{
    uint16_t                num_of_led;
    uint8_t                 inverse;
    uint8_t                 state;
    int16_t                 fadeTime;
    int16_t                 maxBright;
    int16_t                 minBright;
    uint16_t                refreshPeriod;
    RgbColor                targetRGB;
    int                     ledMode;
    int                     numOfPos;
    uint16_t                effectLen;
    int                     dir;
    int16_t                 offset;
    int16_t                 increment;
    STDCOMMANDS             cmds;
} LEDCONFIG, * PLEDCONFIG;

typedef struct {
    uint32_t resolution; /*!< Encoder resolution, in Hz */
} led_strip_encoder_config_t;

// RMT related types (from smartLed.c)
typedef struct {
    rmt_tx_channel_config_t tx_chan_config;
    rmt_transmit_config_t tx_config;
    led_strip_encoder_config_t encoder_config;
    rmt_channel_handle_t led_chan;
    rmt_encoder_handle_t led_encoder;
} rmt_led_heap_t;

#define RMT_LED_HEAP_DEFAULT() { \
    .tx_chan_config = { \
        .clk_src = RMT_CLK_SRC_DEFAULT, \
        .resolution_hz = 10 * 1000 * 1000, \
        .mem_block_symbols = 64, \
        .trans_queue_depth = 4, \
    }, \
    .tx_config = { \
        .loop_count = 0, \
    }, \
    .encoder_config = { \
        .resolution = 10 * 1000 * 1000, \
    }, \
}

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

// Function prototypes for LED types
void led_strip_set_pixel(uint8_t *pixel_array, int pos, int r, int g, int b);
esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);
uint8_t rmt_createAndSend(rmt_led_heap_t *rmt_slot_heap, uint8_t *led_strip_pixels, uint16_t size, uint8_t slot_num);

void configure_led_basic(PLEDCONFIG ch, int slot_num);
void update_led_basic(PLEDCONFIG c, ledc_channel_config_t *ledc_channel, int16_t *currentBright, int16_t *appliedBright, int16_t *targetBright);

// Swiper needs its own handle due to complexity
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
	uint16_t offset;
} swiper_handle_t;
void update_led_swiper(PLEDCONFIG c, swiper_handle_t *swiper, rmt_led_heap_t *rmt_heap, int slot_num, uint8_t *prevState);

#endif // LED_TYPES_H
