// ***************************************************************************
// TITLE
//     artNetCrosslink - проброс DMX-каналов в шину событий moduleBox
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "stateConfig.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include "stdcommand.h"
#include "executor.h"
#include "reporter.h"
#include "artNet.h"
#include <mbdebug.h>

#include <generated_files/gen_artNetCrosslink.h>

/* CONFIG_LOG_MAXIMUM_LEVEL в проекте = INFO, ESP_LOGD включается локально. */
#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

static const char *TAG = "ARTNET_XL";

extern configuration me_config;
extern stateStruct   me_state;

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

/* Потолок каналов на слот. Определён не памятью, а таблицей отчётов:
   MAX_NUM_OF_STDREPORTS равен 100 НА ВСЁ УСТРОЙСТВО, и её делят все слоты.
   Плюс каждый подтопик обязан быть литералом, иначе манифест соврёт. */
#define ARTXL_MAX_CHANNELS      16

#define ARTXL_WIDTH_BYTE        0
#define ARTXL_WIDTH_WORD        1

#define ARTXL_ORDER_HILO        0
#define ARTXL_ORDER_LOHI        1

/* Период холостого пробуждения: команды и таймаут потока обслуживаются даже
   когда кадры не приходят. При живом потоке задачу будит уведомление. */
#define ARTXL_IDLE_TICK_MS      100

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct {
	STDCOMMANDS         cmds;

	int                 universe;
	int                 startAddress;       /* 1-based, как на пульте       */
	int                 numOfChannels;
	int                 chWidth;
	int                 wordOrder;
	int                 deadBand;
	int                 minInterval;        /* мс                           */
	int                 sendOnStart;
	int                 sendOnTimeout;
	int                 active_state;

	int                 chReport            [ ARTXL_MAX_CHANNELS ];
	int                 streamReport;

	uint16_t            last                [ ARTXL_MAX_CHANNELS ];
	int64_t             lastSentUs          [ ARTXL_MAX_CHANNELS ];
	uint8_t             primed;             /* last[] заполнен первым кадром */
} artxl_ctx_t;

// ---------------------------------------------------------------------------
// ------------------------------ CONFIGURATION ------------------------------
// -----|-------------------|-------------------------------------------------

/*
	artNetCrosslink - следит за диапазоном DMX-каналов Art-Net и публикует
	их значения событиями moduleBox
	Пинов не занимает, ставится в любой слот, в том числе виртуальный
    slots: 0-9
*/
static void configure_artNetCrosslink(artxl_ctx_t *c, int slot_num)
{
	stdcommand_init(&c->cmds, slot_num);

	/* Art-Net Port-Address источника, отсчёт от нуля - многие пульты
	   показывают его как Universe 1, По умолчанию 0
	*/
	c->universe = get_option_int_val(slot_num, "universe", "", 0, 0, 32767);
	ESP_LOGD(TAG, "Set universe:%d for slot:%d", c->universe, slot_num);

	/* DMX-адрес первого отслеживаемого канала, отсчёт от единицы, По умолчанию 1
	*/
	c->startAddress = get_option_int_val(slot_num, "startAddress", "", 1, 1, 512);
	ESP_LOGD(TAG, "Set startAddress:%d for slot:%d", c->startAddress, slot_num);

	/* Сколько каналов подряд отслеживать, По умолчанию 8
	*/
	c->numOfChannels = get_option_int_val(slot_num, "numOfChannels", "", 8, 1, 16);
	ESP_LOGD(TAG, "Set numOfChannels:%d for slot:%d", c->numOfChannels, slot_num);

	/* Разрядность значения: byte - один DMX-канал 0-255, word - два соседних канала 0-65535
	*/
	if ((c->chWidth = get_option_enum_val(slot_num, "chWidth", "byte", "word", NULL)) < 0)
	{
		ESP_LOGE(TAG, "chWidth: unrecognized value, fallback to byte");
		c->chWidth = ARTXL_WIDTH_BYTE;
	}

	/* Порядок байтов при chWidth word: hiLo - старший первым (обычный DMX coarse-fine), loHi - наоборот
	*/
	if ((c->wordOrder = get_option_enum_val(slot_num, "wordOrder", "hiLo", "loHi", NULL)) < 0)
	{
		ESP_LOGE(TAG, "wordOrder: unrecognized value, fallback to hiLo");
		c->wordOrder = ARTXL_ORDER_HILO;
	}

	/* Минимальное изменение значения для выдачи события, По умолчанию 2, для chWidth word разумно 64
	*/
	c->deadBand = get_option_int_val(slot_num, "deadBand", "", 2, 0, 4096);
	ESP_LOGD(TAG, "Set deadBand:%d for slot:%d", c->deadBand, slot_num);

	/* Минимальный интервал между событиями одного канала в миллисекундах, По умолчанию 30
	*/
	c->minInterval = get_option_int_val(slot_num, "minInterval", "ms", 30, 0, 10000);
	ESP_LOGD(TAG, "Set minInterval:%d for slot:%d", c->minInterval, slot_num);

	/* Флаг - выдать значения всех каналов по первому принятому кадру
	*/
	c->sendOnStart = get_option_flag_val(slot_num, "sendOnStart");

	/* Флаг - повторить последние значения каналов при пропадании потока
	*/
	c->sendOnTimeout = get_option_flag_val(slot_num, "sendOnTimeout");

	/* Флаг - оставить модуль спящим до команды action/enable 1
	*/
	c->active_state = get_option_flag_val(slot_num, "disableOnStart") ? 0 : 1;

	/* Диапазон обязан помещаться в юниверс - иначе читали бы соседний. */
	{
		int step = (c->chWidth == ARTXL_WIDTH_WORD) ? 2 : 1;
		int span = c->numOfChannels * step;

		if ((c->startAddress - 1 + span) > ARTNET_UNIVERSE_SIZE)
		{
			int fit = (ARTNET_UNIVERSE_SIZE - (c->startAddress - 1)) / step;

			if (fit < 1) fit = 1;

			ESP_LOGE(TAG, "slot_%d: startAddress %d + %d channels do not fit in universe, numOfChannels cut to %d",
					slot_num, c->startAddress, c->numOfChannels, fit);
			mblog(E, "artNetCrosslink slot_%d: numOfChannels cut to %d", slot_num, fit);

			c->numOfChannels = fit;
		}
	}

	/* Топик по умолчанию: <deviceName>/artNetCrosslink_<slot_num>
	   Суффиксы /action/ и /event/ добавляются при приёме команд и публикации.
	   Заполняем обе таблицы - без trigger_topic_list репортер не публикует. */
	{
		char t_str[strlen(me_config.deviceName) + strlen("/artNetCrosslink_0") + 3];
		sprintf(t_str, "%s/artNetCrosslink_%d", me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]  = strdup(t_str);
		me_state.trigger_topic_list[slot_num] = strdup(t_str);
		ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
	}

	/* === COMMANDS === */

	/* Включить-выключить модуль, Значение 0-1, По умолчанию 1, Флаг disableOnStart выключает модуль при старте
	*/
	stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

	/* === EVENTS === */

	/* Состояние модуля - активен (1) или спит (0)
	*/
	stdreport_register(RPTT_int, slot_num, "", "event/enable");

	/* Состояние потока Art-Net по юниверсу - 1 кадры идут, 0 таймаут
	*/
	c->streamReport = stdreport_register(RPTT_int, slot_num, "", "event/stream");

	/* Значение DMX-канала 0, отсчёт от startAddress
	*/
	if (c->numOfChannels > 0)  c->chReport[0]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_0/val");

	/* Значение DMX-канала 1, отсчёт от startAddress
	*/
	if (c->numOfChannels > 1)  c->chReport[1]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_1/val");

	/* Значение DMX-канала 2, отсчёт от startAddress
	*/
	if (c->numOfChannels > 2)  c->chReport[2]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_2/val");

	/* Значение DMX-канала 3, отсчёт от startAddress
	*/
	if (c->numOfChannels > 3)  c->chReport[3]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_3/val");

	/* Значение DMX-канала 4, отсчёт от startAddress
	*/
	if (c->numOfChannels > 4)  c->chReport[4]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_4/val");

	/* Значение DMX-канала 5, отсчёт от startAddress
	*/
	if (c->numOfChannels > 5)  c->chReport[5]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_5/val");

	/* Значение DMX-канала 6, отсчёт от startAddress
	*/
	if (c->numOfChannels > 6)  c->chReport[6]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_6/val");

	/* Значение DMX-канала 7, отсчёт от startAddress
	*/
	if (c->numOfChannels > 7)  c->chReport[7]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_7/val");

	/* Значение DMX-канала 8, отсчёт от startAddress
	*/
	if (c->numOfChannels > 8)  c->chReport[8]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_8/val");

	/* Значение DMX-канала 9, отсчёт от startAddress
	*/
	if (c->numOfChannels > 9)  c->chReport[9]  = stdreport_register(RPTT_int, slot_num, "", "event/ch_9/val");

	/* Значение DMX-канала 10, отсчёт от startAddress
	*/
	if (c->numOfChannels > 10) c->chReport[10] = stdreport_register(RPTT_int, slot_num, "", "event/ch_10/val");

	/* Значение DMX-канала 11, отсчёт от startAddress
	*/
	if (c->numOfChannels > 11) c->chReport[11] = stdreport_register(RPTT_int, slot_num, "", "event/ch_11/val");

	/* Значение DMX-канала 12, отсчёт от startAddress
	*/
	if (c->numOfChannels > 12) c->chReport[12] = stdreport_register(RPTT_int, slot_num, "", "event/ch_12/val");

	/* Значение DMX-канала 13, отсчёт от startAddress
	*/
	if (c->numOfChannels > 13) c->chReport[13] = stdreport_register(RPTT_int, slot_num, "", "event/ch_13/val");

	/* Значение DMX-канала 14, отсчёт от startAddress
	*/
	if (c->numOfChannels > 14) c->chReport[14] = stdreport_register(RPTT_int, slot_num, "", "event/ch_14/val");

	/* Значение DMX-канала 15, отсчёт от startAddress
	*/
	if (c->numOfChannels > 15) c->chReport[15] = stdreport_register(RPTT_int, slot_num, "", "event/ch_15/val");

	/* Одна строка на слот в лог: по ней видно, что модуль поднялся и с какими
	   параметрами, без вычитывания десятка отладочных строк. */
	ESP_LOGI(TAG, "slot_%d ready: universe %d, DMX %d-%d, %s, deadBand %d, minInterval %d ms",
			slot_num,
			c->universe,
			c->startAddress,
			c->startAddress - 1 + c->numOfChannels * ((c->chWidth == ARTXL_WIDTH_WORD) ? 2 : 1),
			(c->chWidth == ARTXL_WIDTH_WORD) ? "word" : "byte",
			c->deadBand,
			c->minInterval);

	/* Таблица отчётов общая на устройство и могла кончиться на соседних слотах. */
	for (int i = 0; i < c->numOfChannels; i++)
	{
		if (c->chReport[i] < 0)
		{
			ESP_LOGE(TAG, "slot_%d: report table is full, channel %d is not published", slot_num, i);
			mblog(E, "artNetCrosslink slot_%d: report table full at channel %d", slot_num, i);
		}
	}
}

// ---------------------------------------------------------------------------
// -------------------------------- REPORTING --------------------------------
// -----|-------------------|-------------------------------------------------

static void publishAll(artxl_ctx_t *c, int64_t now)
{
	for (int i = 0; i < c->numOfChannels; i++)
	{
		if (c->chReport[i] < 0)
			continue;

		stdreport_i(c->chReport[i], c->last[i]);
		c->lastSentUs[i] = now;
	}
}

// ---------------------------------------------------------------------------
// --------------------------------- TASK ------------------------------------
// -----|-------------------|-------------------------------------------------

static void artNetCrosslink_task(void *arg)
{
	int                 slot_num = (int)(intptr_t)arg;
	artxl_ctx_t         c        = { 0 };
	STDCOMMAND_PARAMS   params   = { 0 };
	uint8_t             window   [ ARTXL_MAX_CHANNELS * 2 ];
	bool                wasAlive = false;

	/* Проверка первым делом, до любых захватов ресурсов: без сервиса модулю
	   нечего слушать, а остальное устройство обязано работать штатно. */
	if (!artnet_is_enabled())
	{
		ESP_LOGE(TAG, "Slot_%d: artNet protocol is disabled in config - module cannot run", slot_num);
		mblog(E, "artNetCrosslink slot_%d: artNet is disabled in config", slot_num);
		vTaskDelete(NULL);
		return;
	}

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

	configure_artNetCrosslink(&c, slot_num);

	artnet_universe_t * u = artnet_subscribe(c.universe, slot_num, xTaskGetCurrentTaskHandle());

	if (u == NULL)
	{
		ESP_LOGE(TAG, "Slot_%d: cannot subscribe to universe %d", slot_num, c.universe);
		mblog(E, "artNetCrosslink slot_%d: subscribe to universe %d failed", slot_num, c.universe);
		vTaskDelete(NULL);
		return;
	}

	waitForWorkPermit(slot_num);

	stdreport_enable(slot_num, c.active_state);

	while (1)
	{
		/* Просыпаемся по приходу кадра; pdTRUE обнуляет счётчик, поэтому
		   пачка кадров схлопывается в один проход - разбирать имеет смысл
		   только последний, промежуточные уже перезаписаны в буфере. */
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ARTXL_IDLE_TICK_MS));

		int cmd;

		while ((cmd = stdcommand_receive(&c.cmds, &params, 0)) != -1)
		{
			switch (cmd)
			{
				case STDCMD_ENABLE:
					if (params.count > 0)
					{
						c.active_state = params.p[0].i ? 1 : 0;
						ESP_LOGD(TAG, "enable:%d slot:%d", c.active_state, slot_num);
						/* event/enable публикуем явно - авто-рассылки нет */
						stdreport_enable(slot_num, c.active_state);
					}
					break;

				default:
					break;
			}
		}

		bool alive = artnet_is_alive(u);

		if (alive != wasAlive)
		{
			wasAlive = alive;

			if (c.active_state && (c.streamReport >= 0))
				stdreport_i(c.streamReport, alive ? 1 : 0);

			if (!alive)
			{
				/* Возврат потока считаем новым стартом: политика sendOnStart
				   отработает заново, а значения из прошлой сессии не станут
				   базой для дедбенда. */
				c.primed = 0;

				if (c.active_state && c.sendOnTimeout)
					publishAll(&c, esp_timer_get_time());
			}

			ESP_LOGI(TAG, "slot_%d universe %d stream %s", slot_num, c.universe, alive ? "up" : "lost");
		}

		if (!c.active_state || !alive)
			continue;

		int step = (c.chWidth == ARTXL_WIDTH_WORD) ? 2 : 1;
		int span = c.numOfChannels * step;
		int base = c.startAddress - 1;

		/* Мьютекс держим только на копировании окна: 16-битное значение,
		   собранное из двух разных кадров, увело бы шаговик на полдиапазона. */
		xSemaphoreTake(u->lock, portMAX_DELAY);
		int avail = (u->length > base) ? (u->length - base) : 0;
		if (avail > span) avail = span;
		if (avail > 0) memcpy(window, u->data + base, avail);
		xSemaphoreGive(u->lock);

		/* Кадр короче нашего окна - хвост считаем нулями, а не мусором. */
		if (avail < span)
			memset(window + avail, 0, span - avail);

		int64_t now = esp_timer_get_time();

		for (int i = 0; i < c.numOfChannels; i++)
		{
			uint16_t v;

			if (c.chWidth == ARTXL_WIDTH_WORD)
			{
				uint8_t a = window[i * 2];
				uint8_t b = window[i * 2 + 1];

				v = (c.wordOrder == ARTXL_ORDER_LOHI)
					? (uint16_t)((b << 8) | a)
					: (uint16_t)((a << 8) | b);
			}
			else
			{
				v = window[i];
			}

			if (!c.primed)
			{
				c.last[i] = v;
				continue;
			}

			int diff = (int)v - (int)c.last[i];
			if (diff < 0) diff = -diff;

			if (diff == 0)
				continue;

			/* Дедбенд считается от последнего ОТПРАВЛЕННОГО значения, поэтому
			   медленный сдвиг фейдера не копится незамеченным. */
			if (diff < c.deadBand)
				continue;

			if (c.minInterval && ((now - c.lastSentUs[i]) < (int64_t)c.minInterval * 1000))
				continue;

			c.last[i]       = v;
			c.lastSentUs[i] = now;

			if (c.chReport[i] >= 0)
				stdreport_i(c.chReport[i], v);
		}

		if (!c.primed)
		{
			c.primed = 1;

			if (c.sendOnStart)
				publishAll(&c, now);
		}
	}
}

void start_artNetCrosslink_task(int slot_num)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	char     tmpString[32];

	sprintf(tmpString, "task_artNetCrosslink_%d", slot_num);
	xTaskCreatePinnedToCore(artNetCrosslink_task, tmpString, 1024 * 4,
			(void*)(intptr_t)slot_num, configMAX_PRIORITIES - 5, NULL, 0);

	ESP_LOGD(TAG, "artNetCrosslink_task created for slot: %d Heap usage: %lu free heap:%u",
			slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_artNetCrosslink()
{
	return manifesto;
}
