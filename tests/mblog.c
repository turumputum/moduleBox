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
#include <stateStruct.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define DEF_LOG_FILE_BASE_NAME  "/sdcard/log"

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static char                 srcFname    [ 64 ];
static char                 tgtFname    [ 64 ];
static char                 buffOut     [ 256 ];

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

bool file_exists (char *filename) 
{
  struct stat   buffer;   
  return (stat (filename, &buffer) == 0);
}

// void mblog_init(int sizeofLogs, int numberOfFiles)
// {
//     gSizeofLogs     = sizeofLogs;
//     gNumberOfFiles  = numberOfFiles;
// }


static void _shiftLogs()
{
    int startNum = me_state->logChapters - 1;

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

    if ((logFile = fopen(DEF_LOG_FILE_BASE_NAME ".txt", "a")) != NULL)
    {
        va_start(st_va_list, msg);
        sz = vsnprintf(buffOut, sizeof(buffOut) - 1, msg, st_va_list);
        va_end(st_va_list);

        *(buffOut + sz) = 0;

        fprintf(logFile, "%s\n", buffOut);

        sz = ftell(logFile);

        fclose(logFile);

        if (sz > (me_state->logMaxSize / me_state->logChapters))
        {
            _shiftLogs();
        }

    }
}
int main(void)
{
    for (int i = 0; i < 100; i++)
    {
        mblog(0, "test message number %d", i);
    }

    return 0;
}
