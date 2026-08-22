// ***************************************************************************
// TITLE
//     Art-Net (ArtDMX / ArtPoll) - системный приёмник
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef __ARTNET_H__
#define __ARTNET_H__

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define ARTNET_PORT             6454
#define ARTNET_UNIVERSE_SIZE    512

/* Сколько разных Port-Address устройство способно обслуживать одновременно.
   Каждый занимает 512 байт буфера, поэтому потолок сознательно низкий. */
#define ARTNET_MAX_UNIVERSES    8

/* Портов в одном ArtPollReply - жёсткая величина протокола. Юниверсы, у
   которых старшие 11 бит Port-Address различаются, не помещаются в один
   ответ и рассылаются отдельными пакетами с разным BindIndex. */
#define ARTNET_PORTS_PER_REPLY  4

/* Подписчиков (слотов) на один юниверс. Лента и crosslink спокойно живут
   на одном Port-Address, буфер у них общий. */
#define ARTNET_MAX_SUBS         4

typedef struct {
	uint16_t          universe;                          /* Port-Address 0..32767            */
	uint16_t          length;                            /* длина данных последнего кадра    */
	uint8_t           data[ARTNET_UNIVERSE_SIZE];
	int64_t           last_rx_us;                        /* esp_timer_get_time() кадра       */
	uint8_t           seq;                               /* последний принятый Sequence      */
	uint8_t           inUse;
	uint32_t          frames;                            /* принято кадров с момента старта  */
	int8_t            subSlot[ARTNET_MAX_SUBS];          /* -1 = пусто                       */
	TaskHandle_t      subTask[ARTNET_MAX_SUBS];
	SemaphoreHandle_t lock;                              /* держать на время копирования     */
} artnet_universe_t;

/* Подготовка реестра юниверсов - вызывать из main.c ДО init_slots(), иначе
   слот-модули не найдут сервис в момент подписки. Сокет здесь не открывается:
   сеть на этот момент ещё не поднята.
   Возвращает ESP_ERR_INVALID_STATE, если протокол выключен в конфигурации. */
esp_err_t init_artNet(void);

/* Открыть сокет и запустить приёмник - вызывать после подъёма интерфейсов,
   рядом с прочими сетевыми сервисами. */
void start_artNet_task(void);

/* Протокол включён в конфигурации и реестр готов. */
bool artnet_is_enabled(void);

/* Подписка слота на юниверс. NULL при ошибке (реестр полон или протокол
   выключен). notify_task получает xTaskNotifyGive() на каждый принятый кадр,
   допускается NULL. Повторная подписка того же слота возвращает тот же буфер. */
artnet_universe_t * artnet_subscribe(uint16_t universe, int slot_num, TaskHandle_t notify_task);

void artnet_unsubscribe(uint16_t universe, int slot_num);

/* Поток по юниверсу жив с учётом artNet_timeout. */
bool artnet_is_alive(const artnet_universe_t *u);

/* === Слот-модули === */

void start_artNetCrosslink_task(int slot_num);

/* Строка состояния для статус-отчёта: "ArtNet 2u 1234f" либо "ArtNet off". */
const char * artnetGetStatusString(void);

#endif // __ARTNET_H__
