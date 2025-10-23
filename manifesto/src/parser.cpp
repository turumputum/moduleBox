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

void Parser::resetCommon(bool           first)
{
    if (!first)
    {
        funcSearchEnd   = mod.begin;
    }
}

char * Parser::findNextFunction(Common &        c,
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
            copyFromTo(c.funcName, begin, end);
            cleanValue(c.funcName);
            int len1 = strlen(c.funcName);
            int len2 = strlen(part2);

            if (!len2 || !memcmp(c.funcName + (len1 - len2), part2, len2))
            {
                result          = begin;
                funcSearchEnd   = result + strlen(c.funcName);

            }
            else
            {
                funcSearchEnd   = result + strlen(part1);
                result          = findNextFunction(c, part1, part2);
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
    int oriLen  = end - begin;
    int len     = 0;
    

    if ((dest = (char*)malloc(oriLen * 2 + 3)) != nil)
    {
        // len = oriLen;
        // memcpy(dest, begin, len);

        for (int i = 0; i < oriLen; i++)
        {
            switch (begin[i])
            {
                case '\r':
                    break;

                case '\t':
                    dest[len++] = '\\';
                    dest[len++] = 't';
                    break;

                case '\n':
                    dest[len++] = '\\';
                    dest[len++] = 'n';
                    break;

                default:
                    dest[len++] = begin[i];
                    break;
            }
        }

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
bool Parser::extractCommonDesc(Common &     c,
                               char *         Option)
{
    bool        result = false;
    char *      begin;
    char *      end;

    if ((end = backstrstr(Option, "*/")) != nil)
    {
        if ((begin = backstrstr(end, "/*")) != nil)
        {
            begin += 2;

            if (dupFromTo(c.descRaw, begin, end) > 0)
            {
                cleanValue(c.descRaw);
                result = true;
            }
            else
                printf("error: cannot get desc body for opt %s(%d)\n", c.name, c.line);
        }
        else
            printf("error: cannot get desc begin for opt %s(%d)\n", c.name, c.line);
    }
    else
        printf("error: cannot get desc end for opt %s(%d)\n", c.name, c.line);

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
    resetReports(true);
    resetCommands(true);

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
                      getReports()  && 
                      getCommands() )
                {
                    if (generateManifestoForModule(0 == mods))
                    {
                        mods++;
                    }
                    else
                        printf("error generating manifest for module %s\n", mod.name);
                }
                else
                    printf("error parsing for options, report or command\n");
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
bool Parser::generateManifestoForModule(bool first)
{
    bool            result      = true;
    char            tmp     [ 1024 ];

    snprintf(tmp, sizeof(tmp), "%s\t{\n\t\t\"mode\": \"%s\",\n\t\t\"slots\": \"%d-%d\",\n\t\t\"description\": \"%s\",\n", first ? "\n" : ",\n", 
        mod.name, mod.slotFrom, mod.slotTo,
        mod.descRaw);
    manifesto.append(tmp);

    if (  generateManifestoOfOptions()  && 
          generateManifestoOfReports()  &&
          generateManifestoOfCommands() )
    {
        snprintf(tmp, sizeof(tmp), "%s", "\t}");
        manifesto.append(tmp);
        result = true;
    }

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
void Parser::searchSlots(char * desc)
{
    char * on = strstr(desc, "slots:");
    char * from;
    char * to;

    if (on)
    {
        *on = 0;
        on++;
        if ((on = strchr(on, ':')) != nil)
        {
            from = ++on;
            if ((to = strchr(from, '-')) != nil)
            {
                to++;

                mod.slotFrom    = atoi(from);
                mod.slotTo      = atoi(to);
            }
            else
                mod.slotFrom = mod.slotTo = atoi(from);
        }
    }
    else
    {
        mod.slotFrom = 0;
        mod.slotTo   = 5;
    }

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
                searchSlots(mod.descRaw);
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
