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
#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "MAIN";

extern stateStruct me_state;

const char * paramt[] = {
"none",
"int",
"float",
"string",
"enum"
};

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

    for (int j = 0; (j < count) && (result > 0); j++)
    {
        cmd->keywords[i].p[j] = (char*)(va_arg(list, int));

        if (cmd->keywords[i].p[j] == (char*)PARAMT_enum)
        {
            ESP_LOGE(TAG, "stdcommand: %s parameter unacceptable\n", paramt[(int)cmd->keywords[i].p[j]]);
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
static const char * _skip_spaces(const char * value)
{
    unsigned char * on = (unsigned char *)value;

    while (*on && (*on <= ' '))
    {
        on++;
    }

    return (char*)on;
}
static int _add_param(PSTDCOMMAND_PARAMS params,
                      const char * value)
{
    int         result  = 1;
    int         i       = params->count;

    value = _skip_spaces(value);
    params->p[i].type   = _checkType(value);
    params->p[i].p      = value;

    switch (params->p[i].type)
    {
        case PARAMT_int:
            params->p[i].i = atoi(value);
            break;
        
        case PARAMT_float:
            params->p[i].f = (float)atof(value);
            break;
        
        default:
            break;
    }

    params->count++;

    return result;    
}
static int _checkTypesCompability(PARAMT got, PARAMT desired)
{
    int             result = 0;

    switch (desired)
    {
        case PARAMT_float:
            result = ( (PARAMT_float == got)    || 
                       (PARAMT_int   == got)    );
            break;
    
        case PARAMT_int:
            result = (PARAMT_int == got);
            break;

        case PARAMT_enum:
        case PARAMT_string:
            result = 1;
            break;

        default:
            break;
    }

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
    char *              space;
    int                 len;
    char                tmp             [ 128 ];


    if (xQueueReceive(me_state.command_queue[cmd->slot_num], &cmd->msg, TO) == pdPASS)
    {
        len = strlen(me_state.action_topic_list[cmd->slot_num]);

        //ESP_LOGD(TAG, "Input cmd:%s", cmd->msg.str);

        if (strlen(cmd->msg.str) > len)
        {
            keyword = cmd->msg.str + len + 1;

            params->count = 0;

            // Separate keyword and parameters, if any
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

            keyword = (char*)_skip_spaces(keyword);            

            // Searching for keyword
            for (int i = 0; (i < cmd->count) && (-1 == result); i++)
            {
                if (  cmd->keywords[i].keyword                          &&
                      !strcasecmp(cmd->keywords[i].keyword, keyword)    )
                {
                    if (params->count)
                    {
                        if ( (cmd->keywords[i].type == PARAMT_enum) &&
                             (params->count == 1)                   &&
                             (params->p[0].type == PARAMT_string)   )
                        {
                            params->enumResult = -1;
                    
                            for (int j = 0; (j < cmd->keywords[i].count) && (-1 == params->enumResult); j++)
                            {
                                if (!strcasecmp(cmd->keywords[i].p[j], params->p[0].p))
                                {
                                    params->enumResult = j;
                                }
                            }
                                
                            if (params->enumResult >= 0)
                            {   
                                result = cmd->keywords[i].id;
                            }
                        }
                        else if (params->skipTypeChecking)
                        {
                            result = i;
                        }
                        else  if (cmd->keywords[i].count == params->count)
                        {
                            int allIsOk = 1;
                            for (int j = 0; allIsOk && (j < params->count); j++)
                            {
                                if (!_checkTypesCompability(params->p[j].type, (PARAMT)cmd->keywords[i].p[j]))
                                {
                                    allIsOk = 0;
                                }
                            }

                            if (allIsOk)
                            {
                                result = i;
                            }
                        }
                    }
                    else if ((1 == cmd->keywords[i].count) && (PARAMT_none == (PARAMT)cmd->keywords[i].p[0]))
                    {
                        // If command with no parameters

                        params->p[0].type   = PARAMT_none;
                        params->count       = 1;
                        result              = i;
                    }
                }
            }

            // If no keywork found, we should consider parameters as value without keyword
            if (-1 == result)
            {
                int type = _checkType(keyword);

                // Searching for registered commands without keyword
                for (int i = 0; (i < cmd->count) && (-1 == result); i++)
                {
                    if (!cmd->keywords[i].keyword)
                    {
                        if (_checkTypesCompability(type, (PARAMT)cmd->keywords[i].p[0]))
                        {
                            params->count = 0;
                            _add_param(params, keyword);

                            result = i; 
                        }
                    }
                }
            }
        }
    }

    return result;
}
