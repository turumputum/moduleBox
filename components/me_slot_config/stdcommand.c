// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stateConfig.h>
#include <string.h>
#include "esp_log.h"

#include <stdcommand.h>

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static const char *TAG = "MAIN";

extern stateStruct me_state;

const char * paramt[] = {
"none",
"int",
"float",
"string",
"enum"
};

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

void stdcommand_init(PSTDCOMMANDS       cmd,
                     int                slot_num)
{
    memset(cmd, 0, sizeof(STDCOMMANDS));

    cmd->slot_num = slot_num;
}
int _stdcommand_register(PSTDCOMMANDS       cmd,
                         int                id,
                         const char *       keyword,
                         int                count,
                        ...)
{
    int             result  = 1;
    int             i       = cmd->count;
    va_list         list;

    va_start(list, count);

    cmd->keywords[i].keyword    = keyword;
    cmd->keywords[i].id         = id;
    cmd->keywords[i].type       = PARAMT_none;
    cmd->keywords[i].count      = count;

    //printf("add keyword: %s\n", keyword);

    for (int j = 0; (j < count) && (result > 0); j++)
    {
        cmd->keywords[i].p[j] = va_arg(list, int);

        if (cmd->keywords[i].p[j] == PARAMT_enum)
        {
            ESP_LOGD(TAG, "stdcommand: %s parameter unacceptable\n", paramt[(int)cmd->keywords[i].p[j]]);
            result = -1;
        }
    }

    cmd->count++;
   
   va_end(list);

   return result;
}               
int _stdcommand_register_enum(PSTDCOMMANDS       cmd,
                              int                id,
                              const char *       keyword,
                              int                count,
                              ...)
{
    int             result  = 1;
    int             i       = cmd->count;
    va_list         list;

    va_start(list, count);

    cmd->keywords[i].keyword    = keyword;
    cmd->keywords[i].id         = id;
    cmd->keywords[i].type       = PARAMT_enum;
    cmd->keywords[i].count      = count;

    //printf("add keyword: %s\n", keyword);

    for (int j = 0; (j < count) && (result > 0); j++)
    {
        if ((cmd->keywords[i].p[j] = strdup(va_arg(list, char *))) == NULL)
        {
            ESP_LOGD(TAG, "stdcommand: %s parameter unacceptable\n", paramt[(int)cmd->keywords[i].p[j]]);
            result = -1;
        }
    }

    cmd->count++;
   
    va_end(list);

    return result;
}               
static PARAMT _checkType(const char * value)
{
    PARAMT      t           = PARAMT_none;
    int         nondigit    = 0;
    int         sign        = 0;
    int         dot         = 0;
    const char *  on          = value;

    if (*on)
    {
        t = PARAMT_string;

        if ((*on == '+') || (*on == '-'))
        {
            sign++;
            on++;
        }

        for (; *on; on++)
        {
            if (*on == '.')
            {
                dot++;
            }
            else if (((*on < '0') || (*on > '9')))
            {
                nondigit++;
            }
        }

        if ((nondigit == 0) && (sign <= 1))
        {
            if (dot == 0)
            {
                t = PARAMT_int;

            }
            else if (dot == 1)
            {
                t = PARAMT_float;
            }
        }
    }

    return t;
}
static char * _skip_spaces(char * value)
{
    unsigned char * on = (unsigned char *)value;

    while (*on && (*on <= ' '))
    {
        on++;
    }

    return (char*)on;
}
static int _add_param(PSTDCOMMAND_PARAMS params,
                      char * value)
{
    int         result  = 1;
    int         i       = params->count;

    value = _skip_spaces(value);

    params->p[i].type = _checkType(value);

    switch (params->p[i].type)
    {
        case PARAMT_int:
            params->p[i].data = (int)atoi(value);
            break;
        
        case PARAMT_float:
            params->p[i].data = (float)atof(value);
            break;
        
        default:
            params->p[i].data = (PARAMSIZE)value;
            break;
    }

    params->count++;

    return result;    
}
int stdcommand_receive(PSTDCOMMANDS       cmd,
                       PSTDCOMMAND_PARAMS params,
                       int                TO)
{
    int                 result          = -1;
    char *              keyword;
    char *              delim;
    char *              value           = NULL;

    if (xQueueReceive(me_state.command_queue[cmd->slot_num], &cmd->msg, TO) == pdPASS)
    {
        // printf("MSG HEX: ");
        // for (int i = 0; i < 50; i++)
        // {
        //     printf("%x ", cmd->msg.str[i]);
        // }
        // printf("\n");

        params->count = 0;

        if ((keyword = strchr(cmd->msg.str, ':')) != NULL)
        {
            ++keyword;

            if ((delim = strchr(keyword, ':')) != NULL)
            {
                *delim = 0;
                value = ++delim;
                char * space;

                value = _skip_spaces(value);

                while ((space = strchr(value, ' ')) != NULL)
                {
                    *space = 0;
                    _add_param(params, value);

                    value = _skip_spaces(++space);
                }

                _add_param(params, value);
            }
            else
            {
                _add_param(params, keyword);
                keyword = NULL;
            }
            
            //printf("@@@@@ COMMAND: keyword = '%s'\n", keyword ? keyword : "<<nope>>");
            //printf("params = %d\n", params->count);
            // for (int i = 0; i < params->count; i++)
            // {
            //     printf("param %d, type: %s, value: ", i, paramt[params->p[i].type]);
            //     switch (params->p[i].type)
            //     {
            //         case PARAMT_int:
            //             printf("%d", (int)params->p[i].data);
            //             break;
            //         case PARAMT_float:
            //             printf("%f", (float)params->p[i].data);
            //             break;
            //         default:
            //             printf("'%s'", (char*)params->p[i].data);
            //             break;
            //     }
            //     printf("\n");
            // }

            if (keyword)
            {
                for (int i = 0; (i < cmd->count) && (-1 == result); i++)
                {
                    if (!strcasecmp(cmd->keywords[i].keyword, keyword))
                    {
                        if (  (cmd->keywords[i].type == PARAMT_enum)    && 
                              (params->count == 1)                      && 
                              (params->p[0].type == PARAMT_string)      )
                        {
                            params->enumResult = -1;

                            for (int j = 0; (j < cmd->keywords[i].count) && (-1 == params->enumResult); j++)
                            {
                                if (!strcasecmp(cmd->keywords[i].p[j], (char*)params->p[0].data))
                                {
                                    params->enumResult = j;
                                }
                            }

                            if (params->enumResult >= 0)
                            {
                                result = cmd->keywords[i].id;
                            }
                        }
                        else if (cmd->keywords[i].count == params->count)
                        {
                            result = cmd->keywords[i].id;

                            for (int j = 0; (j < cmd->keywords[i].count) && (-1 != result); j++)
                            {
                                if (cmd->keywords[i].p[j] != params->p[j].type)
                                {
                                    //printf("stage 4: WRONG TYPE!!!\n");
                                    result = -1;
                                }
                            }
                        }
                    }
                }
            }
            else
                result = 0;
        }
    }

    return result;
}
