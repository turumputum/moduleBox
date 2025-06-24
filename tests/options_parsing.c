#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>


#define ESP_LOGW printf("\n"); fprintf
#define ESP_LOGD printf("\n"); fprintf
#define ESP_LOGE printf("\n"); fprintf
#define TAG stdout

#define ESP_FAIL -1
#define ESP_OK 0

typedef struct RgbColor{
//typedef struct RgbColor{
	unsigned char r;
	unsigned char g;
	unsigned char b;
} RgbColor;

const char * me_config_slot_options = "floatOutput,maxVal:4090,minVal:1,inverse,filterK:2.1,deadBand:20,periodic:8,dividerMode:3V3 ,";

int get_option_flag_val(int slot_num, const char* string)
{
	int  	result = 0;

	if (strstr((char*)me_config_slot_options, string) != NULL)
	{
		result = 1;
	}

	return result;
}
int get_option_int_val(int slot_num, const char* string, const char*  unit_name, int default_value, int min_value, int max_value)
{
	int 		result	= default_value;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr((char*)me_config_slot_options, string)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			result = atoi(value);
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s\n", dup);
		}
	}

	return result;
}
float get_option_float_val(int slot_num, const char* string, float default_value)
{
	float 		result	= default_value;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr((char*)me_config_slot_options, string)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			result = atof(value);
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}

	return result;
}
static char * _cleanValue(char *         value)
{
	char * 			result;
    unsigned char * on 		= (unsigned char *)value;

    while (*on && ((*on <= ' ') || (*on == '\"')))
    {
        on++;
    }

	result 	= (char*)on;
    int len = strlen(result);

    if (len)
    {
        result[len] = 0;
        
        on = (unsigned char*)&result[len - 1];
        
        // Clean end
        while ((on > (unsigned char*)result) && ((*on <= ' ') || (*on == '\"')))
        {
            on--;
        }
        
        *(on + 1) = 0;
    }

	return result;
}
int get_option_enum_val(int slot_num, const char* option, ...)
{
	int 			result			= -1;
    va_list         list;
	char * 			arg;
	char *			value;
	char *			begin;
	char *			end;
	int 			len;

    va_start(list, option);

	if ((begin = strstr((char*)me_config_slot_options, option)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(dup, ':')) != NULL)
		{
			value++;
			value = _cleanValue(value);

			if (strlen(value) > 0)
			{
				for (int idx = 0; ((result < 0) && ((arg = va_arg(list, char *)) != NULL)); idx++)
				{
					if (!strcmp(arg, value))
					{
						result = idx;
					}
				}
			}
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}
	else
		result = 0;

	va_end(list);

	return result;
}
int parseRGB(RgbColor *color, char* payload)
{
	int 		result = -1;

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

		result = 0;
        //ESP_LOGD(TAG, "Set RGB:%d %d %d", color->r, color->g, color->b);
	}

	return result;
}
int get_option_color_val(RgbColor * output, int slot_num, const char* string, char * default_value)
{
	int 		result	= ESP_FAIL;
	char *		begin;
	char *		value;
	char *		end;
	int 		len;

	if ((begin = strstr((char*)me_config_slot_options, string)) != NULL)
	{
		if ((end = strchr(begin, ',')) == NULL)
		{ end = begin + strlen(begin); }
		len = end - begin;
		char dup [len + 1];
		memcpy(dup, begin, len);
		dup[len] = 0;

		if ((value = strchr(begin, ':')) != NULL)
		{
			value++;

			if (parseRGB(output, value) == -1)
			{
				ESP_LOGW(TAG, "Color wrong format:%s", dup);
			}
		}
		else
		{
			ESP_LOGW(TAG, "Options wrong format:%s", dup);
		}
	}

	if (result != ESP_OK)
	{
		parseRGB(output, default_value);
	}
	
	return result;
}


int main()
{
    int slot_num = 1;

	/* Флаг определяет формат воводящего значения, 
		если указан - будет выводиться значение с плавающей точкой,
		иначе - целочисленное
	*/
	int flag_float_output = get_option_flag_val(slot_num, "floatOutput");
	ESP_LOGD(TAG, "S%d: Set float output = %d", slot_num, flag_float_output);

	/* Определяет верхний порог значений */
	int MAX_VAL = get_option_int_val(slot_num, "maxVal", "", 4095, 0, 4095);
	ESP_LOGD(TAG, "S%d: Set max_val:%d\n", slot_num, MAX_VAL);

	/* Определяет нижний порог значений */
	int MIN_VAL = get_option_int_val(slot_num, "minVal", "", 0, 0, 4095);
	ESP_LOGD(TAG, "S%d: Set min_val:%d", slot_num, MIN_VAL);

	/* Флаг задаёт инвертирование значений */
	int inverse = get_option_flag_val(slot_num, "inverse");
	ESP_LOGD(TAG, "S:%d: Set inverse:%d", slot_num, inverse);
   
	/* Коэфициент фильтрации */
	float k = get_option_float_val(slot_num, "filterK", 1);
	ESP_LOGD(TAG, "S:%d: Set k filter:%f", slot_num, k);
    
	/* Фильтрация дребезга - определяет порог срабатывания */
	int dead_band = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4095);
	ESP_LOGD(TAG, "S:%d: Set dead_band:%d", slot_num, dead_band);

	/* Задаёт периодичночть отсчётов в миллисекундах */
	int periodic = get_option_int_val(slot_num, "periodic", "", 0, 0, 4095);
	ESP_LOGD(TAG, "S:%d: Set periodic:%d", slot_num, periodic);

	int divider;
	/* Задаёт режим делителя */
	if ((divider = get_option_enum_val(slot_num, "dividerMode", "5V", "3V3", "10V", NULL)) < 0)
	{
		ESP_LOGE(TAG, "S:%d: dividerMode: unricognized value", slot_num);

		divider = 0;
	}

	ESP_LOGD(TAG, "S:%d: Set divider:%d", slot_num, divider);

return 0;
}

