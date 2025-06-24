// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef __STDREPORT_H__
#define __STDREPORT_H__

#include <stdint.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define MAX_NUM_OF_STDREPORTS   100

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef enum
{
    RPTT_string             = 0,
    RPTT_int,
    RPTT_float,
    RPTT_ratio
} RPTT;

typedef struct __tag_STDREPORT
{
    uint32_t                meaningful  : 1;
    uint32_t                slot_num    : 3;
    uint32_t                outType     : 3;

} STDREPORT, * PSTDREPORT;

typedef struct __tag_STDREPORTRATIO
{
    STDREPORT               rpt;

    float                   min;
    float                   max;

} STDREPORTRATIO, * PSTDREPORTRATIO;




// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

void                stdreport_initialize        ();

int                 stdreport_register          (RPTT               output_type,
                                                 int                slot_num, 
                                                 const char *       unit,
                                                 const char *       defaultTopic,
                                                 ...);



void                stdreport_i                 (int                reportRegId,
                                                 int                value);

void                stdreport_f                 (int                reportRegId,
                                                 float              value);

#endif // #define __STDREPORT_H__