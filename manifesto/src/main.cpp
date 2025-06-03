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

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define RESULT_ERROR        -1
#define RESULT_OK           0

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

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
                            fprintf(fout, "%s", manifesto);

                            printf("%s", manifesto);

                            result = RESULT_OK;
                        }
                    }
                    else
                        printf("cannot read file %s\n", argv[1]);
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
