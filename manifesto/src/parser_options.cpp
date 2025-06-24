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
// -------------------------------- OptionS --------------------------------
// -----------------|---------------------------(|------------------|---------

// char * Parser::findNextOption(Option &     opt,
//                                 const char *   part1,
//                                 const char *   part2)
// {
//     char *              result = 0;
//     char *              begin;
//     char *              end;

//     if ((begin = strstr(stageSearchEnd,  part1)) != nil)
//     {
//         if ((end = strchr(begin, '(')) != nil)
//         {
//             copyFromTo(opt.funcName, begin, end);
//             cleanValue(opt.funcName);
//             int len1 = strlen(opt.funcName);
//             int len2 = strlen(part2);

//             if (!memcmp(opt.funcName + (len1 - len2), part2, len2))
//             {
//                 result      = begin;
//                 stageSearchEnd   = result + strlen(opt.funcName);

//             }
//             else
//             {
//                 stageSearchEnd   = result + strlen(part1);
//                 result      = findNextOption(opt, part1, part2);
//             }
//         }
//     }

//     return result;
// }
void Parser::resetOptions(bool first)
{
    for (int i = 0; i < int(sizeof(opts) / sizeof(Option)); i++)
    {
        *opts[i].s.funcName = 0;
        opts[i].type = OPTTYPE_unknown;
        opts[i].line = 0;
        *opts[i].name = 0;
        *opts[i].unit = 0;

        if (!first)
        {
            if (!opts[i].paramsRaw)
                free(opts[i].paramsRaw);

            if (!opts[i].descRaw)
                free(opts[i].descRaw);
        }

        opts[i].paramsRaw = 0;
        opts[i].descRaw = 0;


        opts[i].defaultVal = 0;
        opts[i].maxVal = 0;
        opts[i].minVal = 0;

        opts[i].defaultValF = 0;
        opts[i].maxValF = 0;
        opts[i].minValF = 0;

        opts[i].enumsCount = 0;
    }
    
    numOfOpts = 0;
}
bool Parser::extractOptDesc(Option &     opt,
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

            if (dupFromTo(opt.descRaw, begin, end) > 0)
            {
                cleanValue(opt.descRaw);
                result = true;
            }
            else
                printf("error: cannot get desc body for opt %s(%d)\n", opt.name, opt.line);
        }
        else
            printf("error: cannot get desc begin for opt %s(%d)\n", opt.name, opt.line);
    }
    else
        printf("error: cannot get desc end for opt %s(%d)\n", opt.name, opt.line);

    return result;
}
bool Parser::extractOptIntParam(Option &     opt,
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
            strcpy(opt.name, value);
            break;
        
        case 2:
            strcpy(opt.unit, value);
            break;
        
        case 3:
            opt.defaultVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;
        
        case 4:
            opt.minVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;

        case 5:
            opt.maxVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;

        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractOptFloatParam(Option &     opt,
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
            strcpy(opt.name, value);
            break;
        
        case 2:
            opt.defaultValF = atof(value);
            //result = errno == ERANGE;
            break;
        
        case 3:
            opt.minValF = atof(value);
            //result = errno != ERANGE;
            break;

        case 4:
            opt.maxValF = atof(value);
            //result = errno != ERANGE;
            break;

        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractOptFlagParam(Option &     opt,
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
            strcpy(opt.name, value);
            break;
        
        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractOptStringParam(Option &     opt,
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
            strcpy(opt.name, value);
            break;
        
        default:
            result = false;
            break;
    }

    return result;
}
bool Parser::extractOptEnumParam(Option &     opt,
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
            strcpy(opt.name, value);
            break;
        
        default:
            if (opt.enumsCount < DEF_MAX_ENUMS)
            {
                opt.enums[opt.enumsCount] = (char*)malloc (strlen(value) + 1);
                strcpy(opt.enums[opt.enumsCount], value);
                opt.enumsCount++;
            }
            else
                result = false;
            break;
    }

    //printf("extractOptEnumParam = %d\n", result);

    return result;
}
bool Parser::extractOptColorParam(Option &     opt,
                               int            idx,
                               char *         value)
{
    bool result = true;

    switch (idx)
    {
        case 0:
            // skip - output
            break;

        case 1:
            // skip - slot number
            break;

        case 2:
            strcpy(opt.name, value);
            break;

        case 3:
            strcpy(opt.defaultValStr, value);
            break;

        default:
            result = false;
            break;
    }

    //printf("extractOptEnumParam = %d\n", result);

    return result;
}
bool Parser::extractOptParams(Option &     opt)
{
    bool        result  = true;
    char *      begin   = opt.paramsRaw;
    int         idx     = 0;
    char *      end;
    char        tmp     [ 1024 ];

    while (result && ((end = strchr(begin, ',')) != nil))
    {
        copyFromTo(tmp, begin, end);

        cleanValue(tmp);

        //printf("param %d = '%s'\n", idx, tmp);

        switch (opt.type)
        {
            case OPTTYPE_int:
                result = extractOptIntParam(opt, idx, tmp);
                break;
            
            case OPTTYPE_flag:
                result = extractOptFlagParam(opt, idx, tmp);
                break;

            case OPTTYPE_string:
                result = extractOptStringParam(opt, idx, tmp);
                break;

            case OPTTYPE_float:
                result = extractOptFloatParam(opt, idx, tmp);
                break;

            case OPTTYPE_enum:
                result = extractOptEnumParam(opt, idx, tmp);
                break;

            case OPTTYPE_color:
                result = extractOptColorParam(opt, idx, tmp);
                break;


            default:
                break;
        }

        begin = end + 1;
        idx++;
    }

    return result;
}
bool Parser::getOptions()
{
    bool        result = true;

    char * on;

    resetOptions(false);

    while (result && ((on = findNextFunction(opts[numOfOpts].s, "get_option_", "_val")) != nil))
    {
        // printf("parser.cpp:162 - stage %d\n", stage);
        // stage++;

        char * name = opts[numOfOpts].s.funcName;

        if (!strcmp(name, "get_option_int_val"))
        {
            opts[numOfOpts].type = OPTTYPE_int;
        }
        else if (!strcmp(name, "get_option_string_val"))
        {
            opts[numOfOpts].type = OPTTYPE_string;
        }
        else if (!strcmp(name, "get_option_float_val"))
        {
            opts[numOfOpts].type = OPTTYPE_float;
        }
        else if (!strcmp(name, "get_option_flag_val"))
        {
            opts[numOfOpts].type = OPTTYPE_flag;
        }
        else if (!strcmp(name, "get_option_enum_val"))
        {
            opts[numOfOpts].type = OPTTYPE_enum;
        }
        else if (!strcmp(name, "get_option_color_val"))
        {
            opts[numOfOpts].type = OPTTYPE_color;
        }

        if (!parseOptiosParams(opts[numOfOpts], on))
        {
            result = false;
        }
        else if (++numOfOpts >= MAX_PARAMS)
        {
            printf("error: number of parameters exeed possible number (%d)\n", MAX_PARAMS);

            result = false;
        }
    }

    return result;
}
bool Parser::parseOptiosParams(Option &     opt,
                               char *         option)
{
    bool        result = false;
    char *      params;
    char *      end;

    if ((params = strchr(option, '(')) != nil)
    {
//        printf("parseOptiosParams stage %d\n", stage);

        params++;
        if ((end = strchr(params, ')')) != nil)
        {
            if (dupFromTo(opt.paramsRaw, params, end) > 0)
            {
                int len = strlen(opt.paramsRaw);
                opt.paramsRaw[ len + 0 ] = ',';
                opt.paramsRaw[ len + 1 ] = 0;

                if (extractOptParams(opt))
                {
                    if (extractOptDesc(opt, option))
                    {
                        result = true;
                    }
                    else
                        printf("error: cannot extract desc for opt %s(%d)\n", opt.name, opt.line);
                }
                else
                    printf("error: cannot extract params for opt %s(%d)\n", opt.name, opt.line);
            }
            else
                printf("error: cannot get params body for opt %s(%d)\n", opt.name, opt.line);
        }
        else
            printf("error: cannot get params end for opt %s(%d)\n", opt.name, opt.line);
    }
    else
        printf("error: cannot get params begin for opt %s(%d)\n", opt.name, opt.line);

    return result;
}
