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
    int32_t                 data;
} STDCOMMANDPARAM;

typedef struct __tag_STDCOMMAND_PARAMS
{
    int                     count;
    bool                    nonstricktTypes;
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

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

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


int                 stdcommand_receive          (PSTDCOMMANDS       cmd,
                                                 PSTDCOMMAND_PARAMS params,
                                                 int                TO);


#endif // #define __STDCOMMAND_H__