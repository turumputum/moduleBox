#include <stdint.h>

typedef struct RgbColor {
	unsigned char r;
	unsigned char g;
	unsigned char b;
} RgbColor;

typedef struct HsvColor {
	unsigned char h;
	unsigned char s;
	unsigned char v;
} HsvColor;

void start_smartLed_task(int slot_num);