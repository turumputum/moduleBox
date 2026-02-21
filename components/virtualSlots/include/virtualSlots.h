// ***************************************************************************
// TITLE: Virtual Slots Component
//
// PROJECT: moduleBox
// ***************************************************************************

#ifndef VIRTUALSLOTS_H
#define VIRTUALSLOTS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// ----------------------------- FUNCTION PROTOTYPES -------------------------
// ---------------------------------------------------------------------------

// Startup module
void start_startup_task(int slot_num);

// Counter module
void start_counter_task(int slot_num);

// Random module
void start_random_task(int slot_num);

// Timer module
void start_timer_task(int slot_num);

// Flywheel module
void start_flywheel_task(int slot_num);

// Watchdog module
void start_watchdog_task(int slot_num);

// Whitelist module
void start_whitelist_task(int slot_num);

// Collector module
void start_collector_task(int slot_num);

// Scaler module
void start_scaler_task(int slot_num);

// Tank Control module
void start_tankControl_task(int slot_num);

// Stepper Conductor module
void start_conductor_task(int slot_num);

#endif // VIRTUALSLOTS_H
