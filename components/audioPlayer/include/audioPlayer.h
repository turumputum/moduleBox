
void audioInit(void);
void audioPlay(char *cmd);
void audioStop(void);
void audioPause(void);
void setVolume_num(uint8_t vol);
void setVolume_str(char *cmd);
void listenListener(void *pvParameters);
void audioDeinit(void);
void fatFs_init(void);
void audioShift(char *cmd);
