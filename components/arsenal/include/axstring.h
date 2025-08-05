
#ifndef __AXSTRINGH__                            
#define __AXSTRINGH__

#include <stdarg.h>

#include <arsenal.h>


// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define AXSLUT_FLAG_CASESENS    1

#define LOCHAR(a)       ((a) | 0x20)

#define HEX2NIBBLE(a)   if (((a) >= '0') && ((a) <= '9')) { (a) -= '0'; }    \
                        else { (a) |= 0x20; if (((a) >= 'a') && ((a) <= 'f'))\
                        { (a) -= ('a' - 10); } else { (a) = -1; } }

#define NIBBLE2HEX(a)   if (a < 0xA) (a) += '0'; else (a) += ('A' - 10);

#define SUBSTRS_SEPISFIELD       1               
#define SUBSTRS_COPY             2


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

#ifdef __cplusplus
extern "C" {
#endif                        

BOOL                strz_is_ip                  (PCSTR              psz_str,
                                                 INT                octets);

long                strz_to_bytes               (PCSTR              psz_string);

#ifdef __cplusplus
}
#endif                                      //  #ifdef __cplusplus


#endif // #ifndef __AXSTRINGH__                            