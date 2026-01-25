#ifndef BUTTONLEDS_H
#define BUTTONLEDS_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"

extern SemaphoreHandle_t rmt_semaphore;

void start_button_led_task(int slot_num);
void start_button_smartLed_task(int slot_num);
void start_button_ledRing_task(int slot_num);
void start_button_swiperLed_task(int slot_num);
void start_button_ledBar_task(int slot_num);

// const char * get_manifest_button_led();
// const char * get_manifest_button_smartLed();
// const char * get_manifest_button_ledRing();
// const char * get_manifest_button_swiperLed();

#endif // BUTTONLEDS_H
