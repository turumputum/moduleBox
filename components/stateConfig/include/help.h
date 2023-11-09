#include <stdio.h>

static const char* help = "\n\n[HELP] \n"
"\n;button_optorelay, button_led Standard module_mode for all slots \n"
"\n"
    ";button options: button_inverse\n"
    ";button trigger: [device_name]/button_[num_of_slot]:val(0-1)\n"
"\n"
    ";optorelay options: optorelay_default_high=val(0-1), optorelay_inverse=val(0-1)\n"
    ";optorelay action: [device_name]/optorelay_[num_of_slot]:val(0-1)\n"
"\n"
    ";led options: led_default_high=val(0-1), led_inverse=val(0-1)\n"
    ";led action: [device_name]/led_[num_of_slot]:val(0-1)\n"
"\n"
    ";Standard module_mode: 3n_mosfet \n"
"\n"
    ";encoder_inc Standard module_mode for all slots \n"
    ";encoder_inc options:inverse, absolute, custom_topic\n"
    ";encoder_inc trigger: [device_name]/encoder_[num_of_slot]:val(+1/-1)\n"
                "\t\t\t\t\t[device_name]/encoder_[num_of_slot]:val\n"
"\n"
    ";encoderPWM Standard module_mode for all slots \n"
    ";encoderPWM options: num_of_pos:val, incremental||absolute\n"
    ";encoderPWM trigger: [device_name]/encoderInc_[num_of_slot]:val(+1/-1)\n"
                "\t\t\t\t\t[device_name]/encoderAbs_[num_of_slot]:val(0-num_of_pos)\n"
"\n"
    ";tahometer, value:RPM||boolean Standard module_mode for all slots \n"
    ";tahometer options: threshold:val(boolean output), inverse, custom_topic\n"
    ";tahometer trigger: [device_name]/tahometer_[num_of_slot]:val\n"
"\n"
    ";analog, 12bit ADC, value:(0-4095)||(0.0-1.0) module_mode for 0,2,3,4 slots \n"
    ";analog options:dead_band, float_output, max_val, min_val(only in float mode), inverse, filter_k(filtring depth, float), custom_topic\n"
    ";analog trigger: [device_name]/analog_[num_of_slot]:val\n"
"\n"
    ";audio_player_mono module_mode Special for SLOT_0:  \n"
    ";audio_player_mono module_options: volume:val, delay:val\n"
    ";audio_player_mono action: [device_name]/player_play:val(num, +num, -num, random, #-current)\n"
                    "\t\t\t\t\t\t[device_name]/player_shift:val(num, +num, -num, random)\n"
                    "\t\t\t\t\t\t[device_name]/player_stop:val(need, but ignored)\n"
                    "\t\t\t\t\t\t[device_name]/player_pause:val(need, but ignored)\n"
                    "\t\t\t\t\t\t[device_name]/player_volume:val(num, +num, -num)\n"
    ";audio_player_mono trigger: [device_name]/player_start:val(num_of_track, *-any)\n"
                    "\t\t\t\t\t\t[device_name]/player_end:val(num_of_track, *-any)\n"
"\n"
    ";stepper module_mode Special for SLOT_0:  \n"
    ";stepper module_options: dir_inverse, max_speed:(stepPerMin),acceleration:(stepPerMin^2), max_pos:, sensor_up_inverse, sensor_down_inverse, sensor_slot:, sensor_num:, stop_on_sensor, float_report \n"
    ";stepper action: [device_name]/stepper_moveTo:val(steps or float) \n"            
        "\t\t\t\t\t\t[device_name]/stepper_setSpeed:val(stepPerMin)\n"
        "\t\t\t\t\t\t[device_name]/stepper_stop:val(need, but ignored)\n"
        "\t\t\t\t\t\t[device_name]/stepper_stop_on_sensor:val(need, but ignored)\n"
        "\t\t\t\t\t\t[device_name]/stepper_set_currentPos:val\n"
    ";stepper trigger: [device_name]/stepper_[num_of_slot]:val(current pos)\n";