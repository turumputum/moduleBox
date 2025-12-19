#ifndef ST7789_DISPLAY_H
#define ST7789_DISPLAY_H

#include "driver/spi_master.h"
#include "driver/gpio.h"
// Display dimensions
#define ST7789_WIDTH 240
#define ST7789_HEIGHT 240
// GPIO pins

#define PIN_SDA 18\r\n#define PIN_SCK 17\r\n#define PIN_DC  15\r\n#define PIN_RST 3\r\n#define PIN_BLK 8\r\n\r\n// ST7789 Commands\r\n#define ST7789_NOP     0x00\r\n#define ST7789_SWRESET 0x01\r\n#define ST7789_RDDID   0x04\r\n#define ST7789_RDDST   0x09\r\n#define ST7789_SLPIN   0x10\r\n#define ST7789_SLPOUT  0x11\r\n#define ST7789_PTLON   0x12\r\n#define ST7789_NORON   0x13\r\n#define ST7789_INVOFF  0x20\r\n#define ST7789_INVON   0x21\r\n#define ST7789_DISPOFF 0x28\r\n#define ST7789_DISPON  0x29\r\n#define ST7789_CASET   0x2A\r\n#define ST7789_RASET   0x2B\r\n#define ST7789_RAMWR   0x2C\r\n#define ST7789_RAMRD   0x2E\r\n#define ST7789_PTLAR   0x30\r\n#define ST7789_COLMOD  0x3A\r\n#define ST7789_MADCTL  0x36\r\n\r\n// Color definitions\r\n#define COLOR_BLACK   0x0000\r\n#define COLOR_WHITE   0xFFFF\r\n#define COLOR_RED     0xF800\r\n#define COLOR_GREEN   0x07E0\r\n#define COLOR_BLUE    0x001F\r\n#define COLOR_YELLOW  0xFFE0\r\n#define COLOR_CYAN    0x07FF\r\n#define COLOR_MAGENTA 0xF81F\r\n#define COLOR_ORANGE  0xFC00\r\n\r\n// Initialize the ST7789 display\r\nvoid st7789_init(void);\r\n\r\n// Send command to the display\r\nvoid st7789_send_cmd(uint8_t cmd);\r\n\r\n// Send data to the display\r\nvoid st7789_send_data(uint8_t data);\r\n\r\n// Fill the screen with a color\r\nvoid st7789_fill_screen(uint16_t color);\r\n\r\n// Draw a pixel at x, y with the given color\r\nvoid st7789_draw_pixel(int16_t x, int16_t y, uint16_t color);\r\n\r\n// Draw a rectangle filled with the given color\r\nvoid st7789_draw_filled_rectangle(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);\r\n\r\n// Draw an image from flash memory to the display\r\nvoid st7789_draw_image_from_flash(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* image_data);\r\n\r\n// Set brightness of backlight\r\nvoid st7789_set_backlight(bool enabled);\r\n\r\n#endif // ST7789_DISPLAY_H
