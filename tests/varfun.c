#include<stdio.h>
#include<stdarg.h>
#include<stdio.h>

#define print_numbers(a,b,c,...)               \
do {                                            \
    int parameters[] = {__VA_ARGS__};           \
    int count = sizeof(parameters)/sizeof(int); \
                                                \
    for (int i = 0; i < count; i++)             \
        printf("%d", parameters[i]);            \
    printf("\n");                                \
} while(0)

int main()
{
    print_numbers(0, 0, 0, 1, 2, 3, 4, 5, 6);
    print_numbers(0, 0, 0, 7);
    print_numbers(0, 0, 0, 8, 9, 10);
}
