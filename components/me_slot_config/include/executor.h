typedef struct {
	char str[200];
	size_t length;
} exec_message_t;

void execute(char *action);

void exec_optorelay(int slot_num, int payload);
void exec_led(int slot_num, int payload);

void init_led(int slot_num);
void init_optorelay(int slot_num);
void init_3n_mosfet(int slot_num);
