#ifndef BUTTON_LOGIC_H
#define BUTTON_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "led_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stdreport.h"

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------



typedef enum
{
	BSTYPE_none		= 0,
	BSTYPE_short,
	BSTYPE_long,
	BSTYPE_double,
} BSTYPE;

typedef struct __tag_BUTTONCONFIG
{
	int 					button_inverse;
	int 					debounce_gap;
	int 					event_filter;

	/* Состояние неблокирующего антидребезга (hold-continuously).
	   debounce_gap трактуется как миллисекунды - как во входных модулях. */
	int 					debounce_cand;
	int 					debounce_stable;
	int64_t 				debounce_since_us;
	bool 					debounce_inited;
	int 					stateReport;
	int 					longReport;
	int 					doubleReport;

	uint16_t 				longPressTime;
	uint16_t 				doubleClickTime;

	TickType_t 				pressTimeBegin;
	bool 					longPressSignaled;

	TickType_t 				unpressTimeBegin;
	bool 					dobleClickSignaled;

    uint16_t 				refreshPeriod;
} BUTTONCONFIG, * PBUTTONCONFIG;


typedef struct __tag_ISRCFG
{
	int 			slot_num;
	uint8_t			msg;
} ISRCFG;



typedef struct __tag_MODULE_CONTEXT
{
    BUTTONCONFIG button;
    LEDCONFIG led;
    ISRCFG isrCfgs[2];
} MODULE_CONTEXT, * PMODULE_CONTEXT;

void setup_button_hw(int slot_num, PMODULE_CONTEXT ctx);
void button_logic_update(PBUTTONCONFIG c, int button_state, int slot_num, int *prev_state);

/* Опубликовать текущий уровень кнопки на старте. Вызывать ПОСЛЕ
   waitForWorkPermit (иначе отчёт уйдёт раньше, чем у слотов-приёмников
   кросслинка созданы очереди команд, и стартовое состояние ножки потеряется).

   event/press - это уровень 0-1, а не импульс: button_logic_update шлёт 1 на
   нажатие и 0 на отпускание. Значит подписчик-кросслинк обязан узнать уровень
   ножки на момент включения - retain в системе не используется.

   Заодно снимает стартовые артефакты: prev_state инициализирован -1, поэтому
   первый проход цикла трактовался как "изменение" и при longPressTime+eventFilter
   выдавал фантомный клик (press:1 -> press:0) на НЕнажатой кнопке, а нажатую при
   старте вовсе не рапортовал. Здесь мы задаём prev_state и состояние антидребезга
   реальным уровнем, поэтому цикл ничего лишнего не выдаст.

   pin_level - уже с учётом button_inverse. */
void button_logic_report_initial(PBUTTONCONFIG c, int pin_level, int *prev_state);

/* Неблокирующий антидребезг: новый уровень принимается, только если он
   продержался непрерывно debounce_gap мс. Возвращает текущий стабильный
   уровень кнопки. Состояние хранится в самой структуре c. */
int button_logic_debounce(PBUTTONCONFIG c, int raw_state);

#endif // BUTTON_LOGIC_H
