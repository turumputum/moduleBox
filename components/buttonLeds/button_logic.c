#include "button_logic.h"
#include "esp_log.h"

#include "stateConfig.h"
#include "me_slot_config.h"
#include <string.h>

static const char *TAG = "BUTTON_LEDS";

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

static void IRAM_ATTR button_isr_handler(ISRCFG * cfg)
{
  	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  	xQueueSendFromISR(me_state.interrupt_queue[cfg->slot_num], &cfg->msg, &xHigherPriorityTaskWoken);
  	if (xHigherPriorityTaskWoken == pdTRUE) {
    	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  	}
}

void setup_button_hw(int slot_num, PMODULE_CONTEXT ctx)
{
    uint8_t pin_num = SLOTS_PIN_MAP[slot_num][0];
    ctx->isrCfgs[0].slot_num = slot_num;
    ctx->isrCfgs[0].msg = 0;
    ctx->isrCfgs[1].slot_num = slot_num;
    ctx->isrCfgs[1].msg = 1;

    me_state.interrupt_queue[slot_num] = xQueueCreate(20, sizeof(uint8_t));

    gpio_reset_pin(pin_num);
    esp_rom_gpio_pad_select_gpio(pin_num);
    gpio_config_t in_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .pin_bit_mask = (1ULL << pin_num),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .mode = GPIO_MODE_INPUT
    };
    gpio_config(&in_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pin_num, (gpio_isr_t)button_isr_handler, (void*)&ctx->isrCfgs[0]);
}

static void _buttonStateChanged(BUTTONCONFIG *	c, 
								BSTYPE				type, 
						        int 				button_state, 
						        int 				slot_num)
{
	if (c->doubleClickTime) // Если дабл клик активен и текущее не лонг
	{
		if (button_state) // если было нажатие
		{
			// если отжатие зафиксировано и длительность достигнута 
			if (  c->unpressTimeBegin 																&& 
					(pdTICKS_TO_MS(xTaskGetTickCount() - c->unpressTimeBegin) <= c->doubleClickTime)	)
			{
				c->longPressSignaled	 = false;
				c->dobleClickSignaled = true;

				type = BSTYPE_double;
			}
			else
			{
				c->unpressTimeBegin = 0; 
			}
		}
		else
		{
			if (c->dobleClickSignaled)
			{
				c->unpressTimeBegin 	= 0;
				c->dobleClickSignaled 	= false;

				type = BSTYPE_double;
			}
			else
			{
				c->unpressTimeBegin = xTaskGetTickCount();
			}
		}
	}

	switch (type)
	{
		case BSTYPE_short:
			stdreport_i(c->stateReport, button_state);
			break;
			
		case BSTYPE_long:
			stdreport_i(c->longReport, button_state);
			break;
	
		case BSTYPE_double:
			// Посылаем лишний короткий без фильтра
			if (!c->event_filter)
				stdreport_i(c->stateReport, button_state);

			stdreport_i(c->doubleReport, button_state);
			break;

		default:
			break;
	}
}

void button_logic_update(PBUTTONCONFIG c, int button_state, int slot_num, int *prev_state)
{
    TickType_t now;
    if (c->longPressTime) // Если активен режим длинного нажатия
    {
        if (button_state != *prev_state) // Если было изменение кнопки
        {
            if (button_state)
            {
                c->pressTimeBegin = xTaskGetTickCount();
                // Посылаем лишний короткий при нажатии без фильтра
                if (!c->event_filter) 
                    _buttonStateChanged(c, BSTYPE_short, 1, slot_num);
            }
            else
            {
                if (c->longPressSignaled)
                {
                    // Посылаем лишний короткий при отпускании без фильтра
                    if (!c->event_filter)
                        _buttonStateChanged(c, BSTYPE_short, 0, slot_num);

                    _buttonStateChanged(c, BSTYPE_long, 0, slot_num);

                    c->longPressSignaled = false;
                }
                else
                {
                    // Посылаем задержанный короткий при отпускании если был фильтрован
                    if (c->event_filter)
                        _buttonStateChanged(c, BSTYPE_short, 1, slot_num);
                    _buttonStateChanged(c, BSTYPE_short, 0, slot_num);
                }

                c->pressTimeBegin 	= 0;
                c->unpressTimeBegin 	= xTaskGetTickCount();
            }

            *prev_state = button_state;
        }
        else if (c->pressTimeBegin && !c->longPressSignaled) // Если состояние не изменилось и активно нажатие
        {
            now = xTaskGetTickCount();

            if (pdTICKS_TO_MS(now - c->pressTimeBegin) >= c->longPressTime) // если длительность достигнута
            {
                _buttonStateChanged(c, BSTYPE_long, 1, slot_num);

                c->longPressSignaled = true;
            }
        }
    }
    else if (button_state != *prev_state) // Если состояние кнопки изменилось
    {
        *prev_state = button_state;

        _buttonStateChanged(c, BSTYPE_short, button_state, slot_num);
    }
}
