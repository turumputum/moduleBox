#include "driver/adc.h"
#include "esp_adc_cal.h"

#define ANALOGIN_OVERSAMPLING 32//8

typedef struct {
    adc_channel_t chan;
    adc_unit_t adc;
    uint8_t samples;
    int val;
    int _val;
    //int __val;
    
    int integrated;
    int integrated2;
    int trig;
    int _trig;
    
} ChanCfg;

void start_tenzo_button_task(int slot_num);