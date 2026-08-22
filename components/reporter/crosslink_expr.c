// ***************************************************************************
// TITLE
//     Арифметика значения в правилах crosslink
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "crosslink_expr.h"

// ---------------------------------------------------------------------------
// -------------------------------- INTERNALS --------------------------------
// -----|-------------------|-------------------------------------------------

static const char * _skip_ws(const char *p)
{
    while ((*p == ' ') || (*p == '\t'))
        p++;

    return p;
}

/* Свой разбор числа вместо strtof: нужно запретить экспоненту и hex, иначе
   опечатка '@*1e3' тихо превратится в множитель 1000. */
static const char * _parse_num(const char *p, float *out, int *ok)
{
    int     neg   = 0;
    int     digits = 0;
    double  value = 0.0;

    *ok = 0;

    p = _skip_ws(p);

    if ((*p == '+') || (*p == '-'))
    {
        neg = (*p == '-');
        p++;
    }

    while ((*p >= '0') && (*p <= '9'))
    {
        value = value * 10.0 + (*p - '0');
        digits++;
        p++;
    }

    if (*p == '.')
    {
        double scale = 0.1;

        p++;

        while ((*p >= '0') && (*p <= '9'))
        {
            value += (*p - '0') * scale;
            scale *= 0.1;
            digits++;
            p++;
        }
    }

    if (!digits)
        return p;

    /* Хвост из букв - это не число: 'e' экспоненты, 'x' шестнадцатеричного
       литерала и просто мусор должны быть ошибкой, а не молчаливым обрезком. */
    if (((*p >= 'a') && (*p <= 'z')) || ((*p >= 'A') && (*p <= 'Z')))
        return p;

    *out = (float)(neg ? -value : value);
    *ok  = 1;

    return p;
}

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

int xl_expr_parse(const char *src, xl_expr_t *out)
{
    const char *    p;
    int             ok;

    if (!src || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    p = _skip_ws(src);

    if (!*p)
        return -1;

    if (*p == '@')
    {
        out->passthrough = 1;
        p++;
    }
    else
    {
        p = _parse_num(p, &out->constant, &ok);

        if (!ok)
            return -2;
    }

    while (1)
    {
        xl_op_kind_t    kind;
        float           operand;

        p = _skip_ws(p);

        if (!*p)
            break;

        switch (*p)
        {
            case '*': kind = XL_OP_MUL; break;
            case '/': kind = XL_OP_DIV; break;
            case '+': kind = XL_OP_ADD; break;
            case '-': kind = XL_OP_SUB; break;
            default:  return -2;
        }

        p++;

        p = _parse_num(p, &operand, &ok);

        if (!ok)
            return -2;

        /* Делитель известен на разборе - гонять проверку в рантайме незачем. */
        if ((kind == XL_OP_DIV) && (operand == 0.0f))
            return -3;

        if (out->count >= XL_MAX_OPS)
            return -4;

        out->op[out->count]      = kind;
        out->operand[out->count] = operand;
        out->count++;
    }

    return 0;
}

float xl_expr_eval(const xl_expr_t *e, float in)
{
    float acc;

    if (!e)
        return in;

    acc = e->passthrough ? in : e->constant;

    for (int i = 0; i < e->count; i++)
    {
        switch (e->op[i])
        {
            case XL_OP_MUL: acc *= e->operand[i]; break;
            case XL_OP_DIV: acc /= e->operand[i]; break;
            case XL_OP_ADD: acc += e->operand[i]; break;
            case XL_OP_SUB: acc -= e->operand[i]; break;
            default: break;
        }
    }

    return acc;
}

int xl_expr_is_passthrough(const xl_expr_t *e)
{
    return e && e->passthrough && (e->count == 0);
}

char * xl_expr_format(float value, char *buf, int size)
{
    /* Насыщение вместо переполнения: lroundf от inf или от 1e30 даёт мусор,
       а шаговик по мусорному заданию уезжает в упор. */
    if (value > 2147483000.0f)  value = 2147483000.0f;
    if (value < -2147483000.0f) value = -2147483000.0f;
    if (isnan(value))           value = 0.0f;

    snprintf(buf, size, "%ld", (long)lroundf(value));

    return buf;
}
