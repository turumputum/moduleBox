
#include <arsenal.h>
#include <axstring.h>
#include <string.h>

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// ---------------------------------------------------------------------------
        
typedef struct __ATOLPFX
{
    long        l_mp;
    PSTR        psz_perfix;
} PREFIX;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// ***************************************************************************
// FUNCTION
//      _cmp
// TYPE
//      static
// PURPOSE
//
// PARAMETERS
//      PU8   p_src1PU8     p_src2 --
// RESULT
//      BOOL --
// ***************************************************************************
static  BOOL _cmp(PVOID     p_src1,
                  PVOID     p_src2)
{
    BOOL            b_result        = false;
    PU8             p_on1;
    PU8             p_on2;

    ENTER(true);

    for (  p_on1 = p_src1, p_on2 = p_src2, b_result = true;
           b_result && *p_on1;
           p_on1++, p_on2++)
    {
        if (*p_on2)
        {
            if ((*p_on1 | 0x20) != (*p_on2 | 0x20))
            {
                b_result = false;
            }
        }
        else
        {
            if (*p_on1 > ' ')
            {
                b_result = false;
            }
            break;
        }
    }
        
    RETURN(b_result);
}       


BOOL strz_is_ip(PCSTR   psz_str,
                INT     octets)
{
    BOOL            b_result        = false;
    PCSTR           psz_on;
    INT             found           = 1;
    UINT            num             = AXII;

    ENTER(true);

    psz_on      = psz_str;
    b_result    = true;

    while (b_result && *psz_on)
    {
        if ((*psz_on >= '0') && (*psz_on <= '9'))
        {
            if (num == AXII)
                num = 0;

            num = (num * 10) + (*psz_on - '0');
        }
        else
        {
            if ((*psz_on == '.') && (num < 256))
            {
                num = AXII;
                found++;
            }
            else
                b_result = false;
        }

        psz_on++;
    }

    if (  b_result                                  &&
          ((found != octets) || (num > 255)  )   )
    {
        b_result = false;
    }

    RETURN(b_result);
}

// ***************************************************************************
// FUNCTION
//      _strz_to_postfix
// TYPE
//      static
// PURPOSE
//
// PARAMETERS
//      PSTR       psz_string --
//      PREFIX *   ast_pfx    --
// RESULT
//      long long --
// ***************************************************************************
static long _strz_to_postfix(long *       pl_mp,
                             PCSTR        psz_string,
                             PREFIX *     ast_pfx)
{
    long        l_value         = 0;
    PU8         p_on;
    UINT        i_idx;

    ENTER(true);

    // Leading spaces
    for (  p_on = (PU8)psz_string;
           *p_on && (*p_on <= ' ');
           p_on++)
    {
        ;
    }
    
    // Numeric part
    for ( ; *p_on && ((*p_on >= '0') && (*p_on <= '9') ); p_on++)
    {
        l_value = (l_value * 10) + (*p_on - '0');
    }
    
    // Middle spaces
    for ( ; *p_on && (*p_on <= ' '); p_on++)
    {
        ;
    }
    
    // Prefix

    if (*p_on)
    {
        for (i_idx = 0; !*pl_mp && ast_pfx[i_idx].l_mp; i_idx++)
        {
            if (_cmp(p_on, ast_pfx[i_idx].psz_perfix))
            {
                *pl_mp = ast_pfx[i_idx].l_mp;
            }
        }
    }
    else
        *pl_mp = 1;

    RETURN(l_value);
}


// ***************************************************************************
// FUNCTION
//      strz_to_bytes
// PURPOSE
//
// PARAMETERS
//      PSTR   psz_string --
// RESULT
//      long long --
// ***************************************************************************
long strz_to_bytes(PCSTR psz_string)
{
    long l_result          = 0;
    long l_mp                   = 0;

    PREFIX      ast_pfx         [] =
    {
        {  1024,        "k"      },
        {  1024,        "kb"     },
        {  1024,        "kib"    },
           
        {  1048576,     "m"      },
        {  1048576,     "mb"     },
        {  1048576,     "mib"    },

        {  1073741824,  "g"      },
        {  1073741824,  "gb"     },
        {  1073741824,  "gib"    },

        {  0,           ""    }
    };

    ENTER(psz_string);

    l_result = _strz_to_postfix(&l_mp, psz_string, ast_pfx);

    RETURN(l_result * l_mp);
}
// ***************************************************************************
// FUNCTION
//      strz_cpy
// PURPOSE
//
// PARAMETERS
//      PSTR    psz_target --
//      PCSTR   psz_source --
//      UINT    u_size     --
// RESULT
//      PSTR  --
// ***************************************************************************
PSTR strz_cpy(PSTR      psz_target,
              PCSTR     psz_source,
              UINT      u_size)
{
    PSTR        psz_result      = nil;
    PSTR        psz_on          = (PSTR)psz_source;
    PSTR        psz_on_tgt      = psz_target;
    UINT        u_left          = u_size;

    ENTER(psz_on_tgt && psz_on && u_left);

    while ((--u_left) && *psz_on)
    {
        *(psz_on_tgt++) = *(psz_on++);
    }

    *psz_on_tgt = 0;
    psz_result  = psz_on_tgt;

    RETURN(psz_result);
}
// ***************************************************************************
// FUNCTION
//      strz_notspace
// PURPOSE
//
// PARAMETERS
//      PCSTR psz_string    --
//      UINT  len           --
//      BOOL  b_stop_at_eol --
// RESULT
//      PCSTR --
// ***************************************************************************
PCSTR strz_notspace(PCSTR         psz_string,
                    UINT         len,
                    BOOL         b_stop_at_eol)
{
    PCSTR           result   = psz_string;
    UINT            left            = len;

    ENTER(result);

    while (  *result                                &&
             left--                                 &&
             (*(PU8)result <= ' ')                  &&
             (!b_stop_at_eol || (*result != '\n'))  )
    {
        result++;
    }
    
    RETURN(result);
}
// ***************************************************************************
// FUNCTION
//      strz_clean
// PURPOSE
//
// PARAMETERS
//      PSTR   psz_string --
// RESULT
//      PSTR  --
// ***************************************************************************
PSTR strz_clean(PSTR psz_string)
{
    PU8          p_result        = nil;
    PU8          p_on;
    UINT         u_len;

    ENTER(psz_string);

    p_result    = (PU8)strz_notspace(psz_string, -1, false);
    p_on        = p_result;

    if (*p_on && ((u_len = strlen((PSTR)p_on)) > 0))
    {
        p_on       += u_len;

        while (  (p_on != p_result)     &&
                 (*(p_on - 1) <= ' ')   )
        {
            p_on--;
        }

        *p_on = 0;
    }

    RETURN((PSTR)p_result);
}
// ***************************************************************************
// FUNCTION
//      strz_substrs
// PURPOSE
//
// PARAMETERS
//      PSTR   psz_tgt        --
//      UINT   u_tgt          --
//      PSTR   psz_src        --
//      UINT   u_src          --
//      CHAR   c_sep          --
//      BOOL   b_sep_is_field --
// RESULT
//      PSTR  --
// ***************************************************************************
PSTR strz_substrs_get_u(PSTR    str,
                        PUINT   plen,
                        CHAR    sep)
{
    PSTR            psz_result      = nil;
    PSTR            on;
    UINT            left;

    ENTER(str && plen);

    if (*plen)
    {
        psz_result  = str;
        on          = str;
        left        = *plen;

        while (left)
        {
            if (*on)
            {
                if (*on == sep)
                {
                    *on = 0;
                    break;
                }
    
                on++;
                left--;
            }
            else
            {
                psz_result  = (++on);
                *(plen)     = (--left);
            }
        }

        if (!left)
        {
            *(plen) = 0;
        }
    }

    RETURN(psz_result);
}


