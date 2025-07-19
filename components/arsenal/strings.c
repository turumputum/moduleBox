
#include <arsenal.h>
#include <axstring.h>

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



