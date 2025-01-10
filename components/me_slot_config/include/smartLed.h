#include <stdint.h>
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
//#include "rgbHsv.h"

#define NONE 0
#define SWIPE_UP 1
#define SWIPE_DOWN 2
#define SWIPE_LEFT 3
#define SWIPE_RIGHT 4
#define WAITING 5
#define FLUSH 6
#define FADE_UP 7
#define FADE_DOWN 8

enum {
	LED_RUN, LED_STOP
};


typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

typedef struct {
    uint32_t resolution; /*!< Encoder resolution, in Hz */
} led_strip_encoder_config_t;



typedef struct {
    rmt_tx_channel_config_t tx_chan_config;
    rmt_channel_handle_t led_chan;
    rmt_transmit_config_t tx_config;

    rmt_encoder_handle_t led_encoder;
    led_strip_encoder_config_t encoder_config;
}rmt_led_heap_t;


#define RMT_LED_HEAP_DEFAULT() {\
    .tx_chan_config.clk_src = RMT_CLK_SRC_DEFAULT,\
    .tx_chan_config.gpio_num = 0,\
    .tx_chan_config.mem_block_symbols = 256,\
    .tx_chan_config.resolution_hz = 10 * 1000 * 1000,\
    .tx_chan_config.trans_queue_depth = 10,\
    .tx_config.loop_count = 0,\
    .encoder_config.resolution = 10 * 1000 * 1000,\
    .led_chan = NULL,\
    .led_encoder = NULL,\
}

void start_smartLed_task(int slot_num);
void start_ledRing_task(int slot_num);
void start_swiperLed_task(int slot_num);
void start_ledBar_task(int slot_num);

