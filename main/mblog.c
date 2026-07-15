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
#include <unistd.h>
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

/* Политика сброса на карту.
   Раньше каждая строка делала fopen(a)+fprintf+fclose: fclose обновляет размер
   и mtime в каталоге, то есть КАЖДАЯ строка лога писала метаданные тома. У FAT
   нет журнала - обрыв питания в этот момент бьёт по таблице FAT и каталогу, а
   это повреждение не привязано к файлу и способно утащить за собой config.ini.
   Теперь файл держится открытым, строки копятся в stdio-буфере, а метаданные
   трогаются только на сбросе: по объёму, по времени, и всегда на ошибке -
   причина падения обязана лечь на карту до ребута. */
#define LOG_FLUSH_BYTES        2048
#define LOG_FLUSH_INTERVAL_MS  5000
#define LOG_LOCK_TIMEOUT_MS    500     /* ребут не должен зависнуть на мьютексе */

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static  SemaphoreHandle_t   logMutex    = NULL;
static  FILE *              logFile     = NULL;   /* держим открытым между записями */

static  int                 s_curIdx    = 0;        /* индекс текущего файла кольца */
static  long                s_capPerFile = LOG_MAX_FILE_SIZE; /* лимит на файл, байт */
static  bool                s_logReady  = false;    /* выполнена ли инициализация кольца */

static  long                s_bytesSinceFlush = 0;
static  TickType_t          s_lastFlushTick   = 0;

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
/* Открыть текущий файл кольца. Вызывать под logMutex.
   Файл остаётся открытым между записями - именно это убирает обновление
   каталога на каждую строку. */
static void _logOpenLocked(void)
{
    char fn[64];
    _logName(fn, sizeof(fn), s_curIdx);

    logFile = fopen(fn, "a");
    if (logFile)
    {
        /* крупный stdio-буфер: строки копятся в RAM и уходят на карту пачкой */
        setvbuf(logFile, NULL, _IOFBF, LOG_FLUSH_BYTES);
        s_bytesSinceFlush = 0;
        s_lastFlushTick   = xTaskGetTickCount();
    }
}

/* Протолкнуть данные до карты. Вызывать под logMutex.
   fflush: stdio -> FATFS, fsync: FATFS -> карта + обновление размера в каталоге. */
static void _logFlushLocked(void)
{
    if (!logFile) return;

    fflush(logFile);
    fsync(fileno(logFile));

    s_bytesSinceFlush = 0;
    s_lastFlushTick   = xTaskGetTickCount();
}

/* Дописать и закрыть. Вызывать под logMutex. */
static void _logCloseLocked(void)
{
    if (!logFile) return;

    _logFlushLocked();
    fclose(logFile);
    logFile = NULL;
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
                if (!logFile)    _logOpenLocked();

                if (logFile)
                {
                    int written = fprintf(logFile, "%s\n", logBuff);
                    if (written > 0) s_bytesSinceFlush += written;

                    long fsz = ftell(logFile);   /* включает данные в буфере */

                    /* Метаданные тома трогаем редко: по объёму, по времени и
                       всегда на ошибке - её причина должна лечь на карту. */
                    if ((level <= E)                                                                ||
                        (s_bytesSinceFlush >= LOG_FLUSH_BYTES)                                      ||
                        ((xTaskGetTickCount() - s_lastFlushTick) >= pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS)))
                    {
                        _logFlushLocked();
                    }

                    /* достигли лимита - переходим к следующему файлу по кругу
                       и перезаписываем его (стираем перед первой записью).
                       Следующий mblog откроет его сам. */
                    if (fsz >= s_capPerFile)
                    {
                        char fn[64];

                        _logCloseLocked();

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

void mblog_flush(void)
{
    if (!logMutex) return;

    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(LOG_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        _logFlushLocked();
        xSemaphoreGive(logMutex);
    }
}

void mblog_close(void)
{
    if (!logMutex) return;

    /* Таймаут, а не portMAX_DELAY: зовётся из safeRestart, и перезагрузка не
       должна повиснуть навсегда из-за задачи, застрявшей с мьютексом лога. */
    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(LOG_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        _logCloseLocked();
        xSemaphoreGive(logMutex);
    }
}
