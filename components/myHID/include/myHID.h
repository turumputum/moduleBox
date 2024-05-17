#include <stdint.h>

void send_hid_key_press(uint8_t key);
void send_hid_key_release(uint8_t key);

void start_HID_task(int slot_num);