#include <stdint.h>
#include "driver/rmt_encoder.h"
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




void start_smartLed_task(int slot_num);
void start_ledRing_task(int slot_num);

