#ifndef LIDARS_H
#define LIDARS_H

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "stdcommand.h"

typedef struct {
    // Distance range (mm)
    uint16_t distMinVal;
    uint16_t distMaxVal;

    // Angle range (degrees, 0-360)
    uint16_t angleMinVal;
    uint16_t angleMaxVal;
    uint16_t angleOffset;

    // Threshold mode
    uint16_t distThreshold;
    uint8_t thresholdInverse;
    uint16_t thresholdHysteresis;

    // Filter
    float filterK;

    // Mode flags
    uint8_t flag_distance_only;

    // Current measurement (closest in sector per scan)
    uint16_t currentDist;
    uint16_t currentAngle;

    // Previous reported values
    uint16_t prevDist;
    uint16_t prevAngle;

    // Filtered distance
    float filteredDist;

    // Threshold state (raw, before inversion)
    uint8_t state;
    uint8_t prevState;

    // Timing
    TickType_t lastTick;
    TickType_t debounceGap;

    // Dead band
    uint16_t deadBand;

    // Motor
    uint8_t motorEnabled;
    uint8_t defaultState;

    // Commands
    STDCOMMANDS cmds;

    // Report IDs
    int distanceReport;
    int angleReport;
    int stateReport;
    int errorReport;
} lidars_t;

#define LIDARS_DEFAULT() {\
    .distMinVal = 0,\
    .distMaxVal = 40000,\
    .angleMinVal = 0,\
    .angleMaxVal = 360,\
    .angleOffset = 0,\
    .distThreshold = 0,\
    .thresholdInverse = 0,\
    .thresholdHysteresis = 0,\
    .filterK = 1.0f,\
    .flag_distance_only = 0,\
    .currentDist = 0,\
    .currentAngle = 0,\
    .prevDist = 0,\
    .prevAngle = 0,\
    .filteredDist = 0.0f,\
    .state = 0,\
    .prevState = 0,\
    .lastTick = 0,\
    .debounceGap = 0,\
    .deadBand = 0,\
    .motorEnabled = 0,\
    .defaultState = 0,\
    .distanceReport = -1,\
    .angleReport = -1,\
    .stateReport = -1,\
    .errorReport = -1,\
}

// Common functions
void lidars_report(lidars_t *lidar, uint8_t slot_num);

// Task start functions
void start_rplidarS1_task(int slot_num);

#endif // LIDARS_H
