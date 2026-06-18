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

/* Неблокирующий антидребезг: новый уровень принимается, только если он
   продержался непрерывно debounce_gap мс. Возвращает текущий стабильный
   уровень кнопки. Состояние хранится в самой структуре c. */
int button_logic_debounce(PBUTTONCONFIG c, int raw_state);

#endif // BUTTON_LOGIC_H
