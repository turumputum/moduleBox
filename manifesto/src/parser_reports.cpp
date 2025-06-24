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

void Parser::resetReports(bool first)
{
    for (int i = 0; i < int(sizeof(reps) / sizeof(Report)); i++)
    {
        *reps[i].s.funcName = 0;
        reps[i].type = RPTT_unknown;
        reps[i].line = 0;
        *reps[i].name = 0;
        *reps[i].unit = 0;

        if (!first)
        {
            if (!reps[i].paramsRaw)
                free(reps[i].paramsRaw);

            if (!reps[i].descRaw)
                free(reps[i].descRaw);
        }

        reps[i].paramsRaw = 0;
        reps[i].descRaw = 0;


        reps[i].maxVal = 0;
        reps[i].minVal = 0;

        reps[i].maxValF = 0;
        reps[i].minValF = 0;

    }
    
    numOfReps = 0;
}
bool Parser::extractRepParam(Report &       rep,
                             int            idx,
                             char *         value)
{
    bool result = true;
    //char * end_ptr;

    switch (idx)
    {
        case 0:
            // skip
            break;

        case 1:
            // skip
            break;
        
        case 2:
            // skip
            break;
        
        case 3:
            //opt.defaultVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;
        
        case 4:
            //opt.minVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;

        case 5:
            //opt.maxVal = strtol(value, &end_ptr, 10);
            //result = errno != EINVAL;
            break;

        default:
            //result = false;
            break;
    }

    return result;
}
bool Parser::extractRepParams(Report &     rep)
{
    bool        result  = true;
    char *      begin   = rep.paramsRaw;
    int         idx     = 0;
    char *      end;
    char        tmp     [ 1024 ];

    while (result && ((end = strchr(begin, ',')) != nil))
    {
        copyFromTo(tmp, begin, end);

        cleanValue(tmp);

        printf("param %d = '%s'\n", idx, tmp);

        switch (rep.type)
        {
            case RPTT_unknown:
                if (idx == 0)
                {
                        if (!strcmp(tmp, "RPTT_string"))
                        {
                            rep.type = RPTT_string;
                        }
                        else if (!strcmp(tmp, "RPTT_int"))
                        {
                            rep.type = RPTT_int;
                        }
                        else if (!strcmp(tmp, "RPTT_float"))
                        {
                            rep.type = RPTT_float;
                        }
                        else if (!strcmp(tmp, "RPTT_ratio"))
                        {
                            rep.type = RPTT_ratio;
                        }
                        else    
                        {
                            printf("error: cannot extract type of report (2)\n");
                            result = false;
                        }
                }
                else    
                {
                    printf("error: cannot extract type of report (1)\n");
                    result = false;
                }
                break;

            // case OPTTYPE_int:
            //     result = extractOptIntParam(opt, idx, tmp);
            //     break;
            
            // case OPTTYPE_flag:
            //     result = extractOptFlagParam(opt, idx, tmp);
            //     break;

            // case OPTTYPE_string:
            //     result = extractOptStringParam(opt, idx, tmp);
            //     break;

            // case OPTTYPE_float:
            //     result = extractOptFloatParam(opt, idx, tmp);
            //     break;

            // case OPTTYPE_enum:
            //     result = extractOptEnumParam(opt, idx, tmp);
            //     break;

            // case OPTTYPE_color:
            //     result = extractOptColorParam(opt, idx, tmp);
            //     break;

            default:
                break;
        }

        begin = end + 1;
        idx++;
    }

    return result;
}
bool Parser::parseReportParams(Report &     rep,
                               char *       report)
{
    bool        result = false;
    char *      params;
    char *      end;

    if ((params = strchr(report, '(')) != nil)
    {
//        printf("parseOptiosParams stage %d\n", stage);

        params++;
        if ((end = strchr(params, ')')) != nil)
        {
            if (dupFromTo(rep.paramsRaw, params, end) > 0)
            {
                int len = strlen(rep.paramsRaw);
                rep.paramsRaw[ len + 0 ] = ',';
                rep.paramsRaw[ len + 1 ] = 0;

                if (extractRepParams(rep))
                {
                    // if (extractOptDesc(rep, report))
                    // {
                    //     result = true;
                    // }
                    // else
                    //     printf("error: cannot extractOpt desc for opt %s(%d)\n", rep.name, rep.line);
                }
                else
                    printf("error: cannot extractOpt params for opt %s(%d)\n", rep.name, rep.line);
            }
            else
                printf("error: cannot get params body for opt %s(%d)\n", rep.name, rep.line);
        }
        else
            printf("error: cannot get params end for opt %s(%d)\n", rep.name, rep.line);
    }
    else
        printf("error: cannot get params begin for opt %s(%d)\n", rep.name, rep.line);

    return result;
}
bool Parser::getReports()
{
    bool        result = true;

    char * on;

    resetReports(false);

    while (result && ((on = findNextFunction(reps[numOfReps].s, "stdreport_register", "")) != nil))
    {
        //char * name = reps[numOfReps].s.funcName;

        // if (!strcmp(name, "get_option_int_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_int;
        // }
        // else if (!strcmp(name, "get_option_string_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_string;
        // }
        // else if (!strcmp(name, "get_option_float_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_float;
        // }
        // else if (!strcmp(name, "get_option_flag_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_flag;
        // }
        // else if (!strcmp(name, "get_option_enum_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_enum;
        // }
        // else if (!strcmp(name, "get_option_color_val"))
        // {
        //     reps[numOfReps].type = OPTTYPE_color;
        // }

        if (!parseReportParams(reps[numOfReps], on))
        {
            result = false;
        }
        else if (++numOfReps >= MAX_PARAMS)
        {
            printf("error: number of parameters exeed possible number (%d)\n", MAX_PARAMS);

            result = false;
        }
    }

    return result;
}
