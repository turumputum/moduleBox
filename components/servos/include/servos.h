// ***************************************************************************
// TITLE
//     Сервоприводы с управлением по длительности импульса (PPM/RC)
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef __SERVOS_H__
#define __SERVOS_H__

#include <stdint.h>

// Одна серва на слот. Сигнальный пин - SLOTS_PIN_MAP[slot_num][1].
void start_PPMservo_task(int slot_num);

#endif // __SERVOS_H__
