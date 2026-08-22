// ***************************************************************************
// TITLE
//     PWMgenerator - ШИМ заданной частоты и скважности на пине слота
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef __PWMGEN_H__
#define __PWMGEN_H__

#include <stdint.h>

/* Один генератор на слот. Выход - SLOTS_PIN_MAP[slot_num][1], тот же пин,
   что у PPMservo. Таймер LEDC тоже общий с сервоприводами - см. комментарий
   к PWMGEN_LEDC_TIMER в PWMgenerator-c. */
void start_PWMgenerator_task(int slot_num);

#endif // __PWMGEN_H__
