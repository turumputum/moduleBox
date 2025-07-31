// ***************************************************************************
// TITLE
//
// PROJECT
//
// ***************************************************************************
//
// FILE
//      $Id$
// HISTORY
//      $Log$
// ***************************************************************************

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <parser.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define RESULT_ERROR        -1
#define RESULT_OK           0

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef char * PSTR;
typedef char CHAR;
typedef const char * PCSTR;
typedef char BOOL;
typedef unsigned UINT;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

PSTR strz_dup(PCSTR psz_string)
{
    PSTR        psz_copy    = NULL;
    UINT        u_size;

    if ((u_size = strlen(psz_string)) > 0)
    {
        u_size++;

        if ((psz_copy = (PSTR)malloc(u_size)) != NULL)
        {
            memcpy(psz_copy, psz_string, u_size);
        }
    }

    return psz_copy;
}
BOOL axpath_create(PSTR psz_path)
{
    BOOL            b_result        = false;
    PSTR            psz_copy;
    PSTR            psz_on;
    PSTR            psz_slash;

    if ((psz_copy = strz_dup(psz_path)) != NULL)
    {
        psz_on = psz_copy;
    
#if (TARGET_FAMILY == __AX_win__)

        for (   psz_slash = psz_copy;
                ((psz_slash = strchr(psz_slash, '\\')) != NULL);
                psz_slash++)
        {
            *psz_slash = '/';
        }

        if ((psz_slash = strchr(psz_copy, ':')) != NULL)
        {
            psz_on = ++psz_slash;
        }
#endif
            
        if (*psz_on == '/')
            psz_on++;

        do
        {
            if ((psz_slash = strchr(psz_on, '/'))  != NULL)
                *psz_slash = 0;
    
#ifdef __linux__
            b_result = !mkdir(psz_copy, 0755) || (errno == EEXIST);
#else
            b_result = !mkdir(psz_copy) || (errno == EEXIST);
#endif

            if (psz_slash)
            {
                *psz_slash = '/';
                psz_on  = ++psz_slash;
            }

        } while (b_result && psz_slash);

        free(psz_copy);
    }

    return b_result;
}
bool axpath_create_to_file(char * psz_path_and_filename)
{
    bool        b_result    = false;
    unsigned    d_size;
    char *      psz_slash;
    char *      psz_path;

    if (psz_path_and_filename && *psz_path_and_filename)
    {
        if ((psz_path = (PSTR)malloc(d_size =
                strlen(psz_path_and_filename) + sizeof(CHAR))) != NULL)
        {
            memcpy(psz_path, psz_path_and_filename, d_size);

            if( ((psz_slash = strrchr(psz_path, '/'))  != NULL) ||
                ((psz_slash = strrchr(psz_path, '\\')) != NULL) )
            {
                *psz_slash = 0;
                b_result   = axpath_create(psz_path);
            }
            else
                b_result = true;

            free(psz_path);
        }
    }

    return b_result;
}
int string_to_header(FILE * fout, const char * manifesto)
{
    const char * on = manifesto;

    fprintf(fout, "static const char manifesto[] =\"");

    while (*on)
    {
        switch (*on)
        {
            case '"':
                fprintf(fout , "\\\"");
                break;

            case '\\':
                fprintf(fout , "\\\\");
                break;

            case '\n':
                fprintf(fout , "\\n\"\n\"");
                break;

            default:
                fprintf(fout , "%c", *on);
                break;
        }

        on++;
    }

    fprintf(fout, "\";\n");

    return RESULT_OK;
}
// ***************************************************************************
// FUNCTION
//      main
// PURPOSE
//
// PARAMETERS
//      int    argc   --
//      char * argv[] --
// RESULT
//      int --
// ***************************************************************************
int main(int argc, char * argv[])
{
    int             result          = RESULT_ERROR;
    FILE *          fin;
    FILE *          fout;
    size_t          flen;
    char *          script;
    const char *    manifesto;
    Parser          parser;

    if (argc > 2)
    {
        if ((fin = fopen(argv[1], "rt")) != NULL)
        {
            axpath_create_to_file(argv[2]);
            if ((fout = fopen(argv[2], "w+t")) != NULL)
            {
                fseek(fin, 0, SEEK_END);
                flen = ftell(fin);
                fseek(fin, 0, SEEK_SET);

                if ((script = (char*)malloc(flen + 1)) != NULL)
                {
                    if (fread(script, flen, 1, fin))
                    {
                        *(script + flen) = 0;
                        
                        if ((manifesto = parser.parse(script)) != NULL)
                        {
                            //fprintf(fout, "%s", manifesto);
                            //printf("%s", manifesto);
                            //result = RESULT_OK;

                            result = string_to_header(fout, manifesto);
                        }
                    }
                    else
                    {
                        perror("cannot read: ");
                        printf("file: %s\n", argv[1]);
                    }
                }
                else
                    printf("cannot allocate enought memroy for %s\n", argv[1]);

                fclose(fout);
            }
            else
                printf("cannot open target file %s\n", argv[2]);

            fclose(fin);
        }
        else
            printf("cannot open source file %s\n", argv[1]);
    }
    else
        printf("manifesto <input source file> <output include file>\n");

    return result;
}
