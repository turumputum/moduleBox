#include <stdio.h>
#include <stdint.h>
#include "rgbHsv.h"
#include <string.h>
#include "sdkconfig.h"

const uint8_t gamma_8[256] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3,
		3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
		19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
		50, 51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68, 69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89, 90, 92, 93, 95, 96, 98, 99, 101, 102,
		104, 105, 107, 109, 110, 112, 114, 115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164,
		167, 169, 171, 173, 175, 177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213, 215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247,
		249, 252, 255 };

RgbColor HsvToRgb(HsvColor hsv) {
	RgbColor rgb;
	unsigned char region, remainder, p, q, t;

	if (hsv.s == 0) {
		rgb.r = hsv.v;
		rgb.g = hsv.v;
		rgb.b = hsv.v;
		return rgb;
	}

	region = hsv.h / 43;
	remainder = (hsv.h - (region * 43)) * 6;

	p = (hsv.v * (255 - hsv.s)) >> 8;
	q = (hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8;
	t = (hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8;

	switch (region) {
	case 0:
		rgb.r = hsv.v;
		rgb.g = t;
		rgb.b = p;
		break;
	case 1:
		rgb.r = q;
		rgb.g = hsv.v;
		rgb.b = p;
		break;
	case 2:
		rgb.r = p;
		rgb.g = hsv.v;
		rgb.b = t;
		break;
	case 3:
		rgb.r = p;
		rgb.g = q;
		rgb.b = hsv.v;
		break;
	case 4:
		rgb.r = t;
		rgb.g = p;
		rgb.b = hsv.v;
		break;
	default:
		rgb.r = hsv.v;
		rgb.g = p;
		rgb.b = q;
		break;
	}

	return rgb;
}
HsvColor RgbToHsv(RgbColor rgb) {
	HsvColor hsv;
	unsigned char rgbMin, rgbMax;

	rgbMin = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b) : (rgb.g < rgb.b ? rgb.g : rgb.b);
	rgbMax = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b) : (rgb.g > rgb.b ? rgb.g : rgb.b);

	hsv.v = rgbMax;
	if (hsv.v == 0) {
		hsv.h = 0;
		hsv.s = 0;
		return hsv;
	}

	hsv.s = 255 * (rgbMax - rgbMin) / hsv.v;
	if (hsv.s == 0) {
		hsv.h = 0;
		return hsv;
	}

	if (rgbMax == rgb.r)
		hsv.h = 0 + 43 * (rgb.g - rgb.b) / (rgbMax - rgbMin);
	else if (rgbMax == rgb.g)
		hsv.h = 85 + 43 * (rgb.b - rgb.r) / (rgbMax - rgbMin);
	else
		hsv.h = 171 + 43 * (rgb.r - rgb.g) / (rgbMax - rgbMin);

	return hsv;
}

void parseRGB(RgbColor *color, char* payload){
	//ESP_LOGD(TAG, "Set RGB for slot:%d val:%s",slot_num, payload);
	char *rest;
	char *tok;
    int R,G,B;
	if(strstr(payload, " ")!=NULL){
		tok = strtok_r(payload, " ", &rest);
		R = atoi(tok);
        if(strstr(rest, " ")!=NULL){
		    tok = strtok_r(NULL, " ", &rest);
        }else{
            tok = rest;
        }
		G = atoi(tok);
		B = atoi(rest);

		if(R > 255)R = 255;
        if(R < 0)R = 0;
		if(G > 255)G = 255;
        if(G < 0)G = 0;
		if(B > 255)B = 255;
        if(B < 0)B = 0;
        color->r=R;
        color->g=G;
        color->b=B;
        //ESP_LOGD(TAG, "Set RGB:%d %d %d", color->r, color->g, color->b);
	}
}

uint8_t modeToEnum(char* str){
    if(strstr(str, "flash")!=NULL){
        return FLASH; 
    }else if(strstr(str, "glitch")!=NULL){
        return GLITCH;
    }else if(strstr(str, "rainbow")!=NULL){
        return RAINBOW;
    }else{
        return DEFAULT;
    }
}


uint8_t checkColorAndBright(RgbColor *currentRGB, RgbColor *targetRGB, uint16_t *currentBright, uint16_t *targetBright, uint16_t fade_increment){
    uint8_t ret = 0;
	if(currentRGB!=targetRGB){
		ret=1;
        if(currentRGB->r < targetRGB->r){
            if((targetRGB->r - currentRGB->r) < fade_increment){
               currentRGB->r = targetRGB->r;
            }else{
                currentRGB->r += fade_increment;
            }
        }else if(currentRGB->r > targetRGB->r){
            if((currentRGB->r - targetRGB->r) < fade_increment){
               currentRGB->r = targetRGB->r;
            }else{
                currentRGB->r -= fade_increment;
            }
        }

        if(currentRGB->g < targetRGB->g){
            if((targetRGB->g - currentRGB->g) < fade_increment){
               currentRGB->g = targetRGB->g;
            }else{
                currentRGB->g += fade_increment;
            }
        }else if(currentRGB->g > targetRGB->g){
            if((currentRGB->g - targetRGB->g) < fade_increment){
               currentRGB->g = targetRGB->g;
            }else{
                currentRGB->g -= fade_increment;
            }
        }

        if(currentRGB->b < targetRGB->b){
            if((targetRGB->b - currentRGB->b) < fade_increment){
               currentRGB->b = targetRGB->b;
            }else{
                currentRGB->b += fade_increment;
            }
        }else if(currentRGB->b > targetRGB->b){
            if((currentRGB->b - targetRGB->b) < fade_increment){
               currentRGB->b = targetRGB->b;
            }else{
                currentRGB->b -= fade_increment;
            }
        }
    }
	if(*currentBright!=*targetBright){
		ret=1;
        if(*currentBright < *targetBright){
            if((*targetBright - *currentBright) < fade_increment){
              	*currentBright = *targetBright;
            }else{
                *currentBright += fade_increment;
            }
        }else if(*currentBright > *targetBright){
            if((*currentBright - *targetBright) < fade_increment){
               	*currentBright = *targetBright;
            }else{
                *currentBright -= fade_increment;
            }
        }
    }
	return ret;
}