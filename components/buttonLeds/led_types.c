#include "led_types.h"
#include "esp_log.h"
#include "esp_check.h"
#include "me_slot_config.h"
#include "math.h"
#include <string.h>
#include "stateConfig.h"

static const char *TAG = "BUTTON_LEDS";
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

extern configuration me_config;
extern stateStruct me_state;
extern const uint8_t gamma_8[256];
SemaphoreHandle_t rmt_semaphore = NULL;

// --- Common RMT functions ---

void led_strip_set_pixel(uint8_t *pixel_array, int pos, int r, int g, int b){
    pixel_array[pos * 3 + 0]= (uint8_t)g;
    pixel_array[pos * 3 + 1]= (uint8_t)r;
    pixel_array[pos * 3 + 2]= (uint8_t)b;
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

    uint32_t reset_ticks = config->resolution / 1000000 * 300 / 2; // reset code duration defaults to 50us
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
        int fail_count = 0;
        while(rmt_new_tx_channel(&rmt_slot_heap->tx_chan_config, &rmt_slot_heap->led_chan)!= ESP_OK){
            ESP_LOGE(TAG, "lets repeat creat RMT TX channel for slot:%d", slot_num);
            fail_count++;
            if(fail_count>10){
                ESP_LOGE(TAG, "RMT TX channel fail for slot:%d", slot_num);
                xSemaphoreGive(rmt_semaphore);
                return ESP_FAIL;
            }
        }
        rmt_enable(rmt_slot_heap->led_chan);
        esp_err_t err = rmt_transmit(rmt_slot_heap->led_chan, rmt_slot_heap->led_encoder, led_strip_pixels, size, &rmt_slot_heap->tx_config);
        if(err!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX error:%d", err);
        }
        if(rmt_tx_wait_all_done(rmt_slot_heap->led_chan, portMAX_DELAY)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX wait error:%d", err);
        }
        if(rmt_disable(rmt_slot_heap->led_chan)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX disable error:%d", err);
        }
        if(rmt_del_channel(rmt_slot_heap->led_chan)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX del error:%d", err);
        }
        gpio_set_direction(rmt_slot_heap->tx_chan_config.gpio_num, GPIO_MODE_OUTPUT);
        gpio_set_level(rmt_slot_heap->tx_chan_config.gpio_num, 0);
    }else{
        ESP_LOGE(TAG, "RMT semaphore fail for slot:%d", slot_num);
    }
    xSemaphoreGive(rmt_semaphore);
    return ESP_OK;
}

// --- Basic LED logic ---

void configure_led_basic(PLEDCONFIG ch, int slot_num)
{
    
}

static void checkBright(int16_t *currentBright, uint8_t targetBright, uint8_t fade_increment){
	if(*currentBright!=targetBright){
        if(*currentBright < targetBright){
            if((targetBright - *currentBright) < fade_increment){
              	*currentBright = targetBright;
            }else{
                *currentBright += fade_increment;
            }
        }else if(*currentBright > targetBright){
            if((*currentBright - targetBright) < fade_increment){
               	*currentBright = targetBright;
            }else{
                *currentBright -= fade_increment;
            }
        }
    }
}

void update_led_basic(PLEDCONFIG c, ledc_channel_config_t *ledc_channel, int16_t *currentBright, int16_t *appliedBright, int16_t *targetBright)
{
    if (c->ledMode == 1){ // FLASH
        if(*currentBright<=c->minBright){
            *targetBright=c->maxBright;
        }else if(*currentBright>=c->maxBright){
            *targetBright=c->minBright;
        }
    }

    uint8_t fade_increment = (uint8_t)((c->maxBright - c->minBright) * c->refreshPeriod / c->fadeTime);
    if (fade_increment < 1) fade_increment = 1;

    checkBright(currentBright, *targetBright, fade_increment);

    if (*currentBright != *appliedBright)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel->channel, *currentBright);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel->channel);
        *appliedBright = *currentBright;
    }
}


// --- LED Bar logic ---

// static uint8_t colorChek(uint8_t currentColor, uint8_t targetColor, uint8_t increment){
