
#include <arsenal.h>
#include <axstring.h>

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


