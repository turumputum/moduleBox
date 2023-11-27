#include "esp_err.h"

void audioInit(uint8_t slot_num);
esp_err_t audioPlay(uint8_t truckNum);
void audioStop(void);
void audioPause(void);
void setVolume_num(uint8_t vol);
void setVolume_str(char *cmd);
void listenListener(void *pvParameters);
void audioDeinit(void);
void fatFs_init(void);
void audioShift(char *cmd);
