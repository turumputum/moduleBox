#include <stdint.h>
#include "driver/rmt_encoder.h"

// typedef struct RgbColor {
// 	unsigned char r;
// 	unsigned char g;
// 	unsigned char b;
// } RgbColor;

// typedef struct HsvColor {
// 	unsigned char h;
// 	unsigned char s;
// 	unsigned char v;
// } HsvColor;

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

void start_smartLed_task(int slot_num);