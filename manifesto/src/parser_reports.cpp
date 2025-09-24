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
        *reps[i].c.funcName = 0;
        reps[i].c.line = 0;
        reps[i].type = RPTT_unknown;
        *reps[i].c.name = 0;
        *reps[i].unit = 0;
        *reps[i].topic = 0;

        if (!first)
        {
            if (!reps[i].paramsRaw)
                free(reps[i].paramsRaw);

            if (!reps[i].c.descRaw)
                free(reps[i].c.descRaw);
        }

        reps[i].paramsRaw = 0;
        reps[i].c.descRaw = 0;


        reps[i].maxVal = 0;
        reps[i].minVal = 0;

        reps[i].maxValF = 0;
        reps[i].minValF = 0;

    }
    
    numOfReps = 0;
}
// bool Parser::extractRepParam(Report &       rep,
//                              int            idx,
//                              char *         value)
// {
//     bool result = true;
//     //char * end_ptr;

//     switch (idx)
//     {
//         case 0: // RPTT_int
//             // skip
//             break;

//         case 1: // slot_num
//             // skip
//             break;
        
//         case 2: // unit
//             strcpy(rep.unit, value);
//             break;
        
//         case 3: // topic
//             strcpy(rep.topic, value);
//             break;
        
//         default:
//             //result = false;
//             break;
//     }

//     return result;
// }
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

        //printf("param %d = '%s'\n", idx, tmp);

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

            default:

                switch (idx)
                {
                    // case 0: // RPTT_int
                    //     // skip
                    //     break;

                    case 1: // slot_num
                        // skip
                        break;
                    
                    case 2: // unit
                        strcpy(rep.unit, tmp);
                        break;
                    
                    case 3: // topic
                        strcpy(rep.topic, tmp);
                        break;
                    
                    default:
                        //result = false;
                        break;
                }

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
                    if (extractCommonDesc(rep.c, report))
                    {
                        result = true;
                    }
                    else
                        printf("error: cannot extract desc for rep %s(%d)\n", rep.c.name, rep.c.line);
                }
                else
                    printf("error: cannot extract params for rep %s(%d)\n", rep.c.name, rep.c.line);
            }
            else
                printf("error: cannot get params body for rep %s(%d)\n", rep.c.name, rep.c.line);
        }
        else
            printf("error: cannot get params end for rep %s(%d)\n", rep.c.name, rep.c.line);
    }
    else
        printf("error: cannot get params begin for rep %s(%d)\n", rep.c.name, rep.c.line);

    return result;
}
bool Parser::getReports()
{
    bool        result = true;

    char * on;

    resetReports(false);

    while (result && ((on = findNextFunction(reps[numOfReps].c, "stdreport_register", "")) != nil))
    {
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
bool Parser::generateManifestoOfReports()
{
    bool            result      = true;
    Report *        f;
    char            tmp     [ 1024 ];

    snprintf(tmp, sizeof(tmp), "%s", "\t\t\"reports\": [\n");
    manifesto.append(tmp);

    for (int i = 0; i < numOfReps; i++)
    {
        f = &reps[i];

        switch (f->type)
        {
    // RPTT_unknown,
    // RPTT_int,
    // RPTT_float,
    // RPTT_string,
    // RPTT_ratio,

            case RPTT_int:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"int\",\n"
                        "\t\t\t\t\"unit\": \"%s\",\n"
			"\t\t\t\t\"topic\": \"%s\",\n"
                        "\t\t\t},\n",
                        f->c.descRaw,
                        f->unit,
                        f->topic);
                break;
        
            case RPTT_float:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"float\",\n"
                        "\t\t\t\t\"unit\": \"%s\",\n"
                        "\t\t\t\t\"topic\": \"%s\",\n"
                        "\t\t\t},\n",
                        f->c.descRaw,
                        f->unit,
                        f->topic);
                break;

            case RPTT_string:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"string\",\n"
                        "\t\t\t\t\"unit\": \"%s\",\n"
                        "\t\t\t\t\"topic\": \"%s\",\n"
                        "\t\t\t},\n",
                        f->c.descRaw,
                        f->unit,
                        f->topic);
                break;

            case RPTT_ratio:
                snprintf(tmp, sizeof(tmp), 
                        "\t\t\t{\n"
                        "\t\t\t\t\"description\": \"%s\",\n"
                        "\t\t\t\t\"valueType\": \"ratio\",\n"
                        "\t\t\t\t\"unit\": \"%s\",\n"
                        "\t\t\t\t\"topic\": \"%s\",\n"
                        "\t\t\t},\n",
                        f->c.descRaw,
                        f->unit,
                        f->topic);
                break;

            default:
                break;
        }

        manifesto.append(tmp);
    }

    snprintf(tmp, sizeof(tmp), "\t\t],\n");
    manifesto.append(tmp);

    return result;
}
