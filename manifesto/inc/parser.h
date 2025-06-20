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
    PARAMTYPE_unknown,
    PARAMTYPE_int,
    PARAMTYPE_flag,
    PARAMTYPE_string,
    PARAMTYPE_float,
    PARAMTYPE_enum,
    PARAMTYPE_color,
} PARAMTYPE;


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

class Function
{
public:
        PARAMTYPE       type;
        int             line;
        char            funcName                [ 64 ];
        char            name                    [ 64 ];
        char            unit                    [ 32 ];

        char *          paramsRaw;
        char *          descRaw;

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


class Parser
{
private:

        std::string     manifesto;

        char *          source;

        char *          funcSearchEnd;
        char *          modSearchEnd;

        int             numOfFuncs;

        Module          mod;

        Function        funcs                   [ MAX_PARAMS ];

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

        char *          findNextFunction        (Function &     func,
                                                 const char *   part1,
                                                 const char *   part2);

        bool            getOptions              ();

        bool            extractIntParam         (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractFloatParam       (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractFlagParam        (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractStringParam      (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractEnumParam        (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractColorParam       (Function &     func,
                                                 int            idx,
                                                 char *         value);

        bool            extractParams           (Function &     func);

        bool            extractDesc             (Function &     func,
                                                 char *         on);

        bool            parseFunctionParams     (Function &     func,
                                                 char *         on);

        bool            generateManifestoForModule();

        void            cleanValue              (char *         value);

        void            resetFunctions          (bool           first);

        int             findNextModule          ();

        char *          findCorellatedCurlyBrace(char *         begin);

        bool            getModuleName           (char *         begin);

        bool            getModuleDesc           (char *         begin);

public:
                        Parser                  ()
                        {
                            numOfFuncs = 0;
                        }

        const char *    parse                   (char *         source);


};

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

#endif // #define __PARSER_H__