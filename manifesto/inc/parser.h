// ***************************************************************************
// TITLE
//
// PROJECT
//     manifesto
// ***************************************************************************


#ifndef __PARSER_H__
#define __PARSER_H__

#include <stdbool.h>
#include <string>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define nil         (0)

#define MAX_PARAMS              200       
#define DEF_MAX_ENUMS           32

typedef enum
{
    RPTT_unknown,
    RPTT_int,
    RPTT_float,
    RPTT_string,
    RPTT_ratio,
} RPTT;

typedef enum
{
    OPTTYPE_unknown,
    OPTTYPE_int,
    OPTTYPE_flag,
    OPTTYPE_string,
    OPTTYPE_float,
    OPTTYPE_enum,
    OPTTYPE_color,
} OPTTYPE;

typedef enum
{
    PARAMT_none = 0,
    PARAMT_int,
    PARAMT_float,
    PARAMT_string
} PARAMT;



typedef struct __tag_Common
{
        int             line;
        char *          descRaw;
        char            name                    [ 64 ];
        char            funcName                [ 64 ];

} Common;


// ---------------------------------------------------------------------------
// --------------------------------- CLASSES ---------------------------------
// -----|---------------|-----------------------(|--------------|-------------

class Module
{
public:
        int             line;
        char            name                    [ 64 ];

        char *          begin;

        char *          descRaw;
}; 

class Option
{
public:
        Common          c;
        OPTTYPE         type;
        char            name                    [ 64 ];
        char            unit                    [ 32 ];

        char *          paramsRaw;

        int             defaultVal;
        int             maxVal;
        int             minVal;

        float           defaultValF;
        float           maxValF;
        float           minValF;

        char            defaultValStr           [ 256 ];

        int             enumsCount;

        char *          enums                   [ DEF_MAX_ENUMS ];
};  

class Report
{
public:
        Common          c;
        RPTT            type;
        char            unit                    [ 32 ];

        char *          paramsRaw;

        int             maxVal;
        int             minVal;

        float           maxValF;
        float           minValF;
};  

class Command
{
public:
        Common          c;
        char            name                    [ 32 ];

        char *          paramsRaw;

        int             paramsCount;
        PARAMT          params                  [ DEF_MAX_ENUMS ];
};  


class Parser
{
private:

        std::string     manifesto;

        char *          source;

        char *          funcSearchEnd;
        char *          modSearchEnd;

        int             numOfOpts;
        int             numOfReps;
        int             numOfCmds;

        Module          mod;

        Option          opts                   [ MAX_PARAMS ];
        Report          reps                   [ MAX_PARAMS ];
        Command         cmds                   [ MAX_PARAMS ];

        char *          backstrstrGlobal        (char *         haystack,
                                                 const char *   needle);

        char *          backstrstr              (char *         haystack,
                                                 const char *   needle);

        void            strdup                  (char * &       dest,
                                                 const char *   begin);

        void            copyFromTo              (char *         dest, 
                                                 char *         begin,
                                                 char *         end);

        int             dupFromTo               (char * &       dest, 
                                                 char *         begin,
                                                 char *         end);

        void            resetSearching          ();

        char *          findNextFunction        (Common &       c,
                                                 const char *   part1,
                                                 const char *   part2);

        bool            getOptions              ();

        bool            extractOptIntParam      (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptFloatParam    (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptFlagParam     (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptStringParam   (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptEnumParam     (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptColorParam    (Option &       opt,
                                                 int            idx,
                                                 char *         value);

        bool            extractOptParams        (Option &       opt);

        bool            extractCommonDesc       (Common &       c,
                                                 char *         opt);

        bool            extractOptDesc          (Option &       opt,
                                                 char *         on);

        bool            parseOptiosParams       (Option &       opt,
                                                 char *         on);

        bool            generateManifestoForModule();

        bool            generateManifestoOfOptions();

        bool            generateManifestoOfReports();
        bool            generateManifestoOfCommands();

        void            cleanValue              (char *         value);

        void            resetOptions            (bool           first);

        void            resetReports            (bool           first);

        void            resetCommands           (bool           first);

        int             findNextModule          ();

        char *          findCorellatedCurlyBrace(char *         begin);

        bool            getModuleName           (char *         begin);

        bool            getModuleDesc           (char *         begin);


        bool            getReports              ();

        bool            getCommands             ();


        bool            parseReportParams       (Report &       rep,
                                                 char *         repot);

        bool            extractRepParams        (Report &       rep);

        bool            extractRepParam         (Report &       rep,
                                                 int            idx,
                                                 char *         value);

        bool            parseCommandParams      (Command &      cmd,
                                                 char *         command);

        bool            extractCmdParams        (Command &      cmd);

        PARAMT          extractCmdParamType     (char *         value);

public:
                        Parser                  ()
                        {
                            numOfOpts = 0;
                        }

        const char *    parse                   (char *         source);


};

// ---------------------------------------------------------------------------
// -------------------------------- OptionS --------------------------------
// -----------------|---------------------------(|------------------|---------

#endif // #define __PARSER_H__