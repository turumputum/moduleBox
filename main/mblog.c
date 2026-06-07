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
#include <time.h>
#include <mbdebug.h>
#include <stateConfig.h>
#include <esp_vfs_fat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <arsenal.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define DEF_LOG_FILE_BASE_NAME  "/sdcard/log"

/* Кольцо из не более чем LOG_FILE_COUNT файлов log.0.txt .. log.N.txt.
   Пишем (append) в текущий; при достижении лимита переходим к следующему
   по кругу и перезаписываем его. Размер файла <= LOG_MAX_FILE_SIZE, но
   меньше, если на диске мало места (бюджет = доступно / LOG_FILE_COUNT). */
#define LOG_FILE_COUNT      3
#define LOG_MAX_FILE_SIZE   (1024 * 1024)
#define LOG_MIN_FILE_SIZE   (8 * 1024)

//#define LOG_BUFF_SIZE       2048

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static  SemaphoreHandle_t   logMutex    = NULL;
static  FILE *              logFile     = NULL;

static  int                 s_curIdx    = 0;        /* индекс текущего файла кольца */
static  long                s_capPerFile = LOG_MAX_FILE_SIZE; /* лимит на файл, байт */
static  bool                s_logReady  = false;    /* выполнена ли инициализация кольца */

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

char * _getAvailableBuff(int * avail)
{
    char * result   = nil;
    *avail          = 2048;

    do 
    {
        result = malloc(*avail + 2);
    }
    while (!result && ((*avail >> 1) > 100));
   
    return result;
}
void mblog_init()
{
    logMutex    = xSemaphoreCreateMutex();
}

static void _logName(char *dst, size_t n, int idx)
{
    snprintf(dst, n, "%s.%d.txt", DEF_LOG_FILE_BASE_NAME, idx);
}

static long _fileSize(const char *fn)
{
    struct stat st;
    return (stat(fn, &st) == 0) ? (long)st.st_size : -1;
}

/* Однократная инициализация кольца логов (под logMutex, при первом mblog).
   cleanLogOnStart - стереть все файлы. Иначе - продолжить дозапись в самый
   свежий файл по mtime (на старте новых записей ещё нет, поэтому max mtime =
   последний писанный в прошлой сессии; RTC для этого не нужен). */
static void _logSetup(void)
{
    char fn[64];

    /* старая схема писала в безномерной log.txt - в кольце он не используется,
       удаляем один раз чтобы не занимал место */
    remove(DEF_LOG_FILE_BASE_NAME ".txt");

    if (me_config.cleanLogOnStart)
    {
        for (int i = 0; i < LOG_FILE_COUNT; i++) { _logName(fn, sizeof(fn), i); remove(fn); }
        s_curIdx = 0;
    }
    else
    {
        time_t newest = 0;
        int    newestIdx = -1;
        for (int i = 0; i < LOG_FILE_COUNT; i++)
        {
            _logName(fn, sizeof(fn), i);
            struct stat st;
            if (stat(fn, &st) == 0 && (newestIdx < 0 || st.st_mtime >= newest))
            {
                newest = st.st_mtime;
                newestIdx = i;
            }
        }
        s_curIdx = (newestIdx >= 0) ? newestIdx : 0;
    }

    /* лимит на файл: min(1MB, доступно/3); доступно = свободно + уже занятое логами */
    long cap = LOG_MAX_FILE_SIZE;
    long existing = 0;
    for (int i = 0; i < LOG_FILE_COUNT; i++) { _logName(fn, sizeof(fn), i); long s = _fileSize(fn); if (s > 0) existing += s; }

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK)
    {
        long byBudget = ((long)freeb + existing) / LOG_FILE_COUNT;
        if (byBudget < cap) cap = byBudget;
    }
    if (cap < LOG_MIN_FILE_SIZE) cap = LOG_MIN_FILE_SIZE;
    s_capPerFile = cap;

    s_logReady = true;
}
void mblog(esp_log_level_t level, const char *msg, ...)
{
    va_list             st_va_list;
    size_t              sz;
    int                 avail;
    char *              logBuff;

    if (logMutex && me_config.logLevel && (level <= me_config.logLevel))
    {
        if (xSemaphoreTake(logMutex, portMAX_DELAY) == pdTRUE)
        {
            if ((logBuff = _getAvailableBuff(&avail)) != nil)
            {
                sz = snprintf(logBuff, avail, "(%d) ", (int)pdTICKS_TO_MS(xTaskGetTickCount()));

                va_start(st_va_list, msg);
                sz += vsnprintf(logBuff + sz, avail - sz - 1, msg, st_va_list);
                va_end(st_va_list);

                *(logBuff + sz) = 0;

                printf("\x1b[33m=%s= %s\x1b[0m\n", PRIONAMES_SHORT[level], logBuff);

                if (!s_logReady) _logSetup();

                char fn[64];
                _logName(fn, sizeof(fn), s_curIdx);

                if ((logFile = fopen(fn, "a")) != NULL)
                {
                    fprintf(logFile, "%s\n", logBuff);

                    long fsz = ftell(logFile);

                    fclose(logFile);

                    /* достигли лимита - переходим к следующему файлу по кругу
                       и перезаписываем его (стираем перед первой записью) */
                    if (fsz >= s_capPerFile)
                    {
                        s_curIdx = (s_curIdx + 1) % LOG_FILE_COUNT;
                        _logName(fn, sizeof(fn), s_curIdx);
                        remove(fn);
                    }
                }

                free(logBuff);
            }

            xSemaphoreGive(logMutex);
        }
    }
}
