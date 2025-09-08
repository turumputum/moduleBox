#include <stdint.h>

void start_startup_task(int slot_num);
void start_counter_task(int slot_num);
void start_timer_task(int slot_num);
void start_watchdog_task(int slot_num);
void start_whitelist_task(int slot_num);
void start_collector_task(int slot_num);
void start_tankControl_task(int slot_num);
void start_flywheel_task(int slot_num);
void start_scaler_task(int slot_num);
void start_stepper_conductor_task(int slot_num);
void start_random_task(int slot_num);