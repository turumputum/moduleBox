#ifndef PHONE_DIALER_H
#define PHONE_DIALER_H

#include <stdint.h>
#include <stdcommand.h>
#include <stdreport.h>

typedef struct __tag_DIALER_CONFIG {
    uint16_t waitingTime;
    uint8_t numberMaxLenght;
    uint8_t enaInverse;
    uint8_t pulseInverse;
    int debounceGap;
    int numberReport;
    STDCOMMANDS cmds;
} DIALER_CONFIG, * PDIALER_CONFIG;

void start_dialer_task(int slot_num);

#endif // PHONE_DIALER_H
