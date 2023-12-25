#include <stdio.h>

static const char* help = "\n\n[HELP] \n"
"\n;button_led, button_smartLed Standard module_mode for all slots \n"
"\n"
    ";button options: button_inverse, button_debounce_delay:val(default:0), button_topic:val(string)\n"
    ";button trigger: [device_name]/button_[num_of_slot]:val(0-1)\n"
"\n"
    ";led options: led_inverse, fade_increment:val(default:255), max_bright:val(default:255), min_bright:val(default:0), refreshRate_ms:val(default:30)\n"
    ";led options: ledMode:val(string:default), led_topic:val(string)\n"
    ";led action: [device_name]/led_[num_of_slot]:val(0-1)\n"
"\n"
    ";smartLed options: led_inverse, num_of_led:val(default:24), fade_increment:val(default:255), max_bright:val(default:255), min_bright:val(default:0)\n"
    ";smartLed options: RGB_color:val(string:R G B), ledMode:val(string:default/flash), refreshRate_ms:val(default:30), smartLed_topic:val(string)\n"
    ";smartLed action:[device_name]/smartLed_[num_of_slot]:val(0-1)\n"
    ";smartLed action:[device_name]/smartLed_[num_of_slot]/setRGB:val(string:R G B)\n"
    ";smartLed action:[device_name]/smartLed_[num_of_slot]/setMode:val(string:default/flash)\n"
    ";smartLed action:[device_name]/smartLed_[num_of_slot]/setFadeIncrement:val\n"
"\n"
    ";pwmRGBled options: fade_increment:val(default:255), max_bright:val(default:255), min_bright:val(default:0)\n"
    ";pwmRGBled options: RGB_color:val(string:R G B), ledMode:val(string:default/flash), refreshRate_ms:val(default:30), pwmRGBled_topic:val(string)\n"
    ";pwmRGBled action:[device_name]/pwmRGBled_[num_of_slot]:val(0-1)\n"
    ";pwmRGBled action:[device_name]/pwmRGBled_[num_of_slot]/setRGB:val(string:R G B)\n"
    ";pwmRGBled action:[device_name]/pwmRGBled_[num_of_slot]/setMode:val(string:default/flash)\n"
    ";pwmRGBled action:[device_name]/pwmRGBled_[num_of_slot]/setFadeIncrement:val\n"
"\n"
    ";sensor_2ch Standard module_mode for all slots \n"
    ";sensor_2ch options: ch_1_inverse, ch_2_inverse, sens_debounce_delay:val(int:default-0), mode:val(string:independent/OR_logic/AND_logic), sens_topic:val(string)\n"
    ";sensor_2ch trigger:[device_name]/sens_[num_of_slot]:val(0-1)\n"
            "\t\t\t\t\t[device_name]/sens_[num_of_slot]/ch_1/2:val(0-1)\n"
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
    ";audio_player_mono action: [device_name]/player_0/play:val(num, +num, -num, random, #-current)\n"
                    "\t\t\t\t\t\t[device_name]/player_0/shift:val(num, +num, -num, random)\n"
                    "\t\t\t\t\t\t[device_name]/player_0/stop:val(need, but ignored)\n"
                    "\t\t\t\t\t\t[device_name]/player_0/setVolume:val(num, +num, -num)\n"
    ";audio_player_mono trigger: [device_name]/player_0/endOfTrack:val(num_of_track)\n"
"\n"
    ";stepper module_mode Special for SLOT_0:  \n"
    ";stepper module_options: dir_inverse, max_speed:(stepPerMin),acceleration:(stepPerMin^2), max_pos:, sensor_up_inverse, sensor_down_inverse, sensor_slot:, sensor_num:, stop_on_sensor, float_report \n"
    ";stepper action: [device_name]/stepper_moveTo:val(steps or float) \n"            
        "\t\t\t\t\t\t[device_name]/stepper_setSpeed:val(stepPerMin)\n"
        "\t\t\t\t\t\t[device_name]/stepper_stop:val(need, but ignored)\n"
        "\t\t\t\t\t\t[device_name]/stepper_stop_on_sensor:val(need, but ignored)\n"
        "\t\t\t\t\t\t[device_name]/stepper_set_currentPos:val\n"
    ";stepper trigger: [device_name]/stepper_[num_of_slot]:val(current pos)\n";