#define MAX_STRING_LENGTH 255

typedef struct {
	char str[MAX_STRING_LENGTH];
} exec_message_t;

typedef struct{
	char str[MAX_STRING_LENGTH];
	int  slot_num;
} command_message_t;


void executer_task(void);
void execute(char *action);

void exec_optorelay(int slot_num, int payload);
void exec_led(int slot_num, int payload);

void init_led(int slot_num);
void init_optorelay(int slot_num);
void init_3n_mosfet(int slot_num);
