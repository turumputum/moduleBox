#include <stdint.h>
#include <string.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t state;
    uint8_t prevState;

    uint16_t currentPos;
    uint16_t prevPos;

    uint16_t maxVal;
    uint16_t minVal;

    uint16_t threshold;
    uint8_t inverse;
    uint16_t deadBand;
    float k;
    uint32_t fall_lag;

    uint8_t flag_float_output;

    TickType_t lastTick;
    TickType_t debounceGap;
    TickType_t cooldownTime;
    TickType_t cooldownStartTick;
    uint8_t inCooldown;

    ledc_channel_config_t ledc_chan;
    
    // Report IDs
    int distanceReport;
} distanceSens_t;

#define DISTANCE_SENS_DEFAULT() {\
    .state = 0,\
    .prevState = 0,\
    .currentPos = 0,\
    .prevPos = 0,\
    .maxVal = 65535,\
    .minVal = 0,\
    .threshold = 0,\
    .inverse = 0,\
    .deadBand = 0,\
    .k = 1.0,\
    .fall_lag = 3000*1000,\
    .flag_float_output=0,\
    .lastTick=0,\
    .debounceGap=0,\
    .cooldownTime=0,\
    .cooldownStartTick=0,\
    .inCooldown=0,\
    .distanceReport=-1,\
}

// Common functions
uint16_t crc16_modbus(uint8_t *data, uint8_t length);
void distanceSens_config(distanceSens_t *distanceSens, uint8_t slot_num);
void distanceSens_report(distanceSens_t *distanceSens, uint8_t slot_num);

// Task start functions
void start_tofxxxfuart_task(int slot_num);
void start_benewakeTOF_task(int slot_num);
void start_hlk2410_task(int slot_num);
void start_sr04m_task(int slot_num);