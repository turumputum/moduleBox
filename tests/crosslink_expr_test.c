/* Юнит-тест арифметики значения crosslink.
   Сборка: см. tests/Makefile, цель crosslink_expr_test */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../components/reporter/crosslink_expr.c"

static int failed = 0;
static int total  = 0;

/* Ожидаем успешный разбор и конкретный результат после форматирования. */
static void ok(const char *expr, float in, long want)
{
    xl_expr_t   e;
    char        buf[32];
    int         rc;

    total++;

    rc = xl_expr_parse(expr, &e);

    if (rc != 0)
    {
        printf("FAIL  '%s' <- %g : разбор вернул %d, ожидался успех\n", expr, in, rc);
        failed++;
        return;
    }

    xl_expr_format(xl_expr_eval(&e, in), buf, sizeof(buf));

    if (strtol(buf, NULL, 10) != want)
    {
        printf("FAIL  '%s' <- %g : получили %s, ожидали %ld\n", expr, in, buf, want);
        failed++;
        return;
    }

    printf("ok    '%-14s' <- %-8g -> %s\n", expr, in, buf);
}

/* Ожидаем ошибку разбора. */
static void bad(const char *expr, int wantRc)
{
    xl_expr_t   e;
    int         rc;

    total++;

    rc = xl_expr_parse(expr, &e);

    if (rc >= 0)
    {
        printf("FAIL  '%s' : разбор прошёл, ожидалась ошибка %d\n", expr, wantRc);
        failed++;
        return;
    }

    if (rc != wantRc)
    {
        printf("FAIL  '%s' : ошибка %d, ожидалась %d\n", expr, rc, wantRc);
        failed++;
        return;
    }

    printf("ok    '%-14s' -> ошибка %d\n", expr, rc);
}

int main(void)
{
    /* --- база из ТЗ --- */
    ok("@",             100,    100);
    ok("@*2.4",         100,    240);
    ok("@*2.4",         255,    612);
    ok("@/4",           100,    25);
    ok("@/4",           10,     3);        /* 2-5 округляется от нуля       */
    ok("@*-1",          255,    -255);
    ok("@ * 2.4",       100,    240);      /* пробелы вокруг оператора      */
    ok("@/16.38",       65535,  4001);
    ok("42",            999,    42);       /* литерал игнорирует событие    */

    /* --- цепочка: масштаб и смещение, ради чего всё затевалось --- */
    ok("@*3.8+1000",    0,      1000);
    ok("@*3.8+1000",    255,    1969);
    ok("@*3.8+1000",    128,    1486);
    ok("@/2-10",        100,    40);
    ok("@+10",          100,    110);
    ok("@-10",          100,    90);
    ok("@ * 3.8 + 1000", 255,   1969);     /* пробелы в цепочке             */

    /* --- порядок слева направо, без приоритета умножения --- */
    ok("@+10*2",        5,      30);       /* (5+10)*2, не 5+20             */

    /* --- отрицательные операнды --- */
    ok("@*-2+100",      10,     80);
    ok("@--10",         5,      15);       /* вычитание -10 это прибавление */

    /* --- округление --- */
    ok("@/3",           10,     3);        /* 3-33 вниз                     */
    ok("@/3",           11,     4);        /* 3-67 вверх                    */
    ok("@/4",           -10,    -3);       /* -2-5 от нуля                  */

    /* --- ошибки разбора --- */
    bad("@/0",          -3);
    bad("@*1e3",        -2);               /* экспонента запрещена          */
    bad("@*",           -2);
    bad("@*2,4",        -2);               /* запятая занята под правила    */
    bad("@%2",          -2);
    bad("",             -1);
    bad("@*2*2*2*2*2",  -4);               /* длиннее XL_MAX_OPS            */
    bad("abc",          -2);

    /* --- passthrough распознаётся --- */
    {
        xl_expr_t e;
        total++;
        if ((xl_expr_parse("@", &e) != 0) || !xl_expr_is_passthrough(&e)) {
            printf("FAIL  '@' не опознан как passthrough\n");
            failed++;
        } else {
            printf("ok    '@' opознан как passthrough\n");
        }

        total++;
        if ((xl_expr_parse("@*2", &e) != 0) || xl_expr_is_passthrough(&e)) {
            printf("FAIL  '@*2' ошибочно опознан как passthrough\n");
            failed++;
        } else {
            printf("ok    '@*2' не passthrough\n");
        }
    }

    printf("\n%d/%d passed, %d failed\n", total - failed, total, failed);

    return failed ? 1 : 0;
}
