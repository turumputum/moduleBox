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
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

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

void Parser::resetSearching()
{
    searchEnd = source;    
}
void Parser::cleanValue(char *         value)
{
    unsigned char * on = (unsigned char *)value;

    printf("cleanValue: '%s' ->", value);

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

    printf("'%s'\n", value);
}
char * Parser::findNextFunction(Function &     func,
                                const char *   part1,
                                const char *   part2)
{
    char *              result = 0;
    char *              begin;
    char *              end;

    if ((begin = strstr(searchEnd,  part1)) != nil)
    {
        if ((end = strchr(begin, '(')) != nil)
        {
            copyFromTo(func.funcName, begin, end);
            cleanValue(func.funcName);
            int len1 = strlen(func.funcName);
            int len2 = strlen(part2);

            if (!memcmp(func.funcName + (len1 - len2), part2, len2))
            {
                result      = begin;
                searchEnd   = result + strlen(func.funcName);

            }
            else
            {
                searchEnd   = result + strlen(part1);
                result      = findNextFunction(func, part1, part2);
            }
        }
    }

    return result;
}
// 

bool Parser::getOptions()
{
    bool        result = true;

    resetSearching();

    char * on;

    while (result && ((on = findNextFunction(funcs[numOfFuncs], "get_option_", "_val")) != nil))
    {
        char * name = funcs[numOfFuncs].funcName;

        if (!strcmp(name, "get_option_int_val"))
        {
            funcs[numOfFuncs].type = PARAMTYPE_int;
        }
        else if (!strcmp(name, "get_option_string_val"))
        {
            funcs[numOfFuncs].type = PARAMTYPE_string;
        }
        else if (!strcmp(name, "get_option_float_val"))
        {
            funcs[numOfFuncs].type = PARAMTYPE_float;
        }
        else if (!strcmp(name, "get_option_flag_val"))
        {
            funcs[numOfFuncs].type = PARAMTYPE_flag;
        }

        if (!parseFunctionParams(funcs[numOfFuncs], on))
        {
            result = false;
        }
        else if (++numOfFuncs >= MAX_PARAMS)
        {
            printf("error: number of parameters exeed possible number (%d)\n", MAX_PARAMS);

            result = false;
        }
    }

    return result;
}
char * Parser::backstrstr(char *         haystack,
                          const char *   needle)
{
    char *  result          = nil;
    char *  on              = haystack;
    int     len             = strlen(needle);

    while (!result && (on != source))
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
bool Parser::extractDesc(Function &     func,
                         char *         function)
{
    bool        result = false;
    char *      begin;
    char *      end;

    if ((end = backstrstr(function, "*/")) != nil)
    {
        if ((begin = backstrstr(end, "/*")) != nil)
        {
            begin += 2;

            if (dupFromTo(func.descRaw, begin, end) > 0)
            {
                cleanValue(func.descRaw);
                result = true;
            }
            else
                printf("error: cannot get desc body for func %s(%d)\n", func.name, func.line);
        }
        else
            printf("error: cannot get desc begin for func %s(%d)\n", func.name, func.line);
    }
    else
        printf("error: cannot get desc end for func %s(%d)\n", func.name, func.line);

    return result;
}
bool Parser::extractIntParam(Function &     func,
                             int            idx,
                             char *         value)
{
    bool result = true;
    char * end_ptr;

    switch (idx)
    {
        case 0:
            // skip
            break;

        case 1:
            strcpy(func.name, value);
            break;
        
        case 2:
            strcpy(func.unit, value);
            break;
        
        case 3:
            func.defaultVal = strtol(value, &end_ptr, 10);
            result = errno == 0;
            break;
        
        case 4:
            func.minVal = strtol(value, &end_ptr, 10);
            result = errno == 0;
            break;

        case 5:
            func.maxVal = strtol(value, &end_ptr, 10);
            result = errno == 0;
            break;

        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractFloatParam(Function &     func,
                               int            idx,
                               char *         value)
{
    bool result = true;

    switch (idx)
    {
        case 0:
            // skip
            break;

        case 1:
            strcpy(func.name, value);
            break;
        
        case 2:
            func.defaultValF = atof(value);
            result = errno == 0;
            break;
        
        case 3:
            func.minValF = atof(value);
            result = errno == 0;
            break;

        case 4:
            func.maxValF = atof(value);
            result = errno == 0;
            break;

        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractFlagParam(Function &     func,
                             int            idx,
                             char *         value)
{
    bool result = true;

    switch (idx)
    {
        case 0:
            // skip
            break;

        case 1:
            strcpy(func.name, value);
            break;
        
        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractStringParam(Function &     func,
                                int            idx,
                                char *         value)
{
    bool result = true;

    switch (idx)
    {
        case 0:
            // skip
            break;

        case 1:
            strcpy(func.name, value);
            break;
        
        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractParams(Function &     func)
{
    bool        result  = true;
    char *      begin   = func.paramsRaw;
    int         idx     = 0;
    char *      end;
    char        tmp     [ 1024 ];

    while (result && ((end = strchr(begin, ',')) != nil))
    {
        copyFromTo(tmp, begin, end);

        cleanValue(tmp);

        //printf("param %d = '%s'\n", idx, tmp);

        switch (func.type)
        {
            case PARAMTYPE_int:
                result = extractIntParam(func, idx, tmp);
                break;
            
            case PARAMTYPE_flag:
                result = extractFlagParam(func, idx, tmp);
                break;

            case PARAMTYPE_string:
                result = extractStringParam(func, idx, tmp);
                break;

            case PARAMTYPE_float:
                result = extractFloatParam(func, idx, tmp);
                break;

            default:
                break;
        }

        begin = end + 1;
        idx++;
    }

    return result;
}

bool Parser::parseFunctionParams(Function &     func,
                                 char *         function)
{
    bool        result = false;
    char *      params;
    char *      end;

    if ((params = strchr(function, '(')) != nil)
    {
        params++;
        if ((end = strchr(params, ')')) != nil)
        {
            if (dupFromTo(func.paramsRaw, params, end) > 0)
            {
                int len = strlen(func.paramsRaw);
                func.paramsRaw[ len + 0 ] = ',';
                func.paramsRaw[ len + 1 ] = 0;

                if (extractParams(func))
                {
                    if (extractDesc(func, function))
                    {
                        result = true;
                    }
                }
            }
            else
                printf("error: cannot get params body for func %s(%d)\n", func.name, func.line);
        }
        else
            printf("error: cannot get params end for func %s(%d)\n", func.name, func.line);
    }
    else
        printf("error: cannot get params begin for func %s(%d)\n", func.name, func.line);

    return result;
}
const char * Parser::parse(char * source)
{
    const char *            result = 0;
   
    this->source = source;

    if (getOptions())
    {
        result = generateManifesto();
    }
    else
        printf("error parsing for options\n");
    
    return result;
}
bool Parser::generateManifestoForModule()
{
    bool            result      = true;
    Function *      f;
    char            tmp     [ 1024 ];

    snprintf(tmp, sizeof(tmp), "\t{\n\t\t\"mode\": \"%s\"\n\t\t\"description\": \"%s\"\n\t\t\"options\": [\n", mod.name, mod.descRaw);
    manifesto.append(tmp);

    for (int i = 0; i < numOfFuncs; i++)
    {
        f = &funcs[i];

        switch (f->type)
        {
            case PARAMTYPE_int:
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
        
            case PARAMTYPE_float:
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
        
            case PARAMTYPE_flag:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"flag\",\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw);
                   
                break;
        
            case PARAMTYPE_string:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"name\": \"%s\",\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"string\",\n"
                        "\t\t\t},\n",
                        f->name,
                        f->descRaw);
                   
                break;
        
            default:
                break;
        }

        manifesto.append(tmp);
    }

    snprintf(tmp, sizeof(tmp), "\t},");
    manifesto.append(tmp);


    return result;
}
const char * Parser::generateManifesto()
{
    const char *          result  = nil;

    manifesto.append("[\n");

    strcpy(mod.name, "adc1");
    strdup(mod.descRaw, "ADC convertor");

    if (generateManifestoForModule())
    {
        manifesto.append("]\n");
        result = manifesto.c_str();
    }

    return result;
}
