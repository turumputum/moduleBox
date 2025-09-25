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

void Parser::resetCommands(bool first)
{
    resetCommon(first);

    for (int i = 0; i < int(sizeof(cmds) / sizeof(Command)); i++)
    {
        *cmds[i].c.funcName = 0;
        cmds[i].c.line = 0;
        *cmds[i].c.name = 0;

        if (!first)
        {
            if (!cmds[i].paramsRaw)
                free(cmds[i].paramsRaw);

            if (!cmds[i].c.descRaw)
                free(cmds[i].c.descRaw);
        }

        cmds[i].paramsRaw = 0;
        cmds[i].c.descRaw = 0;

        cmds[i].paramsCount = 0;
    }
    
    numOfCmds = 0;
}
PARAMT  Parser::extractCmdParamType(char *         value)
{
    PARAMT          result      = PARAMT_unknown;

    if (!strcmp(value, "PARAMT_int"))
    {
        result = PARAMT_int;
    }
    else if (!strcmp(value, "PARAMT_float"))
    {
        result = PARAMT_float;
    }
    else if (!strcmp(value, "PARAMT_string"))
    {
        result = PARAMT_string;
    }
    else if (!strcmp(value, "PARAMT_none"))
    {
        result = PARAMT_none;
    }

    return result;
}
bool Parser::extractCmdParams(Command &     cmd)
{
    bool        result  = true;
    char *      begin   = cmd.paramsRaw;
    int         idx     = 0;
    char *      end;
    char        tmp     [ 1024 ];

    cmd.paramsCount = 0;

    while (result && ((end = strchr(begin, ',')) != nil))
    {
        copyFromTo(tmp, begin, end);

        cleanValue(tmp);

        //printf("cmd param %d = '%s'\n", idx, tmp);

        switch (idx)
        {
            case 0: // pointer to COMMAND structure
                break;

            case 1: // local int ID of command
                break;

            case 2:
                strcpy(cmd.c.name, tmp);
                break;                

            default:
                if ((cmd.params[cmd.paramsCount] = (char*)extractCmdParamType(tmp)) == (char*)PARAMT_unknown)
                {
                    //printf("error: unknown command parameter type: %s\n", tmp);
                    result = false;
                }
                else
                    cmd.paramsCount++;
                break;
        }

        begin = end + 1;
        idx++;
    }

    return result;
}
bool Parser::extractCmdParamsEnum(Command &     cmd)
{
    bool        result  = true;
    char *      begin   = cmd.paramsRaw;
    int         idx     = 0;
    char *      end;
    char        tmp     [ 1024 ];

    cmd.paramsCount = 0;

    while (result && ((end = strchr(begin, ',')) != nil))
    {
        copyFromTo(tmp, begin, end);

        cleanValue(tmp);

        //printf("cmd param enum %d = '%s'\n", idx, tmp);

        switch (idx)
        {
            case 0: // pinter to COMMAND structure
                break;

            case 1: // local int ID of command
                break;

            case 2:
                strcpy(cmd.c.name, tmp);
                break;                

            default:
                strdup(cmd.params[cmd.paramsCount], tmp);

                if (cmd.params[cmd.paramsCount] == NULL)
                {
                    printf("error: cannot dup command enum parameter: %s\n", tmp);
                    result = false;
                }
                else
                    cmd.paramsCount++;
                break;
        }

        begin = end + 1;
        idx++;
    }

    return result;
}
bool Parser::parseCommandParams(Command &     cmd,
                               char *       command)
{
    bool        result = false;
    char *      params;
    char *      end;

    if ((params = strchr(command, '(')) != nil)
    {
//        printf("parseOptiosParams stage %d\n", stage);

        params++;
        if ((end = strchr(params, ')')) != nil)
        {
            if (dupFromTo(cmd.paramsRaw, params, end) > 0)
            {
                int len = strlen(cmd.paramsRaw);
                cmd.paramsRaw[ len + 0 ] = ',';
                cmd.paramsRaw[ len + 1 ] = 0;

                if (extractCmdParams(cmd))
                {
                    if (extractCommonDesc(cmd.c, command))
                    {
                        result = true;
                    }
                    else
                        printf("error: cannot extract desc for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
                }
                else
                    printf("error: cannot extract params for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
            }
            else
                printf("error: cannot get params body for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
        }
        else
            printf("error: cannot get params end for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
    }
    else
        printf("error: cannot get params begin for cmd %s(%d)\n", cmd.c.name, cmd.c.line);

    return result;
}
bool Parser::parseCommandParamsEnum(Command &     cmd,
                                    char *       command)
{
    bool        result = false;
    char *      params;
    char *      end;

    if ((params = strchr(command, '(')) != nil)
    {
//        printf("parseCommandParamsEnum stage %d\n", stage);

        params++;
        if ((end = strchr(params, ')')) != nil)
        {
            if (dupFromTo(cmd.paramsRaw, params, end) > 0)
            {
                int len = strlen(cmd.paramsRaw);
                cmd.paramsRaw[ len + 0 ] = ',';
                cmd.paramsRaw[ len + 1 ] = 0;

                if (extractCmdParamsEnum(cmd))
                {
                    if (extractCommonDesc(cmd.c, command))
                    {
                        result = true;
                    }
                    else
                        printf("error: cannot extract desc for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
                }
                else
                    printf("error: cannot extract params for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
            }
            else
                printf("error: cannot get params body for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
        }
        else
            printf("error: cannot get params end for cmd %s(%d)\n", cmd.c.name, cmd.c.line);
    }
    else
        printf("error: cannot get params begin for cmd %s(%d)\n", cmd.c.name, cmd.c.line);

    return result;
}
bool Parser::getCommands()
{
    bool        result = true;

    char * on;

    resetCommands(false);

    while (result && ((on = findNextFunction(cmds[numOfCmds].c, "stdcommand_register", "")) != nil))
    {
        if (!strcmp(cmds[numOfCmds].c.funcName, "stdcommand_register_enum"))
        {
            cmds[numOfCmds].type = PARAMT_enum;

            if (!parseCommandParamsEnum(cmds[numOfCmds], on))
            {
                result = false;
            }
            else if (++numOfCmds >= MAX_PARAMS)
            {
                printf("error: number of parameters exeed possible number (%d)\n", MAX_PARAMS);

                result = false;
            }
        }
        else 
        {
            cmds[numOfCmds].type = PARAMT_none;

            if (!parseCommandParams(cmds[numOfCmds], on))
            {
                result = false;
            }
            else if (++numOfCmds >= MAX_PARAMS)
            {
                printf("error: number of parameters exeed possible number (%d)\n", MAX_PARAMS);

                result = false;
            }
        }
    }

    return result;
}
bool Parser::generateManifestoOfCommands()
{
    bool            result      = true;
    Command *        f;
    char            tmp     [ 1024 ];

    snprintf(tmp, sizeof(tmp), "%s", "\t\t\"commands\": [\n");
    manifesto.append(tmp);

    for (int i = 0; i < numOfCmds; i++)
    {
        f = &cmds[i];

        snprintf(tmp, sizeof(tmp), 
                "\t\t\t{\n"
                "\t\t\t\t\"command\": \"%s\",\n"
                "\t\t\t\t\"description\": \"%s\",\n"
                "\t\t\t\t\"parametersType\": \"%s\",\n"
                "\t\t\t\t\"parameters\": [ ",
                f->c.name,
                f->c.descRaw,
                (f->type == PARAMT_enum) ? "enum" : "types");

        manifesto.append(tmp);

        if (f->paramsCount > 0)
        {
            if (f->type == PARAMT_enum)
            {
                for (int i = 0; i < f->paramsCount; i++)
                {
                    snprintf(tmp, sizeof(tmp), "\"%s\"%s ", 
                                f->params[i], 
                                ((i + 1) == f->paramsCount) ? "" : ",");

                    manifesto.append(tmp);
                }
            }
            else
            {
                for (int i = 0; i < f->paramsCount; i++)
                {
                    long t = (long)f->params[i];
                    const char * curr;

                    switch ((PARAMT)t)
                    {
                        case PARAMT_int:
                            curr = "int ";
                            break;
                    
                        case PARAMT_float:
                            curr = "float ";
                            break;
                    
                        case PARAMT_string:
                            curr = "string ";
                            break;
                    
                        case PARAMT_none:
                            curr = " ";
                            break;
                    
                        default:
                            curr = "unknown";
                            break;
                    }

                    snprintf(tmp, sizeof(tmp), "\"%s\"%s ", 
                                curr, 
                                ((i + 1) == f->paramsCount) ? "" : ",");

                    manifesto.append(tmp);
                }
            }
        }

        manifesto.append("]\n\t\t\t},\n");
    }

    snprintf(tmp, sizeof(tmp), "\t\t],\n");

    manifesto.append(tmp);

    return result;
}
