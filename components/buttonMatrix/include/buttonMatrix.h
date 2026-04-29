#ifndef BUTTON_MATRIX_H
#define BUTTON_MATRIX_H

#include <stdint.h>

#define BUTTON_MATRIX_MAX_SLOTS 5

typedef struct {
    int      outSlots[BUTTON_MATRIX_MAX_SLOTS];
    int      outSlotsCount;
    int      inSlots[BUTTON_MATRIX_MAX_SLOTS];
    int      inSlotsCount;

    uint8_t  out_pin[BUTTON_MATRIX_MAX_SLOTS * 3];
    uint8_t  in_pin[BUTTON_MATRIX_MAX_SLOTS * 3];

    char    *charMap;
    int      charMapSize;

    int      charReport;
} buttonMatrix_t;

#define BUTTON_MATRIX_DEFAULT() {\
    .outSlotsCount = 0,\
    .inSlotsCount  = 0,\
    .charMap       = NULL,\
    .charMapSize   = 0,\
    .charReport    = -1,\
}

void configure_buttonMatrix(buttonMatrix_t *ctx, int slot_num);
void start_buttonMatrix_task(int slot_num);

#endif // BUTTON_MATRIX_H
