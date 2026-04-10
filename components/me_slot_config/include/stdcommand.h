// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************


#ifndef __STDCOMMAND_H__
#define __STDCOMMAND_H__

#include <stdint.h>
#include <executor.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define STDCOMMAN_MAX_KEYWORDS  10
#define STDCOMMAN_MAX_PARAMS    10

#define PARAMSIZE               uint32_t

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

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

/**
 * @brief Регистрирует команду для обработки слотом.
 *
 * @param a   (PSTDCOMMANDS) Указатель на структуру команд слота (&c->cmds)
 * @param b   (int) Идентификатор команды (значение enum, например CMD_start)
 * @param c   (const char*) Ключевое слово команды ("start", "stop", ...) или NULL для безымянной команды
 * @param ... (PARAMT) Типы параметров: PARAMT_none, PARAMT_int, PARAMT_float, PARAMT_string, PARAMT_enum
 *
 * Примеры:
 * @code
 *   stdcommand_register(&cmds, CMD_start, "start", PARAMT_none);
 *   stdcommand_register(&cmds, CMD_set,   "set",   PARAMT_int);
 *   stdcommand_register(&cmds, CMD_val,   NULL,    PARAMT_int);   // безымянная: принимает просто число
 * @endcode
 */
#define stdcommand_register(a,b,c,...)                                          \
do {                                                                            \
    PARAMT parameters[] = {__VA_ARGS__};                                        \
    _stdcommand_register(a,b,c, sizeof(parameters)/sizeof(PARAMT), __VA_ARGS__); \
} while(0)

/**
 * @brief Регистрирует команду с enum-параметром (выбор из списка строк).
 *
 * @param a   (PSTDCOMMANDS) Указатель на структуру команд слота (&c->cmds)
 * @param b   (int) Идентификатор команды (значение enum)
 * @param c   (const char*) Ключевое слово команды ("mode", "type", ...)
 * @param ... (char*) Допустимые строковые значения параметра. Индекс совпавшего значения → params.enumResult
 *
 * Пример:
 * @code
 *   stdcommand_register_enum(&cmds, CMD_mode, "mode", "fast", "slow", "auto");
 *   // команда "mode:fast" → params.enumResult = 0
 *   // команда "mode:slow" → params.enumResult = 1
 * @endcode
 */
#define stdcommand_register_enum(a,b,c,...)                                     \
do {                                                                            \
    char * parameters[] = {__VA_ARGS__};                                        \
    _stdcommand_register_enum(a,b,c, sizeof(parameters)/sizeof(char*), __VA_ARGS__); \
} while(0)

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

/**
 * @brief Инициализирует структуру команд для слота.
 * @param cmd      Указатель на структуру STDCOMMANDS (обычно &c->cmds)
 * @param slot_num Номер слота (0-9)
 */
void                stdcommand_init             (PSTDCOMMANDS       cmd,
                                                 int                slot_num);


int                 _stdcommand_register        (PSTDCOMMANDS       cmd,
                                                 int                id,
                                                 const char *       keyword,
                                                 int                count,
                                                 ...);

int                 _stdcommand_register_enum   (PSTDCOMMANDS       cmd,
                                                 int                id,
                                                 const char *       keyword,
                                                 int                count,
                                                 ...);


/**
 * @brief Получает следующую команду из очереди слота.
 * @param cmd    Указатель на структуру STDCOMMANDS
 * @param params Указатель на структуру параметров (заполняется при получении)
 * @param TO     Таймаут ожидания в тиках FreeRTOS (0 = без ожидания, portMAX_DELAY = ждать бесконечно)
 * @return Идентификатор команды (значение enum) или -1 если команда не получена/не распознана
 */
int                 stdcommand_receive          (PSTDCOMMANDS       cmd,
                                                 PSTDCOMMAND_PARAMS params,
                                                 int                TO);


#endif // #define __STDCOMMAND_H__