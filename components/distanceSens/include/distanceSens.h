#include <stdint.h>
#include <string.h>

typedef struct {
//	uint8_t changeTrack;
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
    .flag_float_output=0\
}


void start_VL53TOF_task(int slot_num);
void start_benewakeTOF_task(int slot_num);
