#include "st7789_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

static const char* TAG = "ST7789";

// SPI handle
spi_device_handle_t spi_handle;

// Low-level functions
static void st7789_spi_write_bytes(const uint8_t* data, size_t len) {
    esp_err_t ret;
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void*)0;
    
    ret = spi_device_transmit(spi_handle, &t);
    ESP_ERROR_CHECK(ret);
}

void st7789_send_cmd(uint8_t cmd) {
    gpio_set_level(PIN_DC, 0); // Command mode
    st7789_spi_write_bytes(&cmd, 1);
}

void st7789_send_data(uint8_t data) {
    gpio_set_level(PIN_DC, 1); // Data mode
    st7789_spi_write_bytes(&data, 1);
}

static void st7789_send_data_array(const uint8_t* data, size_t length) {
    gpio_set_level(PIN_DC, 1); // Data mode
    st7789_spi_write_bytes(data, length);
}

void st7789_set_window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    // Column Address Set
    st7789_send_cmd(ST7789_CASET);  
    st7789_send_data(x0 >> 8);
    st7789_send_data(x0 & 0xFF);
    st7789_send_data(x1 >> 8);
    st7789_send_data(x1 & 0xFF);

    // Row Address Set
    st7789_send_cmd(ST7789_RASET);
    st7789_send_data(y0 >> 8);
    st7789_send_data(y0 & 0xFF);
    st7789_send_data(y1 >> 8);
    st7789_send_data(y1 & 0xFF);

    // Memory write
    st7789_send_cmd(ST7789_RAMWR);
}

void st7789_init(void) {
    // Initialize GPIOs
    gpio_pad_select_gpio(PIN_DC);
    gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT);
    
    gpio_pad_select_gpio(PIN_RST);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    
    gpio_pad_select_gpio(PIN_BLK);
    gpio_set_direction(PIN_BLK, GPIO_MODE_OUTPUT);
    
    // Hardware reset
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // Configure SPI
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,  // Not using MISO
        .mosi_io_num = PIN_SDA,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  // 40 MHz
        .mode = 0,
        .spics_io_num = -1,  // We handle CS manually
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle));
    
    // Initialization sequence
    st7789_send_cmd(ST7789_SWRESET);
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    st7789_send_cmd(ST7789_SLPOUT);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    st7789_send_cmd(ST7789_COLMOD);
    st7789_send_data(0x05);  // 16-bit color
    
    st7789_send_cmd(ST7789_MADCTL);
    st7789_send_data(0x00);  // RGB order
    
    st7789_send_cmd(ST7789_CASET);
    st7789_send_data(0x00);
    st7789_send_data(0x00);
    st7789_send_data(0x00);
    st7789_send_data(0xEF);  // 239
    
    st7789_send_cmd(ST7789_RASET);
    st7789_send_data(0x00);
    st7789_send_data(0x00);
    st7789_send_data(0x00);
    st7789_send_data(0xEF);  // 239
    
    st7789_send_cmd(ST7789_INVON);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    st7789_send_cmd(ST7789_NORON);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    st7789_send_cmd(ST7789_DISPON);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    
    ESP_LOGI(TAG, "ST7789 initialized successfully");
    
    st7789_fill_screen(COLOR_BLACK);
    
    // Turn on backlight
    st7789_set_backlight(true);
}

void st7789_fill_screen(uint16_t color) {
    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);
    
    // Prepare color data
    uint8_t color_bytes[2] = {(uint8_t)(color >> 8), (uint8_t)(color & 0xFF)};
    
    gpio_set_level(PIN_DC, 1); // Data mode
    
    // Send the same color for each pixel
    for (int i = 0; i < ST7789_WIDTH * ST7789_HEIGHT; i++) {
        st7789_spi_write_bytes(color_bytes, 2);
    }
}

void st7789_draw_pixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= ST7789_WIDTH || y < 0 || y >= ST7789_HEIGHT) {
        return;  // Out of bounds
    }
    
    st7789_set_window(x, y, x, y);
    
    uint8_t color_bytes[2] = {(uint8_t)(color >> 8), (uint8_t)(color & 0xFF)};
    st7789_spi_write_bytes(color_bytes, 2);
}

void st7789_draw_filled_rectangle(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // Check bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7789_WIDTH)  w = ST7789_WIDTH - x;
    if (y + h > ST7789_HEIGHT) h = ST7789_HEIGHT - y;
    
    if (w <= 0 || h <= 0) return;
    
    st7789_set_window(x, y, x + w - 1, y + h - 1);
    
    // Prepare color data buffer
    const size_t pixels_to_draw = w * h;
    const size_t max_buffer_size = 256;  // Limit buffer size
    const size_t pixels_per_buffer = max_buffer_size / 2;
    
    if (pixels_to_draw <= pixels_per_buffer) {
        // Small enough to fit in buffer
        uint8_t* color_buffer = malloc(pixels_to_draw * 2);
        if (color_buffer != NULL) {
            for (size_t i = 0; i < pixels_to_draw; i++) {
                color_buffer[i * 2] = (uint8_t)(color >> 8);
                color_buffer[i * 2 + 1] = (uint8_t)(color & 0xFF);
            }
            
            gpio_set_level(PIN_DC, 1); // Data mode
            st7789_spi_write_bytes(color_buffer, pixels_to_draw * 2);
            free(color_buffer);
        }
    } else {
        // Too big, draw in chunks
        uint8_t* color_buffer = malloc(max_buffer_size);
        if (color_buffer != NULL) {
            // Fill buffer with the color
            for (size_t i = 0; i < pixels_per_buffer; i++) {
                color_buffer[i * 2] = (uint8_t)(color >> 8);
                color_buffer[i * 2 + 1] = (uint8_t)(color & 0xFF);
            }
            
            size_t remaining_pixels = pixels_to_draw;
            gpio_set_level(PIN_DC, 1); // Data mode
            
            while (remaining_pixels > 0) {
                size_t pixels_to_send = (remaining_pixels < pixels_per_buffer) ? 
                                        remaining_pixels : pixels_per_buffer;
                
                st7789_spi_write_bytes(color_buffer, pixels_to_send * 2);
                remaining_pixels -= pixels_to_send;
            }
            
            free(color_buffer);
        }
    }
}

void st7789_draw_image_from_flash(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* image_data) {
    // Check bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7789_WIDTH)  w = ST7789_WIDTH - x;
    if (y + h > ST7789_HEIGHT) h = ST7789_HEIGHT - y;
    
    if (w <= 0 || h <= 0 || image_data == NULL) return;
    
    st7789_set_window(x, y, x + w - 1, y + h - 1);
    
    // Prepare buffer for sending image data
    const size_t pixels_to_draw = w * h;
    const size_t max_buffer_size = 1024;  // Adjust based on available memory
    const size_t pixels_per_buffer = max_buffer_size / 2;
    
    uint8_t* buffer = heap_caps_malloc(max_buffer_size, MALLOC_CAP_DMA);
    if (buffer == NULL) {
        buffer = malloc(max_buffer_size);
    }
    
    if (buffer != NULL) {
        size_t total_pixels_sent = 0;
        gpio_set_level(PIN_DC, 1); // Data mode
        
        while (total_pixels_sent < pixels_to_draw) {
            size_t pixels_to_process = (pixels_to_draw - total_pixels_sent < pixels_per_buffer) ? 
                                      (pixels_to_draw - total_pixels_sent) : pixels_per_buffer;
            
            // Copy pixel data to buffer (converting from 16-bit to bytes)
            for (size_t i = 0; i < pixels_to_process; i++) {
                uint16_t pixel_color = image_data[total_pixels_sent + i];
                buffer[i * 2] = (uint8_t)(pixel_color >> 8);
                buffer[i * 2 + 1] = (uint8_t)(pixel_color & 0xFF);
            }
            
            st7789_spi_write_bytes(buffer, pixels_to_process * 2);
            total_pixels_sent += pixels_to_process;
        }
        
        if (buffer) {
            if (heap_caps_get_free_size(MALLOC_CAP_DMA)) {
                free(buffer);
            } else {
                heap_caps_free(buffer);
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to allocate buffer for image drawing");
    }
}

void st7789_set_backlight(bool enabled) {
    gpio_set_level(PIN_BLK, enabled ? 1 : 0);
    ESP_LOGI(TAG, "Backlight %s", enabled ? "ON" : "OFF");
}

void start_st7789_display_task(int slot_num) {
    ESP_LOGI(TAG, "Initializing ST7789 Display on slot %d", slot_num);
    
    // Initialize the ST7789 display
    st7789_init();
    
    // Example: Fill screen with black
    st7789_fill_screen(COLOR_BLACK);
    
    // Example: Draw some test patterns
    st7789_draw_filled_rectangle(10, 10, 50, 50, COLOR_RED);
    st7789_draw_filled_rectangle(70, 10, 50, 50, COLOR_GREEN);
    st7789_draw_filled_rectangle(130, 10, 50, 50, COLOR_BLUE);
    
    ESP_LOGI(TAG, "ST7789 Display initialized on slot %d", slot_num);
}

