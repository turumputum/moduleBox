void testStepper();



void start_stepper_task(int num_of_slot);


void stepper_set_targetPos(char* str);
void stepper_set_currentPos(int32_t val);
void stepper_set_speed(int32_t speed);

void stepper_stop_on_sensor(int8_t val);
void stepper_stop(void);