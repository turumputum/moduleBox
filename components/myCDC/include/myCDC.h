/*
 * cdc.h
 *
 *  Created on: 7 ���. 2022 �.
 *      Author: Yac
 */

#ifndef MAIN_CDC_H_
#define MAIN_CDC_H_

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"



void usb_device_task(void *param);
void usbprintf(char * msg, ...);
void usbprint(char * msg);
void usb_device_task(void *param);
void cdc_task(void *params);

/** Готова ли USB-консоль принимать вывод: хост перечислил устройство.
 *  Обёртка над tud_is_plugged() - чтобы вызывающим (reporter) не тянуть tusb.h
 *  и не опираться на транзитивную зависимость через циклический REQUIRES. */
bool usb_console_ready(void);


#endif /* MAIN_CDC_H_ */
