void reporter_init(void);
void report(char *msg, int num_of_slot);
void report_retain(const char *topic, const char *payload);
void crosslinker_task(void *parameter);
void reportTaskList();
void reportNETstatus();
void reportFreeRAM();
void reportFreeDisk();
void reportVersion();

/* Periodic diagnostic snapshot — publishes <deviceName>/system/diag
   with a single-line JSON: uptime, heap (free/min/largest/internal/spiram),
   MQTT counters (connect/disconnect/pub/data/err + last-pub-age),
   open-socket count, FreeRTOS task count. */
void reportSystemDiag(void);

//void startup_crosslinks_exec(void);
void crosslinks_process(char *crosslinks_str, char *event);
//void inbox_handler(char *msg);
