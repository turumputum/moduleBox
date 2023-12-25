#include <stdint.h>

void start_pwm_ch(void);

void init_3n_mosfet(int slot_num);
void setRGB(int slot_num, char* payload);
void setGlitch(int slot_num, char* payload);
