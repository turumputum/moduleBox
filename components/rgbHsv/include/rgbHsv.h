#define DEFAULT 0
#define FLASH 1
#define GLITCH 2

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

RgbColor HsvToRgb(HsvColor hsv);
HsvColor RgbToHsv(RgbColor rgb);

void parseRGB(RgbColor *color, char* payload);
uint8_t modeToEnum(char* str);

void checkColorAndBright(RgbColor *currentRGB, RgbColor *targetRGB, uint16_t *currentBright, uint16_t *targetBright, uint16_t fade_increment);