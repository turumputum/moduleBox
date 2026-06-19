// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdreport.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"

#include "reporter.h"
#include "stateConfig.h"
#include <axstring.h>
#include <string.h>

extern configuration me_config;
extern stateStruct me_state;

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define PREFIX              (*p->topic ? p->topic : "")

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------


static  xSemaphoreHandle    modSemaphore		= NULL; 

static  int                 numberOfReport      = 0;

static  PSTDREPORT          reports             [ MAX_NUM_OF_STDREPORTS ];


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

void stdreport_initialize()
{
    modSemaphore = xSemaphoreCreateMutex();
}

int stdreport_register(RPTT               output_type,
                       int                slot_num, 
                       const char *       unit,
                       const char *       defaultTopic,
                        ...)
{
    int             result          = -1;
    int             size;
    va_list         list;

    va_start(list, defaultTopic);

    if (numberOfReport < MAX_NUM_OF_STDREPORTS)
    {
        if (xSemaphoreTake(modSemaphore, portMAX_DELAY) == pdTRUE)
        {
            switch (output_type)
            {
                case RPTT_ratio:
                    size = sizeof(STDREPORTRATIO);
                    break;
            
                default:
                    size = sizeof(STDREPORT);
                    break;
            }

            PSTDREPORT p = calloc(1, size);

            if (p != NULL)
            {
                p->slot_num     = slot_num;
                p->outType      = output_type;
                *p->topic       = 0;

                /* defaultTopic ожидается уже с направлением, например "event/press".
                   Хранится как "/event/press:" — reporter префиксирует базу слота. */
                if (defaultTopic && *defaultTopic)
                    snprintf(p->topic, sizeof(p->topic) - 1, "/%s:", defaultTopic);

                switch (p->outType)
                {
                    case RPTT_ratio:
                        {
                            PSTDREPORTRATIO r = (PSTDREPORTRATIO)p;

                            r->min = va_arg(list, int);
                            r->max = va_arg(list, int);
                        }
                        break;
                
                    default:
                        break;
                }

                result = numberOfReport++;

                reports[result] = p;
            }

            xSemaphoreGive(modSemaphore);
        }
    }

    va_end(list);

    return result;
}
static void _render_ratio(PSTDREPORTRATIO r, char * tmpString, float f_res)
{
    if (f_res > r->max) f_res    = r->max;
    if (f_res < r->min) f_res    = r->min;

    f_res                       -= r->min;
    f_res                        = (float)f_res / (r->max - r->min);

    sprintf(tmpString,"%s%f", *r->rpt.topic ? r->rpt.topic : "", f_res);
}
void stdreport_i(int                reportRegId,
                 int                value)
{
    PSTDREPORT      p           = reports[reportRegId];
	char            tmpString   [255];

    if ((reportRegId < MAX_NUM_OF_STDREPORTS) && p)
    {
        switch (p->outType)
        {
            case RPTT_ratio:
                _render_ratio((PSTDREPORTRATIO)p, tmpString, (float)value);
                break;

            case RPTT_float:
                sprintf(tmpString, "%s%d.0", PREFIX, value);
                break;

            default:
                sprintf(tmpString, "%s%d", PREFIX, value);
                break;
        }

        //printf("@@@@@@@@@@@@@@@@@22 report: '%s' slot %d\n", tmpString, p->slot_num);

        report(tmpString, p->slot_num);
    }
}
void stdreport_f(int                reportRegId,
                 float              value)
{
    PSTDREPORT      p           = reports[reportRegId];
	char            tmpString   [255];

    if ((reportRegId < MAX_NUM_OF_STDREPORTS) && p)
    {
        switch (p->outType)
        {
            case RPTT_ratio:
                _render_ratio((PSTDREPORTRATIO)p, tmpString, value);
                break;

            case RPTT_int:
                sprintf(tmpString, "%s%d", PREFIX, (int)value);
                break;

            default:
                sprintf(tmpString, "%s%f", PREFIX, value);
                break;
        }

        report(tmpString, p->slot_num);
    }
}
void stdreport_s(int                reportRegId,
                 char *             value)
{
    PSTDREPORT      p           = reports[reportRegId];
	char            tmpString   [255];

    if ((reportRegId < MAX_NUM_OF_STDREPORTS) && p)
    {
        switch (p->outType)
        {
            case RPTT_ratio:
                _render_ratio((PSTDREPORTRATIO)p, tmpString, atof(value));
                break;

            default:
                sprintf(tmpString, "%s%s", PREFIX, value);
                break;
        }

        report(tmpString, p->slot_num);
    }
}

void stdreport_enable(int slot_num, int value)
{
    if (slot_num < 0 || slot_num >= NUM_OF_SLOTS) return;
    if (!me_state.trigger_topic_list[slot_num]) return;

    /* Обычная публикация события (retain в системе не используется).
       report() сам префиксует базу слота, когда строка начинается с '/'. */
    char msg[24];
    snprintf(msg, sizeof(msg), "/event/enable:%d", value);
    report(msg, slot_num);
}
