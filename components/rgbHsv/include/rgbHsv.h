#define DEFAULT 0
#define FLASH 1
#define GLITCH 2
#define SWIPER 3
#define RAINBOW 4
#define RUN 5

typedef struct RgbColor{
//typedef struct RgbColor{
	unsigned char r;
	unsigned char g;
	unsigned char b;
} RgbColor;

typedef struct HsvColor {
//typedef struct RgbColor{
	unsigned char h;
	unsigned char s;
	unsigned char v;
} HsvColor;

RgbColor HsvToRgb(HsvColor hsv);
HsvColor RgbToHsv(RgbColor rgb);

void parseRGB(RgbColor *color, char* payload);
uint8_t modeToEnum(char* str);

uint8_t checkColorAndBright(RgbColor *currentRGB, RgbColor *targetRGB, int16_t *currentBright, int16_t *targetBright, int16_t fade_increment);