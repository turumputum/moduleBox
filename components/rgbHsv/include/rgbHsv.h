// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************


#ifndef __RGBHSV_H__
#define __RGBHSV_H__

#define MODE_DEFAULT 0
#define MODE_FLASH 1
#define MODE_GLITCH 2
#define MODE_SWIPER 3
#define MODE_RAINBOW 4
#define MODE_RUN 5

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

int parseRGB(RgbColor *color, char* payload);
uint8_t modeToEnum(char* str);

uint8_t checkColorAndBright(RgbColor *currentRGB, RgbColor *targetRGB, int16_t *currentBright, int16_t *targetBright, int16_t fade_increment);


#endif // #define __RGBHSV_H__
