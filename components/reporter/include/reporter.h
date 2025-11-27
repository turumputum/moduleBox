void reporter_init(void);
void report(char *msg, int num_of_slot);
void crosslinker_task(void *parameter);
void reportTaskList();
void reportNETstatus();
void reportFreeRAM();
void reportVersion();

//void startup_crosslinks_exec(void);
void crosslinks_process(char *crosslinks_str, char *event);
//void inbox_handler(char *msg);
