// ***************************************************************************
// TITLE
//      retractor - мотор Steadywin GIM8108-8 по CAN, режим скорости
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef RETRACTOR_H
#define RETRACTOR_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "driver/twai.h"
#include "esp_err.h"

// ---------------------------------------------------------------------------
// ----------------------- Протокол ODrive CANSimple 0.6 ---------------------
// ---------------------------------------------------------------------------
// Драйвер Steadywin с прошивкой 0.6.x (в Motor Wizard: CAN Protocol SIMPLE).
// ID кадра = (node_id << 5) | cmd, стандартные кадры, порядок байт LSB, float -
// IEEE. Запрос Get_ сообщения - RTR-кадр с тем же ID. Heartbeat и оценки
// энкодера драйвер шлёт сам (по умолчанию раз в 100 и 10 мс).

#define OD_CMD_GET_VERSION          0x000   // протокол, железо, прошивка - по байту
#define OD_CMD_HEARTBEAT            0x001   // u32 ошибки оси, u8 состояние, u8 результат процедуры
#define OD_CMD_GET_ERROR            0x003   // u32 активные ошибки, u32 причина отключения
#define OD_CMD_SET_AXIS_STATE       0x007   // u32 требуемое состояние
#define OD_CMD_ENCODER_ESTIMATES    0x009   // float позиция, обороты; float скорость, об/с
#define OD_CMD_SET_CONTROLLER_MODE  0x00B   // u32 режим регулятора, u32 режим входа
#define OD_CMD_SET_INPUT_VEL        0x00D   // float скорость, об/с; float момент упреждения, Нм
#define OD_CMD_SET_LIMITS           0x00F   // float предел скорости, об/с; float предел тока, А
#define OD_CMD_BUS_VOLTAGE_CURRENT  0x017   // float напряжение, В; float ток, А
#define OD_CMD_CLEAR_ERRORS         0x018   // u8 identify
#define OD_CMD_SET_VEL_GAINS        0x01B   // float Kp скорости; float Ki скорости

#define OD_AXIS_STATE_IDLE          1
#define OD_AXIS_STATE_CLOSED_LOOP   8
#define OD_CONTROL_MODE_VELOCITY    2
#define OD_INPUT_MODE_PASSTHROUGH   1

// ---------------------------------------------------------------------------
// --------------------------------- CAN шина --------------------------------
// ---------------------------------------------------------------------------
// TWAI-контроллер у ESP32-S3 один: на устройстве возможна одна CAN-шина.
// Все устройства на ней (мотор, в будущем абсолютный энкодер крайних
// положений) обслуживаются одной задачей retractor - кадры разбираются по ID.

typedef enum {
    RCAN_BITRATE_500K = 0,
    RCAN_BITRATE_1M,
    RCAN_BITRATE_250K,
    RCAN_BITRATE_125K,
    RCAN_BITRATE_100K,
} rcan_bitrate_t;

esp_err_t   rcan_init               (int tx_pin, int rx_pin, rcan_bitrate_t bitrate);
esp_err_t   rcan_send               (uint32_t id, const uint8_t *data, uint8_t len);
esp_err_t   rcan_send_rtr           (uint32_t id, uint8_t len);
/* Следит за bus-off: запускает восстановление и перезапускает контроллер.
   Возвращает 1 если шина сейчас не в рабочем состоянии */
int         rcan_check_bus          (void);
/* Состояние контроллера и счётчики ошибок в лог - для диагностики связи */
void        rcan_log_status         (const char *prefix);

// ---------------------------------------------------------------------------
// --------------------------------- ODrive ----------------------------------
// ---------------------------------------------------------------------------

esp_err_t   od_request              (uint8_t node, uint8_t cmd);
esp_err_t   od_set_axis_state       (uint8_t node, uint32_t state);
esp_err_t   od_set_controller_mode  (uint8_t node, uint32_t control_mode, uint32_t input_mode);
esp_err_t   od_set_input_vel        (uint8_t node, float vel);
esp_err_t   od_set_limits           (uint8_t node, float vel_limit, float current_limit);
esp_err_t   od_clear_errors         (uint8_t node);
esp_err_t   od_set_vel_gains        (uint8_t node, float kp, float ki);
/* Кадр от узла node: возвращает номер команды, иначе -1 */
int         od_frame_cmd            (uint8_t node, const twai_message_t *msg);
uint32_t    od_get_u32              (const twai_message_t *msg, int offset);
float       od_get_float            (const twai_message_t *msg, int offset);
const char *od_error_name           (uint32_t errors);

// ---------------------------------------------------------------------------
// -------------------------------- Запуск -----------------------------------
// ---------------------------------------------------------------------------

void start_retractor_task(int slot_num);

#endif // RETRACTOR_H
