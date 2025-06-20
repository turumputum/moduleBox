#include <stdint.h>
#include <rgbHsv.h>

#define AUDIO_PLAYER_MODULE 0
#define BUTTON_OPTORELAY_MODULE 1
#define BUTTON_LED_MODULE 2
#define INCREMENTAL_ENCODER_MODULE 3
#define MOSFET_MODULE 4


int init_slots(void);

int get_option_int_val(int slot_num, char* string, const char*  unit_name, int default_value, int min_value, int max_value);
float get_option_float_val(int slot_num, char* string, float default_value);
char* get_option_string_val(int slot_num, char* option, ...);
int get_option_enum_val(int slot_num, char* option, ...);
int get_option_flag_val(int slot_num, char* string);
int get_option_color_val(RgbColor * output, int slot_num, char* string, char * default_value);
//void get_option_string_val(int num_of_slot, char* option, char* custom_topic);
