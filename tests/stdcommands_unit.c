#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define MAX_STRING_LENGTH  255
#define pdPASS  1
#define PARAMSIZE           uint64_t

#define ESP_LOGD    fprintf
#define TAG         stdout

#define MODE_RUN    5

typedef struct{
	char str[MAX_STRING_LENGTH];
	int  slot_num;
} command_message_t;

struct 
{
    char command_queue[ 100 ];
} me_state;

static int queueIndex = 0;

static char * queueStrings [] = 
{
    "moduleBox/smartLed_0:setRGB: 1  2  3",
    "moduleBox/smartLed_0:stop",
    "moduleBox/smartLed_0:100500",
    "moduleBox/smartLed_0:stopp",
    "moduleBox/smartLed_0:setMode:run",
    "moduleBox/smartLed_0:setIncrement:1",

    "moduleBox/smartLed_0:setRGB: 1  2  3 4",
    "moduleBox/smartLed_0:setRGB 1  2  3 4",
    "moduleBox/player_0:play:1",

};


int xQueueReceive(char b, command_message_t * msg, int TO)
{
    int result = 0;

    if (queueIndex < (sizeof(queueStrings) / sizeof(char*)))
    {
        strcpy(msg->str, queueStrings[queueIndex]);

        queueIndex++;

        result = pdPASS;
    }
    
    return result;
}

typedef struct RgbColor{
//typedef struct RgbColor{
	unsigned char r;
	unsigned char g;
	unsigned char b;
} RgbColor;


#define STDCOMMAN_MAX_KEYWORDS  10
#define STDCOMMAN_MAX_PARAMS    10

// ========================================= BEGIN 1 =========================================

typedef enum
{
    PARAMT_none = 0,
    PARAMT_int,
    PARAMT_float,
    PARAMT_string,
    PARAMT_enum
} PARAMT;


typedef struct __tag_STDCOMMANDPARAM
{
    PARAMT                  type;
    const char *            p;
    float                   f;
    int32_t                 i;
} STDCOMMANDPARAM;

typedef struct __tag_STDCOMMAND_PARAMS
{
    int                     skipTypeChecking;
    int                     count;
    int                     enumResult;
    STDCOMMANDPARAM         p                   [ STDCOMMAN_MAX_PARAMS ];
} STDCOMMAND_PARAMS, * PSTDCOMMAND_PARAMS;

typedef struct __tag_STDCOMMAND_KEYWORD
{
    const char *            keyword;
    int                     id;
    PARAMT                  type;
    int                     count;
    char *                  p                   [ STDCOMMAN_MAX_PARAMS ];
} STDCOMMAND_KEYWORD, * PSTDCOMMAND_KEYWORD;

typedef struct __tag_STDCOMMANDS
{
    int                     slot_num;
    command_message_t       msg;
    int                     count;
    STDCOMMAND_KEYWORD      keywords            [ STDCOMMAN_MAX_KEYWORDS ];
} STDCOMMANDS, * PSTDCOMMANDS;

#define stdcommand_register(a,b,c,...)                                          \
do {                                                                            \
    PARAMT parameters[] = {__VA_ARGS__};                                        \
    _stdcommand_register(a,b,c, sizeof(parameters)/sizeof(PARAMT), __VA_ARGS__); \
} while(0)

#define stdcommand_register_enum(a,b,c,...)                                     \
do {                                                                            \
    char * parameters[] = {__VA_ARGS__};                                        \
    _stdcommand_register_enum(a,b,c, sizeof(parameters)/sizeof(char*), __VA_ARGS__); \
} while(0)

// ========================================= END 1 =========================================

// ========================================= BEGIN 2 =========================================

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
    char                tmp             [ 128 ];

    if (xQueueReceive(me_state.command_queue[cmd->slot_num], &cmd->msg, TO) == pdPASS)
    {
        if ((keyword = strchr(cmd->msg.str, ':')) != NULL)
        {
            params->count = 0;

            ++keyword;

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

// ========================================= END 2 =========================================


typedef enum
{
    MYCMD_defaultInt = 0,
    MYCMD_defaultStr = 1,
    MYCMD_setRGB,
    MYCMD_setMode,
    MYCMD_setIncrement,
    MYCMD_stop,
	MYCMD_play,
} MYCMD;

typedef struct __tag_SMARTLEDCONFIG
{
    uint16_t                num_of_led;
    uint8_t                 inverse;
    uint8_t                 state;
    int16_t                 increment;
    int16_t                 maxBright;
    int16_t                 minBright;
    uint16_t                refreshPeriod;
    uint8_t                 ledMode;
    RgbColor                targetRGB;
    int                     floatReport;
    int                     numOfPos;
    uint16_t                effectLen;
    int                     dir;
    int16_t                 offset;
    STDCOMMANDS             cmds;
} SMARTLEDCONFIG, * PSMARTLEDCONFIG; 


int xPortGetFreeHeapSize()
{
    return 1000;
}
// init_runEffect(&led_strip_pixels, c->num_of_led, c->minBright, c->maxBright, &c->targetRGB);
int init_runEffect(char * pixels, int num_of_led, int minBright, int maxBright, RgbColor * targetRGB)
{
    return 0;
}
int main(void)
{
    PSMARTLEDCONFIG c = calloc(1, sizeof(SMARTLEDCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };
    int                     slot_num = 0;
    int                     targetBright = 255;
    int                     currentBright = -1;
    char                    led_strip_pixels [ 24 ];

    printf("\n\n\n------------------------\n\n\n");

    /* Числовое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&c->cmds, MYCMD_defaultInt, NULL, PARAMT_int);


    /* Строковое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&c->cmds, MYCMD_defaultStr, NULL, PARAMT_string);

    /* Установить новый целевой цвет. 
       Цвет задаётся десятичными значениями R G B через пробел
    */
    stdcommand_register(&c->cmds, MYCMD_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Установить новый режим анимации цветов
    */
    stdcommand_register_enum(&c->cmds, MYCMD_setMode, "setMode", "default", "flash", "glitch", "swiper", "rainbow", "run");

    /* Установить новое значение приращения
    */
    stdcommand_register(&c->cmds, MYCMD_setIncrement, "setIncrement", PARAMT_int);


    /* Stop
    */
    stdcommand_register(&c->cmds, MYCMD_stop, "stop", PARAMT_none);

    /* Проиграть трек
       Опционально - номер трека
    */
    stdcommand_register(&c->cmds, MYCMD_play, "play", PARAMT_string);


    //PARAMT result;
    // result = _checkType("+00021234139871038471");
    // printf("result = %s\n", paramt[result]);

    int id;

    params.skipTypeChecking = 1;

    while ((id = stdcommand_receive(&c->cmds, &params, 0)) != -1)
    {
        switch (id)
        {
            case -1:
                break;

            case MYCMD_defaultInt:
                printf("got default command with INT parameter '%d'\n", params.p[0].i);
                break;

            case MYCMD_defaultStr:
                printf("got default command with STR parameter '%s'\n", params.p[0].p);
                break;

            case MYCMD_stop:
                printf("got command STOP\n");
                break;

            case MYCMD_play:
                printf("got command PLAY\n");
                break;

            case MYCMD_setRGB:
                c->targetRGB.r = params.p[0].i;
                c->targetRGB.g = params.p[1].i;
                c->targetRGB.b = params.p[2].i;

                ESP_LOGD(TAG, "Slot:%d target RGB: %d %d %d\n", slot_num, c->targetRGB.r, c->targetRGB.g, c->targetRGB.b); 
                break;

            case MYCMD_setMode:
                c->ledMode = params.enumResult;
                
                if(c->ledMode==MODE_RUN)
                {
                    ESP_LOGD(TAG, "Slot:%d init run effect\n", slot_num); 

                    init_runEffect(&led_strip_pixels[0], c->num_of_led, c->minBright, c->maxBright, &c->targetRGB);
                }
                break;

            case MYCMD_setIncrement:
                c->increment = params.p[0].i;
                ESP_LOGD(TAG, "Set fade increment:%d\n", c->increment);
                break;

            default:
                ESP_LOGD(TAG, "@@@@@@@@@@@@@@@@@@ GOT!!!!\n");
                //printf("@@@@@@@@@@@@@@@@@@ GOT!!!!\n");
                break;                
        }

    }
    printf("\n\n\n------------------------\n\n\n");

	return 1;
}

