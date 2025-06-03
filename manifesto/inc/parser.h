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

#define MAX_PARAMS          200       


typedef enum
{
    PARAMTYPE_unknown,
    PARAMTYPE_int,
    PARAMTYPE_flag,
    PARAMTYPE_string,
    PARAMTYPE_float
} PARAMTYPE;


// ---------------------------------------------------------------------------
// --------------------------------- CLASSES ---------------------------------
// -----|---------------|-----------------------(|--------------|-------------

class Module
{
public:
        int             line;
        char            name                    [ 64 ];

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
}; 


class Parser
{
private:

        std::string     manifesto;

        char *          source;

        char *          searchEnd;

        int             numOfFuncs;

        Module          mod;

        Function        funcs                   [ MAX_PARAMS ];

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

        bool            extractParams           (Function &     func);

        bool            extractDesc             (Function &     func,
                                                 char *         on);

        bool            parseFunctionParams     (Function &     func,
                                                 char *         on);

        bool            generateManifestoForModule();

        const char *    generateManifesto       ();

        void            cleanValue              (char *         value);
        

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