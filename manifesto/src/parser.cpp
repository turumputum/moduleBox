// ***************************************************************************
// TITLE
//
// PROJECT
//     manifesto
// ***************************************************************************

#include <stdio.h>
#include <parser.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

const char * CONFIGURE_BEGIN = "void configure_";

//int stage = 1;

// ---------------------------------------------------------------------------
// -------------------------------- OptionS --------------------------------
// -----------------|---------------------------(|------------------|---------

char * Parser::findNextFunction(FUNCSEARCH &    f,
                                const char *    part1,
                                const char *    part2)
{
    char *              result = 0;
    char *              begin;
    char *              end;

    if ((begin = strstr(funcSearchEnd,  part1)) != nil)
    {
        if ((end = strchr(begin, '(')) != nil)
        {
            copyFromTo(f.funcName, begin, end);
            cleanValue(f.funcName);
            int len1 = strlen(f.funcName);
            int len2 = strlen(part2);

            if (!len2 || !memcmp(f.funcName + (len1 - len2), part2, len2))
            {
                result          = begin;
                funcSearchEnd   = result + strlen(f.funcName);

            }
            else
            {
                funcSearchEnd   = result + strlen(part1);
                result          = findNextFunction(f, part1, part2);
            }
        }
    }

    return result;
}

void Parser::copyFromTo(char *         dest, 
                        char *         begin,
                        char *         end)
{
    int len = end - begin;
    memcpy(dest, begin, len);

    dest[len] = 0;
}

int Parser::dupFromTo(char * &       dest, 
                       char *         begin,
                       char *         end)
{
    int len = end - begin;

    if ((dest = (char*)malloc(len + 3)) != nil)
    {
        memcpy(dest, begin, len);
        dest[len] = 0;
    }

    return len;
}
void Parser::strdup(char * &       dest,
                    const char *   begin)
{
    if ((dest = (char*)malloc(strlen(begin) + 1)) != nil)
    {
        strcpy(dest, begin);
    }
}
void Parser::cleanValue(char *         value)
{
    unsigned char * on = (unsigned char *)value;

    //printf("cleanValue: '%s' ->", value);

    // Skip begin
    while (*on && ((*on <= ' ') || (*on == '\"')))
    {
        on++;
    }

    int len = strlen((char*)on);

//    printf("          len = %d     ", len);

    if (len)
    {
        memmove(value, on, len);
        value[len] = 0;

        on = (unsigned char*)&value[len - 1];

        // Clean end
        while ((on > (unsigned char*)value) && ((*on <= ' ') || (*on == '\"')))
        {
            on--;
        }

        *(on + 1) = 0;
    }
    else
        *value = 0;

    //printf("'%s'\n", value);
}
char * Parser::backstrstrGlobal(char *         haystack,
                                const char *   needle)
{
    char *  result          = nil;
    char *  on              = haystack;
    int     len             = strlen(needle);

    while (!result && *on && (on != source))
    {
        if ((*on == *needle) && (!memcmp(on, needle, len)))
        {
            result = on;
        }
        else
            on--;
    }

    return result;
}
char * Parser::backstrstr(char *         haystack,
                          const char *   needle)
{
    char *  result          = nil;
    char *  on              = haystack;
    int     len             = strlen(needle);

    while (!result && (on != mod.begin))
    {
        if ((*on == *needle) && (!memcmp(on, needle, len)))
        {
            result = on;
        }
        else
            on--;
    }

    return result;
}
const char * Parser::parse(char * source)
{
    const char *            result  = 0;
    int                     mods    = 0;
    int                     rc;

    this->source    = source;
    modSearchEnd    = source;

    //manifesto.append("[");

    resetOptions(true);

    do
    {
        switch (rc = findNextModule())
        {
            case 0:
                break;

            case -1:
                printf("error searching for module\n");
                mods = 0;
                break;

            default:
                if (  getOptions()  &&
                      getReports()  )
                {
                    if (generateManifestoForModule())
                    {
                        mods++;
                    }
                    else
                        printf("error generating manifest for module %s\n", mod.name);
                }
                else
                    printf("error parsing for options\n");
                break;
        }

    } while (rc > 0);

    if (mods > 0)
    {
        //manifesto.append("]\n");
        result = manifesto.c_str();
    }

    return result;
}
bool Parser::generateManifestoForModule()
{
    bool            result      = true;
    Option *      f;
    char            tmp     [ 1024 ];

    snprintf(tmp, sizeof(tmp), "\n\t{\n\t\t\"mode\": \"%s\"\n\t\t\"description\": \"%s\"\n\t\t\"options\": [\n", mod.name, mod.descRaw);
    manifesto.append(tmp);

    for (int i = 0; i < numOfOpts; i++)
    {
        f = &opts[i];

        switch (f->type)
        {
            case OPTTYPE_int:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"int\",\n"
                        "\t\t\t\t\"unit\": \"%s\",\n"
                        "\t\t\t\t\"valueDefault\": %d,\n"
                        "\t\t\t\t\"valueMax\": %d,\n"
                        "\t\t\t\t\"valueMin\": %d\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw,
                        f->unit,
                        f->defaultVal,
                        f->maxVal,
                        f->minVal);
                   
                break;
        
            case OPTTYPE_float:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"float\",\n"
                        "\t\t\t\t\"valueDefault\": %g,\n"
                        "\t\t\t\t\"valueMax\": %g,\n"
                        "\t\t\t\t\"valueMin\": %g\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw,
                        f->defaultValF,
                        f->maxValF,
                        f->minValF);
                   
                break;
        
            case OPTTYPE_flag:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"flag\",\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw);
                   
                break;
        
            case OPTTYPE_string:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"string\",\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw);
                   
                break;
        
            case OPTTYPE_enum:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"enum\",\n"
                        "\t\t\t\t\"values\": [ ",
                        f->name,
                        f->descRaw);

                manifesto.append(tmp);

                if (f->enumsCount > 1)
                {
                    for (int i = 0; i < f->enumsCount - 1; i++)
                    {
                        if (i)
                            snprintf(tmp, sizeof(tmp), ", \"%s\"", f->enums[i]);
                        else
                            snprintf(tmp, sizeof(tmp), "\"%s\"", f->enums[i]);

                        manifesto.append(tmp);
                    }
                }                

                snprintf(tmp, sizeof(tmp), "%s", " ]\n\t\t\t},\n");
                  
                break;

            case OPTTYPE_color:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"color\",\n"
                        "\t\t\t\t\"valueDefault\": \"%s\",\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw,
                        f->defaultValStr);
                   
                break;


            default:
                break;
        }

        manifesto.append(tmp);
    }

    snprintf(tmp, sizeof(tmp), "\t},\n");
    manifesto.append(tmp);


    return result;
}
char * Parser::findCorellatedCurlyBrace(char *         begin)
{
    char *      result  = nil;
    char *      on      = begin;
    int         count   = 0;
    
    do
    {
        //printf("@@@@@@@@ '%c', count = %d | ", *on, count);

        if (*on == '{')
        {
            count++;
        }
        else if (*on == '}')
        {
            count--;
        }
        
        if (!count)
        {
            result = on;
        }

        on++;

        //printf("count = %d, result = %p\n", count, result);

    } while (*on && !result);
    
    return result;
}
bool Parser::getModuleDesc(char *         moduleBegin)
{
    bool        result = false;
    char *      begin;
    char *      end;

    if ((end = backstrstrGlobal(moduleBegin, "*/")) != nil)
    {
        if ((begin = backstrstrGlobal(end, "/*")) != nil)
        {
            begin += 2;

            if (dupFromTo(mod.descRaw, begin, end) > 0)
            {
                cleanValue(mod.descRaw);
                result = true;
            }
            else
                printf("error: cannot get desc body for module %s\n", mod.name);
        }
        else
            printf("error: cannot get desc begin for module %s\n", mod.name);
    }
    else
        printf("error: cannot get desc end for module %s\n", mod.name);

    return result;
}
bool Parser::getModuleName(char *         begin)
{
    bool            result      = false;
    char *          on          = begin;
    
    if ((on = strchr(begin, '(')) != nil)
    {
        copyFromTo(mod.name, begin + strlen(CONFIGURE_BEGIN), on);
        cleanValue(mod.name);

        result = true;
    }

    return result;
}
int Parser::findNextModule()
{
    int                 result          = -1;
    char *              begin;
    char *              end;

    if ((begin = strstr(modSearchEnd,  CONFIGURE_BEGIN)) != nil)
    {
        //printf("parser.cpp:669 - stage FOUND !!! %p \n", begin);
        
        if ((mod.begin = strchr(begin, '{')) != nil)
        {
            if ((end = findCorellatedCurlyBrace(mod.begin)) != nil)
            {
                if (getModuleName(begin))
                {
                    if (getModuleDesc(begin))
                    {
                        funcSearchEnd   = mod.begin;
                        *end            = 0;
                        modSearchEnd    = ++end;

                        result = 1;
                    }
                    else
                        printf("error: cannot get description of module\n");
                }
                else
                    printf("error: cannot get name of module\n");
            }
            else
                printf("error: cannot get end of module configure Option\n");
        }
        else
            printf("error: cannot get begin of module configure Option\n");
    }
    else 
        result = 0;

    return result;
}
