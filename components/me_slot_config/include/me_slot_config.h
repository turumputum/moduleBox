#include <stdint.h>
#define AUDIO_PLAYER_MODULE 0
#define BUTTON_OPTORELAY_MODULE 1
#define BUTTON_LED_MODULE 2
#define INCREMENTAL_ENCODER_MODULE 3
#define MOSFET_MODULE 4


int init_slots(void);

int get_option_int_val(int num_of_slot, char* string);
float get_option_float_val(int num_of_slot, char* string);
char* get_option_string_val(int num_of_slot, char* option);
//void get_option_string_val(int num_of_slot, char* option, char* custom_topic);