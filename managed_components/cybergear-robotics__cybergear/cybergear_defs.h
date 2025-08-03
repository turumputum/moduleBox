#ifndef CYBERGEAR_DEFS_H
#define CYBERGEAR_DEFS_H

#define CMD_POSITION                  0x1
#define CMD_REQUEST                   0x2
#define CMD_ENABLE                    0x3
#define CMD_RESET                     0x4
#define CMD_SET_MECH_POSITION_TO_ZERO 0x6
#define CMD_SET_CAN_ID                0x7
#define CMD_RAM_READ                 0x11
#define CMD_RAM_WRITE                0x12
#define CMD_GET_STATUS               0x15
#define CMD_GET_MOTOR_FAIL              21

#define ADDR_SPEED_KP              0x2014
#define ADDR_SPEED_KI              0x2015
#define ADDR_POSITION_KP           0x2016
#define ADDR_RUN_MODE              0x7005
#define ADDR_I_REF                 0x7006
#define ADDR_SPEED_REF             0x700A
#define ADDR_LIMIT_TORQUE          0x700B
#define ADDR_CURRENT_KP            0x7010
#define ADDR_CURRENT_KI            0x7011
#define ADDR_CURRENT_FILTER_GAIN   0x7014
#define ADDR_POSITION_REF          0x7016
#define ADDR_LIMIT_SPEED           0x7017
#define ADDR_LIMIT_CURRENT         0x7018

#define ADDR_IQ_REF 0x7006
#define ADDR_LOC_REF 0x7016
#define ADDR_MECH_POS 0x7019
#define ADDR_IQF 0x701A
#define ADDR_MECH_VEL 0x701B
#define ADDR_VBUS 0x701C
#define ADDR_ROTATION 0x701D
#define ADDR_LOC_KP 0x701E
#define ADDR_SPD_KP 0x701F
#define ADDR_SPD_KI 0x7020

#define POS_MIN                   -12.5f
#define POS_MAX                    12.5f
#define V_MIN                     -30.0f
#define V_MAX                      30.0f
#define KP_MIN                      0.0f
#define KP_MAX                    500.0f
#define KI_MIN                      0.0f
#define KI_MAX                     10.0f
#define KD_MIN                      0.0f
#define KD_MAX                      5.0f
#define T_MIN                     -12.0f
#define T_MAX                      12.0f
#define I_MIN                     -27.0f
#define I_MAX                      27.0f
#define CURRENT_FILTER_GAIN_MIN     0.0f
#define CURRENT_FILTER_GAIN_MAX     1.0f

#define RET_CYBERGEAR_OK              0x00
#define RET_CYBERGEAR_MSG_NOT_AVAIL   0x01
#define RET_CYBERGEAR_INVALID_CAN_ID  0x02
#define RET_CYBERGEAR_INVALID_PACKET  0x03

typedef enum
{
    CYBERGEAR_MODE_MOTION  = 0x00,
    CYBERGEAR_MODE_POSITION = 0x01,
    CYBERGEAR_MODE_SPEED = 0x02,
    CYBERGEAR_MODE_CURRENT = 0x03
} cybergear_mode_e;

typedef enum
{
    CYBERGEAR_STATE_RESET = 0x00,
    CYBERGEAR_STATE_CALIBRATION = 0x01,
    CYBERGEAR_STATE_RUNNING = 0x02,
} cybergear_state_e;

#endif // !CYBERGEAR_DEFS_H
