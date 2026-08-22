

#gcc -g stdcommands_unit.c -o .build/stdcommands_unit

all: mblog crosslink_expr_test

mblog:
	gcc -g mblog.c -o .build/mblog

crosslink_expr_test:
	gcc -g -Wall -I../components/reporter/include crosslink_expr_test.c -lm -o .build/crosslink_expr_test
	./.build/crosslink_expr_test

