// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h> 
#include <stdbool.h>  
#include <mbdebug.h>
#include <stateConfig.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define DEF_LOG_FILE_BASE_NAME  "/sdcard/log"

#define LOG_BUFF_SIZE       2048

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static  SemaphoreHandle_t   logMutex    = NULL;
static  char *              logBuff     = NULL;
static  FILE *              logFile     = NULL;

extern configuration        me_config;

static const char * PRIONAMES_SHORT [] = 
{
    " ",        /*!< No log output */
    "E",        /*!< Critical errors, software module can not recover on its own */
    "W",        /*!< Error conditions from which recovery measures have been taken */
    "I",        /*!< Information messages which describe normal flow of events */
    "D",        /*!< Extra information which is not necessary for normal use (values, pointers, sizes, etc). */
    "V"         /*!< Bigger */
};


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

void mblog_init()
{
    logMutex    = xSemaphoreCreateMutex();
    logBuff     = malloc(LOG_BUFF_SIZE + 2);
}

bool file_exists (char *filename) 
{
  struct stat   buffer;   
  return (stat (filename, &buffer) == 0);
}

static void _shiftLogs()
{
    char                 srcFname    [ 64 ];
    char                 tgtFname    [ 64 ];

    int startNum = me_config.logChapters - 1;

    for (int i = startNum; i != 0; i--)
    {
        sprintf(tgtFname, "%s.%d.txt", DEF_LOG_FILE_BASE_NAME, i);

        if ((i == startNum) && file_exists(tgtFname))
        {
    //        printf("@@@ removing %s\n", tgtFname);
            remove(tgtFname);
        }

        if (i == 1)
            sprintf(srcFname, "%s.txt", DEF_LOG_FILE_BASE_NAME);
        else
            sprintf(srcFname, "%s.%d.txt", DEF_LOG_FILE_BASE_NAME, i - 1);        

//        printf("@@@ moving %s -> %s\n", srcFname, tgtFname);
        if (file_exists(srcFname))
        {
            rename(srcFname, tgtFname);
        }
    }
}
void mblog(esp_log_level_t level, const char *msg, ...)
{
    va_list             st_va_list;
    size_t              sz;

    if (logMutex && me_config.logLevel && (level <= me_config.logLevel))
    {
        if (xSemaphoreTake(logMutex, portMAX_DELAY) == pdTRUE)
        {
            sz = snprintf(logBuff, LOG_BUFF_SIZE, "(%d) ", (int)pdTICKS_TO_MS(xTaskGetTickCount()));

            va_start(st_va_list, msg);
            sz += vsnprintf(logBuff + sz, LOG_BUFF_SIZE - sz - 1, msg, st_va_list);
            va_end(st_va_list);

            *(logBuff + sz) = 0;

            printf("\x1b[33m=%s= %s\x1b[0m\n", PRIONAMES_SHORT[level], logBuff);

            if ((logFile = fopen(DEF_LOG_FILE_BASE_NAME ".txt", "a")) != NULL)
            {
                fprintf(logFile, "%s\n", logBuff);

                sz = ftell(logFile);

                fclose(logFile);

                if (sz > (me_config.logMaxSize / me_config.logChapters))
                {
                    _shiftLogs();
                }
            }

            xSemaphoreGive(logMutex);
        }
    }
}
