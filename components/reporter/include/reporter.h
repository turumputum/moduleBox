void reporter_init(void);
void report(char *msg, int num_of_slot);
void crosslinker_task(void *parameter);
void reportTaskList();
void reportNETstatus();
void reportFreeRAM();
void reportFreeDisk();
void reportVersion();

/* Periodic diagnostic snapshot — writes JSON line to /sdcard/log.txt via mblog.
   Content: uptime, heap (free/min/largest/internal/spiram), MQTT counters
   (connect/disconnect/pub/data/err + last-event ages), open-socket count,
   FreeRTOS task count. Does NOT touch MQTT/OSC/UDP. */
void reportSystemDiag(void);

/* Dumps FreeRTOS task list with stack high-water marks to /sdcard/log.txt
   via mblog. Heavier than reportSystemDiag (~2 KB). */
void logTaskList(void);

//void startup_crosslinks_exec(void);
void crosslinks_process(char *crosslinks_str, char *event);
//void inbox_handler(char *msg);
