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
#include <stateConfig.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define DEF_LOG_FILE_BASE_NAME  "/sdcard/log"

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern configuration        me_config;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

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
void mblog(int priority, const char *msg, ...)
{
    FILE *              logFile;
    va_list             st_va_list;
    size_t              sz;
    char                buffOut     [ 386 ];

    if ((logFile = fopen(DEF_LOG_FILE_BASE_NAME ".txt", "a")) != NULL)
    {
        sz = snprintf(buffOut, sizeof(buffOut), "%.8d ", (int)pdTICKS_TO_MS(xTaskGetTickCount()));

        va_start(st_va_list, msg);
        sz = vsnprintf(buffOut + sz, sizeof(buffOut) - sz - 1, msg, st_va_list);
        va_end(st_va_list);

        *(buffOut + sz) = 0;

        fprintf(logFile, "%s\n", buffOut);
        
        printf("\x1b[31mMBL%d %s\x1b[0m\n", priority, buffOut);

        sz = ftell(logFile);

        fclose(logFile);

        if (sz > (me_config.logMaxSize / me_config.logChapters))
        {
            _shiftLogs();
        }
    }
}
